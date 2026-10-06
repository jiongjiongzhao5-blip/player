# M12 门面层详解：`MediaPlayer`

> 本文档对应学习工程 `learn/` 的第 12 个模块（阶段五"Qt 集成"的第一件），
> 也是**整个项目第一次和 Qt 打交道**。
> 代码位置：`learn/src/mediaplayer.h/.cpp`
> 测试位置：`learn/tests/test_mediaplayer.cpp`（7 组测试，16 项断言）

---

## 目录

- [一、模块定位](#一模块定位)
- [二、功能](#二功能)
  - [2.1 它解决什么问题：语义鸿沟](#21-它解决什么问题语义鸿沟)
  - [2.2 `PlaybackState` 状态机](#22-playbackstate-状态机)
  - [2.3 信号与槽](#23-信号与槽)
  - [2.4 三条线程边界](#24-三条线程边界)
- [三、具体实现](#三具体实现)
  - [3.1 ★★ `teardown()`：线程生命周期](#31-teardown线程生命周期)
  - [3.2 ★★ `onKernelFrame()`：AVFrame → QImage](#32-onkernelframeavframe--qimage)
  - [3.3 ★★ `eventLoop()`：显式切线程](#33-eventloop显式切线程)
  - [3.4 `onPositionTick()`：GUI 线程的定时器](#34-onpositiontickgui-线程的定时器)
  - [3.5 状态机与动作](#35-状态机与动作)
- [四、要注意的问题](#四要注意的问题)
  - [★★ 坑 1：`play()` 里的线程生命周期竞态](#-坑-1play-里的线程生命周期竞态)
  - [★★ 坑 2：`connect` 三参数 vs 四参数](#-坑-2connect-三参数-vs-四参数)
  - [★ 坑 3：`image.copy()` 为什么必须](#-坑-3imagecopy-为什么必须)
  - [★ 坑 4：`convertMtx_` 其实不解决任何问题](#-坑-4convertmtx_-其实不解决任何问题)
  - [坑 5：Debug 版 Qt 的 DLL 需要 PATH](#坑-5debug-版-qt-的-dll-需要-path)
  - [坑 6：`qDebug` 在 Windows 上走 `OutputDebugString`](#坑-6qdebug-在-windows-上走-outputdebugstring)
- [五、实测验证数据](#五实测验证数据)
- [六、踩坑清单汇总](#六踩坑清单汇总)
- [七、面试常见追问](#七面试常见追问)

---

## 一、模块定位

前 11 轮写出来的 `FFPlayer` **一行 Qt 代码都没有**。它的接口只有两个东西：

| 出口 | 形式 |
|---|---|
| 帧回调 | `std::function<void(const Frame&)>` |
| 事件队列 | `MessageQueue`（`post` 进去就不管了，**没有返回值**） |

**而 Qt 程序员的世界是信号槽**——声明式、跨线程安全、可多播。

**两者的语义模型完全不同，中间必须有一个翻译官。**

### ★ 这一层不含任何播放逻辑

**它不决定什么时候显示哪一帧、不算同步、不碰 FFmpeg。** 它只做三件事：

1. 把内核的**消息**翻译成 Qt **信号**；
2. 把内核丢出来的 **`AVFrame`** 翻译成 **`QImage`**；
3. 把 Qt 侧的**调用**转发成内核的**方法调用**。

**判断这一层写得好不好，标准就一条：能不能把它删掉换成另一个 UI 框架（比如 MFC），而内核一行不改。**

---

## 二、功能

### 2.1 它解决什么问题：语义鸿沟

**为什么不让内核直接发 Qt 信号？**

那样内核就依赖 Qt 了。而"**`FFPlayer` 与 Qt 完全解耦**"是本项目的核心设计目标之一——**只有这样，M8~M11 四轮才能脱离 GUI 单独测试内核**（命令行跑播放器）。

而且 Qt 信号需要 `QObject` 作为发送者，**内核对象不是 `QObject`，也不该是**。

### 2.2 `PlaybackState` 状态机

```cpp
enum class PlaybackState {
    Idle,          // 未加载
    Preparing,     // 已调用 prepare，等内核的 Prepared 事件
    Playing,
    Paused,
    Completed,     // 播放到末尾（注意：是"播完"，不是"读完"）
    Error
};
```

**状态迁移图：**

```
        ┌──── play() ────┐
        ▼                │
      Idle               │
        │ play()         │
        ▼                │
   Preparing ──prepared──> Playing ⇄ Paused
        │                     │         │
        │ error               │ completed
        ▼                     ▼
      Error                Completed
                              │ play()
                              └──> Preparing（重新开始）
```

**谁驱动迁移：**

| 迁移 | 由谁触发 | 在哪条线程 |
|---|---|---|
| `→ Preparing` | `play()` 调用 | GUI |
| `Preparing → Playing` | 内核 `Prepared` 事件 | **事件线程 → 切到 GUI** |
| `Playing ⇄ Paused` | `pause()` / `resume()` | GUI |
| `→ Completed` | 内核 `Completed` 事件 | **事件线程 → 切到 GUI** |
| `→ Error` | 内核 `Error` 事件 | **事件线程 → 切到 GUI** |
| `→ Idle` | `stop()` | GUI |

**★ 注意"内核事件驱动的迁移"都要跨线程**——这就是 `invokeMethod` 存在的原因（见 3.3）。

### 2.3 信号与槽

**7 个信号**（内核 → UI）：

| 信号 | 载荷 | 来自哪 |
|---|---|---|
| `prepared()` | — | 内核 `Prepared` |
| `completed()` | — | 内核 `Completed` |
| `errorOccurred(QString)` | 可读错误文字 | 内核 `Error`（**错误码转文字在这一层做**） |
| `frameReady(QImage)` | **视频帧** | 内核帧回调 |
| `positionChanged(qint64)` | 毫秒 | `QTimer` 200ms |
| `durationChanged(qint64)` | 毫秒 | 内核 `durationMs()` |
| `stateChanged(PlaybackState)` | 状态 | `setState()` |

**8 个公开槽**（UI → 内核）：

```cpp
void play();                 // 统一入口：空闲/完成态→准备并播放，暂停态→恢复
void togglePlayPause();      // 一个函数服务于"播放/暂停"按钮
void pause();
void resume();
void stop();
void seek(qint64 ms);
void setVolume(int percent0to100);   // ★ 界面用 0~100，换算在这一层做
void setSpeed(float ratio);
```

#### ★ 两个接口设计点

**① `setVolume` 用 `0~100` 的整数。**

因为**界面滑块天然就是这个范围**。换算成 `AudioDevice` 要的 `0.0~1.0` **在这一层做**——**界面不需要知道内核的浮点约定**。

**② `togglePlayPause()` 一个函数服务于一个按钮。**

```cpp
void MediaPlayer::togglePlayPause()
{
    switch (state_) {
    case PlaybackState::Playing:  pause();  break;
    case PlaybackState::Paused:   resume(); break;
    default:                      play();   break;
    }
}
```

**"根据当前状态决定做什么"这个逻辑放在这一层是对的**——因为**状态就是这一层维护的**。如果让界面自己判断，界面就得多存一份状态。

### 2.4 三条线程边界

**这是 M12 最核心的内容。**

| 路径 | 从哪来 | 怎么切 |
|---|---|---|
| **① 帧** | 内核**刷新线程** | **`emit` 跨线程信号**（Qt 自动排队） |
| **② 状态/事件** | **事件线程** | **`QMetaObject::invokeMethod(..., QueuedConnection)`** |
| **③ 位置** | **GUI 线程**（QTimer） | **不需要切** |

**三条路径对应三种处理方式**——**这正好体现了"同步成本的正确粒度"：需要跨线程的地方才付代价。**

---

## 三、具体实现

### 3.1 ★★ `teardown()`：线程生命周期

```cpp
void MediaPlayer::teardown()
{
    if (player_) {
        player_->close();                  // ① 停内核（内部会 join 它的 4 条线程）
        if (eventThread_.joinable())
            eventThread_.join();           // ② 等事件线程退出
        player_.reset();                   // ③ 现在销毁才安全
    }
    positionTimer_->stop();
}
```

**只有 8 行，但它是 M12 最有价值的一段**——因为**顺序错了就是 use-after-free**。

#### 三步各自的必要性

| 步骤 | 作用 | 跳过会怎样 |
|---|---|---|
| ① `player_->close()` | 停内核全部线程（**含刷新线程，它是唯一会调 `onKernelFrame` 的地方**） | 刷新线程还在跑，可能访问已析构的 scaler |
| ② `eventThread_.join()` | 等事件线程退出 | 事件线程还在读 `player_` → 悬垂 |
| ③ `player_.reset()` | 销毁 `FFPlayer` | — |

**★ 核心原则：对象生命周期的结束，必须晚于所有会用到它的线程生命周期的结束。**

#### 为什么必须封成函数（而不是在 `play()` 里直接重新赋值）

**参考工程的 `play()`：**

```cpp
player_ = std::make_unique<FFPlayer>();   // ← 旧的 FFPlayer 在这里被析构
...
if (eventThread_.joinable()) eventThread_.join();   // ← 之后才 join
```

**问题**：旧的事件线程此刻正阻塞在

```cpp
player_->messages().get(msg, true)
```

上。而 `player_ = make_unique(...)` 会**先析构旧对象**——`MessageQueue` 的 mutex / condition_variable 跟着被销毁，**而事件线程还在那个 condition_variable 上睡着**。

**详细的竞态分析见坑 1。**

#### 由此产生的一条"写法规范"

```cpp
void MediaPlayer::play()
{
    ...
    teardown();                                   // ★ 先彻底停干净
    player_ = std::make_unique<FFPlayer>();       // 再建新的
    ...
    eventThread_ = std::thread(&MediaPlayer::eventLoop, this);
}
```

```cpp
void MediaPlayer::stop()
{
    teardown();
    setState(PlaybackState::Idle);
}
```

**注意 `play()` 里最后起线程时"不需要再 join 旧线程了"**——因为 `teardown()` 已经做过。

> **这是一个很实用的重构模式**：**把"清理"抽成一个幂等的函数**，
> 让 `play()`（重建）和 `stop()`（停止）都复用它。
>
> **好处**：清理逻辑只有一份，**不会出现"play 里漏了一步、stop 里又对"的情况**。

### 3.2 ★★ `onKernelFrame()`：AVFrame → QImage

```cpp
void MediaPlayer::onKernelFrame(const Frame& frame)
{
    int w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(convertMtx_);
        if (scaler_.toRgb24(frame.frame.get(), rgbCache_, w, h) < 0)
            return;

        // ⚠ QImage 的这个构造函数【不拷贝数据】—— 它只是包装了 rgbCache_ 的指针
        QImage image(rgbCache_.data(), w, h, w * 3, QImage::Format_RGB888);

        // ★★ .copy() 是必须的
        emit frameReady(image.copy());
    }
}
```

#### ★ 为什么转换放在这里，不放 GUI 线程

**YUV → RGB 是逐像素的 CPU 活**（640×480 就是 92 万个像素）。

| 放在哪 | 后果 |
|---|---|
| GUI 线程 | **每帧占用界面线程几毫秒 → 界面会顿** |
| 刷新线程（本项目） | 界面线程只需"贴一张已经好的图"，几乎不花时间 |

**这就是"把计算留在后台线程、把呈现留给界面线程"的分工。**

> **注意它和 `ImageScaler`（M7）的配合**：
> **M7 决定"转成什么格式"，M12 决定"在哪条线程转"。** 两件事分开后各自都很清晰。

#### ★ `QImage` 的两种构造方式

| 方式 | 是否持有数据 |
|---|---|
| `QImage(data, w, h, stride, fmt)` | ❌ **只包装指针**（浅） |
| `QImage(...).copy()` | ✅ **深拷贝，自己持有** |

**第一行代码用的是浅包装**——这本身没问题（避免了一次拷贝），**但必须在传给别人的那一刻 `copy()`**。

**⚠️ 不要误以为"`QImage` 一定拷贝数据"**。它默认是**隐式共享**（copy-on-write），但**隐式共享的前提是数据由 `QImage` 自己管理**——而浅包装的数据是**别人的**。

#### 为什么 `copy()` 必须（两个理由，见坑 3）

1. `rgbCache_` 是**复用缓冲**——下一帧的转换会把它整块覆盖；
2. 这个信号会**跨线程投递**——不拷贝就等于两个线程读写同一块内存。

### 3.3 ★★ `eventLoop()`：显式切线程

```cpp
void MediaPlayer::eventLoop()
{
    Message msg;
    while (player_) {
        const int ret = player_->messages().get(msg, true);   // 阻塞取
        if (ret < 0) break;                                   // 队列被 abort

        switch (msg.what) {
        case FFMsg::Prepared:
            QMetaObject::invokeMethod(this, [this] {
                setState(PlaybackState::Playing);
                emit prepared();
                positionTimer_->start();
                if (player_) {
                    const int64_t total = player_->durationMs();
                    if (total >= 0) emit durationChanged(total);
                }
            }, Qt::QueuedConnection);
            break;
        ...
        }
    }
}
```

#### ★★ 为什么必须 `invokeMethod`，而不是直接 `emit`

**这是一个容易理解错的地方。**

**直接 `emit` 其实是安全的**——Qt 的自动连接会根据**接收者所在线程**决定是否排队，**跨线程 emit 本来就是安全的**。

**真正需要切线程的是另外两件事：**

| 操作 | 为什么必须 GUI 线程 |
|---|---|
| `setState(...)` | **改 `state_` 成员**，而它同时被 GUI 线程读（`isPlaying()` / `state()`） |
| `positionTimer_->start()` | **`QTimer` 属于 GUI 线程，只能从它自己的线程启动** |

**所以 `invokeMethod` 的作用是**：**让"改状态 + 起定时器 + 发信号"这三件事【原子地】发生在 GUI 线程上，顺序也确定。**

> **这是一个很精细的区别**：
> - **信号**跨线程 → Qt 帮你搞定
> - **对象状态和 QObject 成员**跨线程 → **必须自己搞定**
>
> **很多人只知道"信号槽能跨线程"，就以为"跨线程传数据都没问题"**——
> **忽略了"状态也需要在正确的线程里改"这件事。**

#### ★ `emit` 与 `invokeMethod` 的选择判据

| 场景 | 用什么 |
|---|---|
| 只是"通知别人一件事"（无副作用） | **直接 `emit`**（如 `onKernelFrame` 里的 `frameReady`） |
| 还要**改自己的状态**或**操作 QObject 成员** | **`invokeMethod(QueuedConnection)`** |

### 3.4 `onPositionTick()`：GUI 线程的定时器

```cpp
MediaPlayer::MediaPlayer(QObject* parent) : QObject(parent)
{
    positionTimer_ = new QTimer(this);
    positionTimer_->setInterval(200);      // 200ms 上报一次位置：够平滑也不浪费
    connect(positionTimer_, &QTimer::timeout, this, &MediaPlayer::onPositionTick);
}

void MediaPlayer::onPositionTick()
{
    if (!player_) return;
    emit positionChanged(static_cast<qint64>(player_->positionSeconds() * 1000.0));
    const int64_t total = player_->durationMs();
    if (total >= 0)
        emit durationChanged(static_cast<qint64>(total));
}
```

#### ★ 这条路不需要切线程

**`QTimer` 天然在 GUI 线程触发**，而 `positionSeconds()` 读的是 `Clock`——**`Clock` 内部自带锁**（M5 加的），视频线程也在读它。

**所以这里"什么都不用做"。**

#### 200ms 这个数字的来历

| 快一点 | 慢一点 |
|---|---|
| 进度条更平滑 | 更省 CPU |
| 但 UI 刷新也占带宽 | 但 >500ms 会觉得卡 |

**200ms 是常见的选择**——**1 秒 5 次，肉眼看起来是连续的**。

#### ⚠️ 不要在这里做耗时操作

**它跑在 GUI 线程，卡住就是界面卡住。**

### 3.5 状态机与动作

```cpp
void MediaPlayer::pause()
{
    if (player_ && state_ == PlaybackState::Playing) {
        player_->setPaused(true);
        setState(PlaybackState::Paused);
    }
}

void MediaPlayer::resume()
{
    if (player_ && state_ == PlaybackState::Paused) {
        player_->setPaused(false);
        setState(PlaybackState::Playing);
    }
}
```

**注意每个动作都先判状态**——比如 `pause()` 只在 `Playing` 时才生效。

**为什么？** 因为**同一时刻可能有多个"来源"要改状态**（用户点按钮、内核发事件、定时器），**状态判断能防止"重复动作"和"非法迁移"**。

#### `setState` 集中处理

```cpp
void MediaPlayer::setState(PlaybackState s)
{
    if (state_ == s)
        return;                    // 状态没变就别发信号，减少 UI 无谓刷新
    state_ = s;
    qDebug() << "[MediaPlayer] 状态 ->" << ...;   // 诊断日志
    emit stateChanged(s);
}
```

**所有状态迁移都走这一个函数**——好处：

1. **不会漏发 `stateChanged`**；
2. `if (state_ == s) return;` 的**去重只写一处**；
3. **日志只写一处**（排查"卡在哪一步"时非常有用）。

> **这个模式和 M8 的 `openComponent`、M10 的 `videoRefresh` 是同一个思路**：
> **把"必须成对/按顺序做的事"收在一个函数里。**

#### 关于那条 `qDebug`

**它是刻意留的**：

> 真实播放器都会有这类日志——**用户报"卡住/黑屏"时，第一件事就是看状态停在哪一步**（是 `Preparing` 没过去？还是 `Playing` 了但没帧？）。

**在 WIN32（无控制台）构建里它进调试器输出，没有副作用。**

---

## 四、要注意的问题

### ★★ 坑 1：`play()` 里的线程生命周期竞态

**这是 M12 挖出的最重要的缺陷。**

#### 问题代码（参考工程）

```cpp
void MediaPlayer::play()
{
    player_ = std::make_unique<FFPlayer>();       // ← 旧的 FFPlayer 在这里被析构
    ...
    if (eventThread_.joinable()) eventThread_.join();   // ← 之后才 join
}
```

#### 竞态过程

```
时刻 T1：旧事件线程阻塞在 player_->messages().get(msg, true)
          —— 它持有对 msgQ_ 的引用，睡在 msgQ_ 的 condition_variable 上
时刻 T2：用户点播放 → play()
          player_ = make_unique(...) → 【旧 FFPlayer 析构】
          → close() → msgQ_.abort() → notify_all
          → 但紧接着 msgQ_ 的 mutex / cv 被销毁
时刻 T3：事件线程被唤醒，尝试【重新获取 mutex】
          —— 而那个 mutex 可能已经不存在了
```

**"被唤醒后重新获取锁"与"锁被销毁"之间是一个真实的竞态窗口——属于未定义行为。**

#### 更糟的第二处

```cpp
void MediaPlayer::eventLoop()
{
    while (player_) {          // ← 循环条件在【读那个正被改写的 unique_ptr】
        ...
    }
}
```

**`std::unique_ptr` 不是原子的**——GUI 线程在 `play()` 里赋值，事件线程在 `while` 条件里读，**这是另一个数据竞争**。

#### ★ 复现路径短得惊人

> **播完之后（状态 `Completed`）再点一次播放。**

这时 `player_` 非空、事件线程还活着（阻塞在 `get` 上），`play()` 就会踩进这个窗口。

**为什么"播完之后"？**

| 状态 | `play()` 会怎样 |
|---|---|
| `Idle`（stop 过） | `player_` 已被 `teardown()` 清空 → **不会析构任何东西** |
| `Playing` / `Preparing` | 直接 `return` |
| **`Completed`** | **`player_` 非空且会走到底 → 踩中** |

#### 修复

**引入 `teardown()`，把三步绑在一起、顺序固定**（见 3.1）。

**实测**：

```
连续 12 轮 play->完成 全部成功，没有崩溃或挂死
```

**12 轮 × 每轮 1 次 play（从 Completed 状态）+ 1 次 seek + 1 次 completed，累计 281 帧。**

#### 📌 这条坑的教训

> **对象生命周期的结束，必须晚于所有会用到它的线程生命周期的结束。**
>
> **这个顺序不能靠"调用方记得先 stop"来保证，必须封在一个函数里。**
>
> 而且要意识到：**"赋值给智能指针"这个动作，等于"析构旧对象"**——
> 它看起来只是一次赋值，**实际上是资源生命周期的分界线**。

---

### ★★ 坑 2：`connect` 三参数 vs 四参数

#### 代码

```cpp
// 三参数：没有接收者上下文
connect(sender, signal, lambda);

// 四参数：有上下文
connect(sender, signal, context, lambda);
```

#### ★ 三参数版本会把槽跑在**发送者的线程**里

**Qt 无从判断"槽应该在哪个线程执行"**（因为没有接收者对象），于是**连接的默认类型是 `DirectConnection`**——

> **槽会在【发送信号的线程】里同步执行。**

#### 我在测试里亲身踩了

**第一版测试用三参数接 `frameReady`**，结果：

```
在非 GUI 线程收到信号的次数: 61      ← 全部 61 帧都在内核刷新线程里被执行了！
```

**`frameReady` 是在内核刷新线程里 `emit` 的**——用三参数接，**槽就在刷新线程里跑**。

**如果槽里碰了 UI，这就是灾难性的崩溃。**

#### 四参数版本为什么能解决

```cpp
QObject::connect(&mp, &MediaPlayer::frameReady, &guiContext, [&rec](const QImage& img) {
    ...
});
```

**用 context 对象的线程来决定连接类型**：

```
context 活在 GUI 线程 + 发送发生在别的线程
  → 自动变成 QueuedConnection
  → 槽在 GUI 线程执行 ✓
```

#### 真实工程的正确写法

**直接把接收控件传进去：**

```cpp
connect(mp_, &MediaPlayer::frameReady, display_, &DisplayWind::presentFrame);
//                                     ^^^^^^^^ 接收者在 GUI 线程 → 自动排队
```

**修正后实测：**

```
在非 GUI 线程收到信号的次数: 0
```

#### 📌 这条坑的教训

> **`connect` 不写 context，就等于放弃了对自己槽所在线程的控制。**
>
> **凡是跨线程信号，一律写四参数。**

**⚠️ 这个坑的隐蔽性在于**：三参数版本**语法完全正确、编译通过、大部分时候也能工作**（因为 `DirectConnection` 在同线程时就是对的）——**只有在跨线程时才出问题，而且症状是"偶发崩溃"**。

---

### ★ 坑 3：`image.copy()` 为什么必须

**两个理由，缺一不可：**

#### 理由 1：`rgbCache_` 是复用缓冲

```
帧 N   → swr_convert 写入 rgbCache_ → QImage 包装它 → emit
帧 N+1 → swr_convert 【覆盖 rgbCache_】← 此时帧 N 的 QImage 还"指向"这块内存！
```

**不拷贝的话，接收方拿到的 `QImage` 会随着下一帧的到来而"变色"。**

**表现**：界面上显示的画面**永远是上一帧和当前帧的混合体**（撕裂）。

#### 理由 2：跨线程投递

**信号在刷新线程 `emit`，槽在 GUI 线程执行。**

**不拷贝就等于两个线程同时读写同一块内存——数据竞争。**

#### ★ 那为什么不在 `toRgb24` 里就拷贝

**因为那样会多一次拷贝。**

现在的流程：

```
swr/转换 → rgbCache_（复用，零分配）
         → QImage 浅包装（零拷贝）
         → .copy()（一次拷贝，得到"属于接收方"的数据）
```

**如果让 `ImageScaler` 每次返回一个新 `vector`，就是"每帧一次堆分配 + 一次拷贝"**——**比现在差**。

> **`copy()` 放在"最后一次使用点"是最优的**：
> **在此之前都是"内部借用"，只有跨出去的那一刻才需要"独立所有权"。**

#### ⚠️ 一个容易搞混的点

**`QImage` 有隐式共享（copy-on-write）机制**——所以很多人会以为"赋值 `QImage` 很便宜，不用管拷贝"。

**这个理解只对了一半**：

| 场景 | 隐式共享生效吗 |
|---|---|
| `QImage` 自己持有数据（构造时拷贝过，或用 `QImage(w,h,fmt)` 分配） | ✅ 生效，赋值只增加引用计数 |
| **`QImage(data, ...)` 浅包装外部缓冲** | ❌ **不生效**——它不拥有那块内存，谈不上"共享" |

**所以判断标准是"这块内存归谁管"，而不是"是不是 QImage"。**

---

### ★ 坑 4：`convertMtx_` 其实不解决任何问题

**参考工程有一把锁保护 `scaler_` / `rgbCache_`：**

```cpp
std::lock_guard<std::mutex> lock(convertMtx_);
```

**但 `onKernelFrame` 只被【刷新线程】一条线程调用**——所以**这把锁当前不解决任何实际问题**，属于**防御性冗余**。

#### ★ 我保留了它，但在注释里写清了两面性

```cpp
// 保护 scaler_ / rgbCache_。
// ⚠ 说实话：当前 onKernelFrame 只被【刷新线程】一条线程调用，
//   所以这把锁并不解决任何实际问题，属于防御性冗余。
//   保留它的理由是"万一将来多一个帧消费方"；但你要知道它现在不起作用，
//   别误以为"加了锁所以这里一定线程安全"。
```

#### 📌 这条坑的价值在于"诚实的边界"

> **代码里有两类东西容易骗人**：
> - **看起来多余但实际上必要的**（比如 `while(true)` 里的 `sleep`——防止 CPU 空转）
> - **看起来必要但实际上多余的**（比如这把锁）
>
> **后者更危险**：因为它**会让人误以为"这里已经线程安全了"**，
> 从而在真正需要加锁的地方（或者需要"必须都在同一线程"的地方）放松警惕。

**这算是给前面几轮的一个呼应**：

- M3 讲"`peek()` 不加锁"——**约定"单线程访问"比加锁更清晰**；
- M8 讲"`av_seek_frame` 串行化"——**与其加锁，不如规定只有一条线程能碰**；
- **M12 这里则是反向的例子**——**不要把"加锁"当成"线程安全"的同义词**。

---

### 坑 5：Debug 版 Qt 的 DLL 需要 PATH

**`test_mediaplayer` 是第一个【真正在运行时需要 Qt DLL】的目标。**

> **前面那些测试虽然也过 AUTOMOC，但生成的 moc 文件是空的、不引用 Qt 符号**，
> **所以能独立运行。**

**症状**：

```
error while loading shared libraries: Qt6Cored.dll
```

**修法**：命令行运行需要把 Qt 的 bin 目录加到 `PATH`：

```bash
PATH="/c/Qt/6.8.2/msvc2022_64/bin:$PATH" ./test_mediaplayer.exe
```

**Qt Creator 会自动处理**（它知道 Kit 的 Qt 路径）——**所以这个问题只在命令行构建/CI 时出现。**

> **注意 `Qt6Cored.dll` 结尾的 `d`** —— 那是 **Debug 版**的标记。
> Debug 程序必须配 Debug 版 Qt DLL（和 FFmpeg 那边的 Debug/Release 问题同源）。

---

### 坑 6：`qDebug` 在 Windows 上走 `OutputDebugString`

**第一次运行 M13 的成品版时**，进程存活了，**但日志文件是空的**。

**根因**：**Qt 在 Windows 上默认把日志送到 `OutputDebugString`**，而不是 `stderr`——因为 GUI 程序通常没有控制台。

**两种查看方式：**

| 方式 | 做法 |
|---|---|
| 调试器 | `DebugView`（Sysinternals）或 VS 的"输出"窗口 |
| **强制走 stderr** | 设环境变量 **`QT_FORCE_STDERR_LOGGING=1`** |

**实测（M13 验证时用的）：**

```bash
QT_FORCE_STDERR_LOGGING=1 ./voice_player_learn.exe "X:/_media/test_media.mp4"
# 输出：
# [main] 命令行打开: "X:/_media/test_media.mp4"
# [MediaPlayer] 状态 -> Preparing
# [MediaPlayer] 状态 -> Playing
# [DisplayWind] 已显示 25 帧左右，尺寸 QSize(640, 480)
# [MediaPlayer] 状态 -> Completed
```

#### 📌 这条坑的教训

> **"你的日志没输出，不一定是没执行到，可能是送去了别的地方。"**
>
> 排查"程序跑到哪一步"时，**先确认日志通道是通的**——
> 否则你会去怀疑代码逻辑，而问题只是在输出目标上。

---

## 五、实测验证数据

`test_mediaplayer.cpp` 共 **7 组测试、16 项断言**，全部通过。

### ★ 这是一个 Qt 控制台程序

```cpp
QCoreApplication app(argc, argv);   // 注意：QCoreApplication，不是 QApplication
```

**为什么需要真的跑 Qt 的事件循环？**

因为要验证**信号槽和跨线程投递**——**`QueuedConnection` 的投递目标就是这个事件循环**。

**没有事件循环，所有排队信号都不会被处理。**

**测试的驱动方式**：**"边 `processEvents()` 边等条件"**：

```cpp
static bool pumpUntil(const std::function<bool()>& pred, int timeoutMs)
{
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (pred()) return true;
        if (超时) return false;
        msleep(3);
    }
}
```

> **注意这里用 `QCoreApplication` 而不是 `QApplication`** ——
> 我们**不需要 GUI，只需要事件循环**。少一个 `QApplication` 意味着更少的初始化（不加载平台插件）。

### [1] 信号序列与状态迁移

```
[PASS] 收到 prepared 信号
[PASS] ★ 状态迁移序列正确：Preparing -> Playing
      stateChanged 序列前 3 个: 1 2
```

### [2] ★ `frameReady`：QImage 的数量 / 尺寸 / 内容

```
已收到 61 帧 QImage（首帧 640x480）
[PASS] ★ 持续收到视频帧
[PASS] ★ 每帧尺寸都是 640x480（转换结果正确）
[PASS] ★ 没有全黑的帧 —— 说明 RGB 数据确实被拷进 QImage 了（不是空指针包装）
```

**★ 第三条断言的写法很有讲究**：

```cpp
// 抽样看内容：素材是蓝色调 + 一个亮方块，不该是纯黑
int bright = 0;
for (int y = 0; y < img.height(); y += 20)
    for (int x = 0; x < img.width(); x += 20) {
        const QRgb p = img.pixel(x, y);
        if (qRed(p) + qGreen(p) + qBlue(p) > 40) ++bright;
    }
if (bright == 0) ++rec.blankFrames;
```

**为什么必须验证"内容非空"？**

因为**如果忘了 `.copy()` 或者拷贝失败，`QImage` 是"尺寸正确但内容全黑"的**——
**尺寸断言抓不到这个问题**。

> **"尺寸对"不等于"内容对"**——这和 M10 的"帧率对不等于同步对"是同一类思维：
> **要验证什么，就要去找那个"只有在目标达成时才会出现的量"。**

### [3] ★★ 线程归属 —— 见坑 2

```
GUI 线程 = 0000025E3CFBE090（guiContext 也活在这个线程）
[PASS] ★★ 所有信号的槽都在 GUI 线程执行 —— 跨线程投递机制生效
```

**实现方式**：在接收槽里比较当前线程：

```cpp
QObject::connect(&mp, &MediaPlayer::frameReady, &guiContext, [&rec](const QImage& img) {
    rec.noteThread();      // 比较 QThread::currentThread() 与录制时记下的 GUI 线程
    ...
});
```

**这一组是"跨线程机制"的直接验证**——不是"应该能跨"，而是**真的检查了槽在哪个线程跑**。

### [4] 位置与时长上报

```
位置信号 11 次（最后一次 2247 ms），时长信号 12 次（5015 ms）
[PASS] 周期性收到 positionChanged（200ms 定时器在工作）
```

### [5] ★★ 重复播放（测 `teardown` 修的竞态）

```
第  1 轮完成（帧总数 94）
第  4 轮完成（帧总数 145）
第  8 轮完成（帧总数 213）
第 12 轮完成（帧总数 281）
[PASS] ★★ 连续 12 轮 play->完成 全部成功，没有崩溃或挂死
```

**测试设计的要点**：

```cpp
// ★ 先让当前这次播放跑到 Completed —— 因为 play() 在 Playing 状态下
//   会直接返回。我们要构造的正是"从 Completed 状态再次 play"这条路径。
mp.seek(4600);                                   // 跳到接近末尾，快点放完
pumpUntil([&rec] { return rec.gotCompleted.load(); }, 5000);

for (int i = 0; i < 12; ++i) {
    mp.play();                                   // ← 此时 state 是 Completed
    ...
}
```

**两个设计点：**

1. **必须从 `Completed` 状态 play** —— 因为只有这条路径会踩中竞态（见坑 1 的分析）；
2. **用 `seek(4600)` 加速** —— 否则每轮要等 5 秒，12 轮就是 1 分钟。

> **"构造能走到目标分支的状态"**——这是"要测一条分支，必须构造能走到那条分支的输入"的又一次应用（M10 用纯视频素材覆盖降级路径，这里用 Completed 状态覆盖竞态路径）。

### [6] / [7] 错误路径与 stop

```
[PASS] ★ 打开不存在的文件会发出 errorOccurred 信号
      错误内容: No such file or directory
[PASS] 状态被置为 Error
[PASS] stop 之后状态是 Idle
[PASS] stop 之后还能重新播放
```

**最后一条验证的是"实例可以复用"**——`stop()` 之后 `player_` 被清空，再 `play()` 会重新创建。**这条路径也要测**，否则用户"停下来再播"就会崩。

---

## 六、踩坑清单汇总

| 类别 | 坑 | 后果 | 修法 |
|---|---|---|---|
| **生命周期** | **`play()` 先析构旧 `player_`、后 join 事件线程** | 事件线程在已销毁的 cv 上醒来 → **UB / 偶发崩溃** | `teardown()` 固定三步顺序 |
| **生命周期** | `eventLoop` 的 `while (player_)` 读非原子 `unique_ptr` | 数据竞争 | 靠 `teardown()` 保证顺序 |
| **Qt** | **`connect` 用三参数（无 context）** | 槽跑在**发送者线程**里 → **碰 UI 就崩** | **一律用四参数** |
| **Qt** | 以为 `QImage` 一定拷数据 | 浅包装外部缓冲时内容会变 | 明确区分"包装"与"持有" |
| **QImage** | 忘了 `.copy()` | 画面撕裂 / 数据竞争 | 跨出去前拷贝 |
| **QImage** | 每帧新建 `vector` 传出去 | 每帧一次堆分配 + 拷贝 | 复用缓冲 + 最后一次使用点才拷贝 |
| 线程 | 以为"信号跨线程安全"就等于"所有跨线程都安全" | **状态和 QObject 成员没切线程** | 改状态/动 QTimer 必须 `invokeMethod` |
| 线程 | `onKernelFrame` 里做耗时操作 | 拖慢刷新线程 | 转换在刷新线程，**绘制**在 GUI |
| 线程 | 在 `onPositionTick` 里做耗时操作 | **卡住 GUI** | 它跑在 GUI 线程 |
| 状态 | 多个来源改状态，判断分散 | 非法迁移 / 漏发信号 | `setState()` 集中处理 |
| 接口 | 音量用 0.0~1.0 传给界面 | 界面要多做换算 | **界面用 0~100，换算在这层** |
| 部署 | Debug 版找不到 `Qt6Cored.dll` | 启动即失败 | PATH 加 Qt bin（End 有 `d` 是 Debug） |
| 日志 | 以为日志没输出 = 没执行到 | 排查方向跑偏 | `QT_FORCE_STDERR_LOGGING=1` |
| 设计 | 加锁当作"线程安全"的同义词 | **放松警惕** | 分清"锁的作用"和"必须同线程" |

---

## 七、面试常见追问

**Q：为什么需要 `MediaPlayer` 这一层？不能直接在 UI 里用 `FFPlayer` 吗？**

**两者语义模型完全不同**：

- 内核：`std::function` 回调 + 一个没有返回值的异步消息队列；
- Qt：信号槽（声明式、跨线程安全、可多播）。

**必须有个翻译官。**

**而且这一层还不含任何播放逻辑**——它只做三件事：消息→信号、`AVFrame`→`QImage`、Qt 调用→内核调用。

**判断标准**：能不能把它删掉换成另一个 UI 框架，而内核一行不改。

**Q：`QImage` 为什么要 `.copy()`？它不是有隐式共享吗？**

**隐式共享的前提是"数据由 `QImage` 自己管理"。**

`QImage(data, w, h, stride, fmt)` 这个构造函数**不拷贝数据，只包装指针**——数据是**别人的**（我们的复用缓冲），**谈不上共享**。

**两个必须拷贝的理由**：
1. 缓冲是复用的，下一帧会覆盖它；
2. 信号跨线程投递，不拷贝就是两个线程读写同一块内存。

**Q：为什么转换放在刷新线程而不是 GUI 线程？**

**YUV→RGB 是逐像素的 CPU 活**（640×480 就是 92 万像素）。

放 GUI 线程等于每帧占用界面几毫秒 → **界面会顿**。

放刷新线程，界面只需"贴一张已经好的图"。

**这就是"把计算留在后台线程、把呈现留给界面线程"。**

**Q：`connect` 的三参数和四参数有什么区别？**

| 版本 | 连接类型 |
|---|---|
| `connect(sender, signal, lambda)` | **没有接收者上下文 → `DirectConnection`** → **槽在发送者线程执行** |
| `connect(sender, signal, context, lambda)` | 用 **context 的线程**决定 → 跨线程时自动 `QueuedConnection` |

**我在测试里踩过这个坑**：三参数接 `frameReady`，结果 61 帧全部在**内核刷新线程**里执行。

**凡是跨线程信号，一律用四参数。**

**Q：直接 `emit` 不是已经跨线程安全了吗？为什么还要 `invokeMethod`？**

**是的，直接 `emit` 是安全的**——Qt 会根据**接收者所在线程**决定是否排队。

**但需要 `invokeMethod` 的不是"发信号"，而是另外两件事**：

1. `setState(...)` 要**改 `state_` 成员**，而它同时被 GUI 线程读；
2. `positionTimer_->start()` **操作 `QTimer`**——`QTimer` 属于 GUI 线程。

**所以 `invokeMethod` 的作用是"让这三件事原子地、按确定顺序发生在 GUI 线程上"。**

**很多人的误解是**："信号槽能跨线程" = "跨线程传数据都没问题"——**忽略了"状态也需要在正确的线程里改"。**

**Q：`play()` 里那个竞态具体是什么？**

参考工程：

```cpp
player_ = std::make_unique<FFPlayer>();       // ← 旧对象在这里析构
if (eventThread_.joinable()) eventThread_.join();   // ← 之后才 join
```

**旧的事件线程此刻正阻塞在 `player_->messages().get()` 上**——`make_unique` 赋值会**先析构旧对象**，`MessageQueue` 的 mutex/cv 被销毁，**而事件线程还在那个 cv 上睡着**。

"被唤醒后重新获取锁"与"锁被销毁"之间是**真实的竞态窗口（UB）**。

**复现路径**：**播完之后（`Completed`）再点一次播放**。

**修法**：`teardown()` 把"close → join 事件线程 → reset"三步绑死顺序。

**Q：`onPositionTick` 为什么不需要切线程？**

因为 **`QTimer` 天然在 GUI 线程触发**，而 `positionSeconds()` 读的 `Clock` **内部自带锁**（M5 加的）。

**所以这条路"什么都不用做"。**

**三条线程路径（帧 / 状态 / 位置）用了三种不同处理**——**正好体现"同步成本的正确粒度"。**

**Q：`convertMtx_` 有用吗？**

**说实话：当前不解决任何问题。**

`onKernelFrame` 只被刷新线程一条线程调用，**这把锁是防御性冗余**。

**我保留它但在注释里写清了**——因为**"看到锁就以为线程安全"是一种危险的误判**。

**这类"看起来必要但实际多余"的代码比"看起来多余但必要"的更危险**，因为它会让人放松警惕。

**Q：怎么测"信号真的在 GUI 线程被投递"？**

**在接收槽里比较当前线程**：

```cpp
QObject::connect(&mp, &MediaPlayer::frameReady, &guiContext, [&rec](const QImage& img) {
    if (QThread::currentThread() != rec.guiThread)
        rec.wrongThreadSeen.fetch_add(1);
});
```

**实测：`在非 GUI 线程收到信号的次数: 0`。**

**这一组的价值是**：它验证的不是"我认为应该能跨线程"，而是**"确实检查了槽在哪个线程跑"**。

**Q：怎么验证 `QImage` 内容是对的（不只是尺寸对）？**

**抽样检查像素。**

因为**如果忘了 `.copy()` 或拷贝失败，`QImage` 会是"尺寸正确但内容全黑"的**——**尺寸断言抓不到**。

```cpp
int bright = 0;
for (int y = 0; y < h; y += 20)
    for (int x = 0; x < w; x += 20)
        if (qRed(p) + qGreen(p) + qBlue(p) > 40) ++bright;
// bright == 0 → 全黑 → 有问题
```

**"尺寸对"不等于"内容对"。**

---

*文档对应代码版本：`learn/src/mediaplayer.{h,cpp}`（M12 完成态）*
