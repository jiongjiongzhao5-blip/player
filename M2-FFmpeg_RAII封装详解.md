# M2 FFmpeg RAII 封装详解：`av_utils.h`

> 本文档对应学习工程 `learn/` 的第 2 个模块（阶段一"地基"的第二件）。  
> 代码位置：`learn/src/av_utils.h`（纯头文件，无 .cpp）  
> 测试位置：`learn/tests/test_av_utils.cpp`

---

## 目录

- [一、模块定位](#一模块定位)
- [二、功能](#二功能)
  - [2.1 它解决什么问题](#21-它解决什么问题)
  - [2.2 它提供什么](#22-它提供什么)
- [三、具体实现](#三具体实现)
  - [3.1 `extern "C"`：为什么必须加](#31-extern-c为什么必须加)
  - [3.2 编译期版本闸门](#32-编译期版本闸门)
  - [3.3 删除器：为什么用"结构体 + operator()"](#33-删除器为什么用结构体--operator)
  - [3.4 为什么用 `unique_ptr` 而不是 `shared_ptr`](#34-为什么用-unique_ptr-而不是-shared_ptr)
  - [3.5 便捷工厂](#35-便捷工厂)
  - [3.6 工具函数](#36-工具函数)
- [四、要注意的问题](#四要注意的问题)
- [五、实测验证数据](#五实测验证数据)
  - [★ 测试 \[3\]：怎么"看见"资源真的被释放了](#-测试-3怎么看见资源真的被释放了)
- [六、踩坑清单汇总](#六踩坑清单汇总)
- [七、面试常见追问](#七面试常见追问)

---

## 一、模块定位

`av_utils.h` 是**全项目的"内存安全地基"**，被后面几乎所有模块包含：

| 模块                   | 用到了哪些                                               |
| -------------------- | --------------------------------------------------- |
| M6 `Decoder`         | `AVCodecContextPtr`                                 |
| M7 `ImageScaler`     | `SwsContextPtr`                                     |
| M9 `FFPlayer`（音频）    | `SwrContextPtr`                                     |
| M8/M9/M10 `FFPlayer` | `AVFormatContextPtr` / `AVPacketPtr` / `AVFramePtr` |

**核心承诺：业务代码里再也不出现一次手工 `free`。**

所有 FFmpeg 对象都像 `std::vector` 一样"到点自动销毁"——**这是后面 11 个模块能放心写的前提**。

---

## 二、功能

### 2.1 它解决什么问题

FFmpeg 是纯 C 库，资源管理模型&#x662F;**"手工配对"**：

```c
AVFormatContext* ic = NULL;
avformat_open_input(&ic, ...);     // 分配
// ... 中间可能有几十条 return / break / 异常路径
avformat_close_input(&ic);         // 必须配对释放
```

**只要有任何一条 `return` 路径漏掉释放，就是内存泄漏。**

而且播放器的泄漏是**流式累积**的：

> 一次泄漏一个 `AVFrame`，1080p 一帧 3MB，**每秒 25 帧就是 75MB/s**——几秒钟就吃掉几个 GB。

**RAII 让编译器替我们记住这件事**：把"释放"绑在对象的生命周期上，

```cpp
{
    AVFormatContextPtr ic(open_input(...));
    if (fail1()) return;      // ← 自动释放
    if (fail2()) return;      // ← 自动释放
    // 正常结束              // ← 自动释放
}
```

**无论从哪条路径出去，析构函数都会被调用。**

### 2.2 它提供什么

四类东西，一共不到 90 行：

| 类别         | 内容                                                                                                                |
| ---------- | ----------------------------------------------------------------------------------------------------------------- |
| **编译期防线**  | 版本闸门（`LIBAVCODEC_VERSION_MAJOR < 61` 直接 `#error`）                                                                 |
| **删除器**    | 6 个：`AVFormatContext` / `AVCodecContext` / `AVFrame` / `AVPacket` / `SwsContext` / `SwrContext`                   |
| **智能指针别名** | 6 个：`AVFormatContextPtr` / `AVCodecContextPtr` / `AVFramePtr` / `AVPacketPtr` / `SwsContextPtr` / `SwrContextPtr` |
| **便捷工具**   | `make_frame()` / `make_packet()` / `av_err_string()` / `print_av_error()`                                         |

---

## 三、具体实现

### 3.1 `extern "C"`：为什么必须加

```cpp
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
// ...
}
```

**FFmpeg 是 C 库，函数符号没有 C++ 名字修饰（name mangling）。**

| 语言                   | `av_frame_alloc` 编译后的符号名                |
| -------------------- | --------------------------------------- |
| C                    | `av_frame_alloc`                        |
| C++（不加 `extern "C"`） | `?av_frame_alloc@@YAPEAVAVFrame@@XZ` 之类 |

**不加 `extern "C"` 的话，C++ 编译器会按 C++ 规则修饰这些函数名，链接时就会报"找不到符号"。**

> **注意报错的形态**：它会报 `unresolved external symbol "?av_frame_alloc@@..."`——  
> 你看到的是一个**被修饰过的乱码符号**，而不是 `av_frame_alloc`。  
> **不认识这个现象的人会去怀疑"库没链接上"，其实只是 `extern "C"` 漏了。**

### 3.2 编译期版本闸门

```cpp
// 版本对照：FFmpeg 7.x = libavcodec 61，8.x = 62，9.x = 63
#if LIBAVCODEC_VERSION_MAJOR < 61
#error "本项目要求 FFmpeg 7 及以上版本（推荐 FFmpeg 9），请升级 FFmpeg 开发包。"
#endif
```

#### 为什么需要它

**FFmpeg 7 是一次"破坏性升级"，删掉/改掉了一批老 API：**

| 被移除的                                                            | 替代方案                             |
| --------------------------------------------------------------- | -------------------------------- |
| `AVCodecContext.channels` / `channel_layout`、`AVFrame.channels` | 统一的 `AVChannelLayout ch_layout`  |
| `swr_alloc_set_opts()`                                          | `swr_alloc_set_opts2()`          |
| `av_get_default_channel_layout()`                               | 无（用 `av_channel_layout_default`） |
| `avcodec_decode_video2` / `avcodec_decode_audio4`               | `send_packet` / `receive_frame`  |

**如果有人拿 FFmpeg 4.x 来编译**，会得到**几百条**"结构体没有成员 `channels`"之类的报错——**极难定位根因**（你会以为是自己的代码写错了）。

**所以在头文件顶部用 `#error` 提前拦住，并直接告诉他原因。**

> **这是一个很有价值的工程习惯**：**当"错误的使用方式"会产生大量难以理解的报错时，  
> 主动加一道门槛，把问题提前到"一句话就能看懂"的位置。**
>
> 代价是 3 行代码，收益是——**下一个接手的人不会浪费半天去查"为什么 channels 不存在"**。

#### ⚠️ 双保险：为什么 CMake 里还要再查一次

```cmake
# CMakeLists.txt 里也做了一次版本检查
```

| 防线                        | 位置   | 生效时机       |
| ------------------------- | ---- | ---------- |
| CMake `find_package` 版本检查 | 配置阶段 | `cmake` 时  |
| 头文件 `#if` 闸门              | 编译阶段 | `cl.exe` 时 |

**`#if` 是防止有人绕过 CMake 直接编译**（比如写个脚本 `cl main.cpp /I...`）。

> **"在多个层次重复做同一件事"不是冗余，而是**——  
> **每一层的绕过成本不同，防线要放在所有可能的入口上。**  
> 这条原则在别处也适用：`extern "C"` 和 `#pragma once`、服务端的参数校验、前端的表单校验……

### 3.3 删除器：为什么用"结构体 + `operator()`"

```cpp
struct AVFormatContextDeleter {
    void operator()(AVFormatContext* p) const { if (p) avformat_close_input(&p); }
};

struct AVCodecContextDeleter {
    void operator()(AVCodecContext* p) const { if (p) avcodec_free_context(&p); }
};

struct AVFrameDeleter {
    void operator()(AVFrame* p) const { if (p) av_frame_free(&p); }
};

struct AVPacketDeleter {
    void operator()(AVPacket* p) const { if (p) av_packet_free(&p); }
};

// ★ 注意：sws_freeContext 是全 FFmpeg 里唯一一个接"一级指针"的释放函数
struct SwsContextDeleter {
    void operator()(SwsContext* p) const { if (p) sws_freeContext(p); }
};

struct SwrContextDeleter {
    void operator()(SwrContext* p) const { if (p) swr_free(&p); }
};
```

#### 三个实现细节

**① 用"结构体 + `operator()`"而不是函数指针。**

`unique_ptr` 的默认删除器是 `std::default_delete`。要换成自定义的，最省事的就是给一个**可调用对象类型**。

**函数对象是零开销的**——编译器能内联 `operator()`，生成的代码和手写 `free` 完全一样。如果用函数指针，每次析构都要**多一次间接调用**，而且编译器无法内联。

**② `operator()` 标记 `const`。**

删除器本身不该被修改。这是"逻辑常量性"的正确表达。

**③ 先判断非空再释放。**

多数 FFmpeg 释放函数**能容忍空指针**（`av_frame_free(&p)` 内部会检查 `*p`），但显式判断：

- **更安全**（不依赖库的实现细节）；
- **表明意图**（"我要释放一个可能为空的东西"）。

#### 📌 `sws_freeContext` 是唯一的例外 —— 这解释了为什么没用"统一模板"

**FFmpeg 里几乎所有释放函数都接二级指针：**

```c
void av_frame_free(AVFrame **frame);        // 二级：为了能把指针置空
void avformat_close_input(AVFormatContext **s);
void swr_free(SwrContext **s);
```

**"接二级指针"的意义**：释放之后顺手把调用方的指针置成 `NULL`，防悬挂。

**但 `sws_freeContext` 偏偏接一级指针：**

```c
void sws_freeContext(struct SwsContext *swsContext);
```

**它不置空。** 而 `unique_ptr` 的删除器拿到的是**一级指针**（`unique_ptr` 自己负责把内部指针置空）——

| 释放函数                 | 能否直接用 `unique_ptr` 默认调用形式              |
| -------------------- | -------------------------------------- |
| `av_frame_free(&p)`  | ✅ 对局部变量 `p` 取地址，`unique_ptr` 之后置空自己的成员 |
| `sws_freeContext(p)` | ✅ 直接传值，正合适                             |

**两类都能适配，所以 6 个删除器各写各的反而最清楚。**

> **这正是"统一模板方案"被否掉的原因**：如果想用一个模板统一处理，  
> 就得处理"有的接一级、有的接二级"这个差异——  
> 要么加特化，要么加 trait，**复杂度立刻超过"手写 6 个小结构体"**。
>
> **教训：当"通用方案"需要为每个特例加分支时，就说明它不通用。**  
> 老老实实写 6 遍，反而更好读、更好改。

### 3.4 为什么用 `unique_ptr` 而不是 `shared_ptr`

```cpp
using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
using AVCodecContextPtr  = std::unique_ptr<AVCodecContext,  AVCodecContextDeleter>;
using AVFramePtr         = std::unique_ptr<AVFrame,         AVFrameDeleter>;
using AVPacketPtr        = std::unique_ptr<AVPacket,        AVPacketDeleter>;
using SwsContextPtr      = std::unique_ptr<SwsContext,      SwsContextDeleter>;
using SwrContextPtr      = std::unique_ptr<SwrContext,      SwrContextDeleter>;
```

**三条理由：**

**① 零开销。**

`unique_ptr` 的大小**和裸指针一样**，析构就是**一次函数调用**，没有原子引用计数。  
`shared_ptr` 多一个控制块 + 每次拷贝/析构的原子操作——**在高频路径上（比如每帧）这是实打实的代价**。

**② 所有权唯一。**

FFmpeg 对象天然只有一个所有者（谁分配谁释放）。**用 `shared_ptr` 是在表达一个不存在的语义。**

**③ 不可拷贝、只能移动。**

```cpp
AVFramePtr a = make_frame();
AVFramePtr b = a;              // ✗ 编译错误！
AVFramePtr b = std::move(a);   // ✓ 显式转移，a 变成空
```

**这一条最重要**：它**在编译期杜绝了"两个指针指向同一块内存、释放两次"的经典崩溃**。

如果用的是裸指针，下面这段代码能编过，运行时 double-free 崩溃：

```cpp
AVFrame* f = av_frame_alloc();
AVFrame* g = f;                 // 浅拷贝 —— 编译器不会拦你
av_frame_free(&f);
av_frame_free(&g);              // 💥 double free
```

**用 `unique_ptr` 之后，这类 bug 从"运行时崩溃"变成了"编译不过"。**

> **"把运行时错误提前到编译期"是 C++ 现代实践的核心目标之一。** 这是最典型的一个例子。

### 3.5 便捷工厂

```cpp
inline AVFramePtr  make_frame()  { return AVFramePtr(av_frame_alloc()); }
inline AVPacketPtr make_packet() { return AVPacketPtr(av_packet_alloc()); }
```

`av_frame_alloc` / `av_packet_alloc` 分配的&#x662F;**"空壳"**（没有任何数据）。包一层之后业务代码可以直接写：

```cpp
AVFramePtr frame = make_frame();
```

而不必每次重复写 `AVFramePtr(av_frame_alloc())`。

**失败时返回的是持有 `nullptr` 的智能指针**，调用方用 `if (!frame)` 判断即可——**不用改变错误处理风格**。

**为什么标 `inline`？** 因为这是头文件里的函数，多个 .cpp 包含它会产生多个定义，`inline` 允许重复定义（链接器合并）。

### 3.6 工具函数

```cpp
// 把 FFmpeg 的负错误码（如 -1094995529）转成人能看懂的字符串
inline std::string av_err_string(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    return std::string(buf);
}

inline void print_av_error(const char* where, int err)
{
    av_log(nullptr, AV_LOG_ERROR, "%s: %s\n", where, av_err_string(err).c_str());
}
```

#### ★ 为什么必须用 `av_strerror`

**FFmpeg 的错误码不是简单的 `-errno`。**

普通 POSIX 错误码（`-2` = `ENOENT`）能直接用 `strerror`，但 FFmpeg 还定义了**大量自己的错误码**：

```
-1094995529  →  AVERROR_INVALIDDATA  →  "Invalid data found when processing input"
-541478725   →  AVERROR_EOF
```

**看到 `-1094995529` 这种数字，人是无法理解的**，必须转换。

`AV_ERROR_MAX_STRING_SIZE` 是 FFmpeg 提供的缓冲区大小常量（64）——**别自己拍一个数字**。

#### `print_av_error` 的一个细节

```cpp
av_log(nullptr, AV_LOG_ERROR, ...);
```

**第一个参数传 `nullptr`** 表示"用默认的日志上下文"（而不是某个特定的 `AVCodecContext`）。

**好处**：日志会带上 FFmpeg 统一的格式和级别过滤；**比用 `printf` 更好**，因为：

- 用户可以用 `av_log_set_level` 全局调级别；
- 可以用 `av_log_set_callback` 把 FFmpeg 的日志重定向到自己的日志系统。

---

## 四、要注意的问题

### ⚠️ 坑 1：漏写 `extern "C"` → 报"找不到符号"

见 3.1。**报错形态是被修饰过的乱码符号名**（`?av_frame_alloc@@...`），容易误判成"库没链接上"。

### ⚠️ 坑 2：`unique_ptr<AVFrame>` 持有的**不是帧数据**，而是 AVFrame 对象

**这是最容易产生误解的一点。**

```cpp
AVFramePtr frame = make_frame();
frame->width = 1920;
av_frame_get_buffer(frame.get(), 32);   // 分配像素缓冲
// framePtr 析构 → av_frame_free → 释放 AVFrame 对象【和】它挂着的像素缓冲
```

**关键区别在"释放时机"和"释放什么"：**

|                  | 对象本身      | 挂着的像素/采样数据 |
| ---------------- | --------- | ---------- |
| `av_frame_free`  | ✅ 释放      | ✅ 释放       |
| `av_frame_unref` | ❌ 保留（可复用） | ✅ 释放       |

**`AVFramePtr` 的删除器是 `av_frame_free`** —— 所以它下面挂的像素数据也会一起释放。

**而 `FrameQueue` 的槽位复用需要的是 `unref`**（保留对象、只放数据）——所以那里调的是 `av_frame_unref(queue_[rindex_].frame.get())`，**不是**让 `AVFramePtr` 析构。

> **这两个操作在"释放数据"上是相同的，在"释放对象"上不同。**  
> 搞混的后果：要么对象被反复分配（性能），要么数据泄漏。

### ⚠️ 坑 3：`move_ref` 与 `unique_ptr` 是**两个层次**的所有权转移

在 `PacketQueue::put` 里：

```cpp
av_packet_move_ref(&item->pkt, pkt);   // ← 转移的是【packet 内部的 buffer 所有权】
```

**注意这里 `pkt` 是一个裸指针**（调用方从 `AVPacketPtr` 里 `.get()` 出来的）。

**分两层看：**

| 层次                         | 谁在管                                 |
| -------------------------- | ----------------------------------- |
| **C 对象内部的数据**（buffer）      | FFmpeg 的引用计数 + `move_ref` / `unref` |
| **C 对象本身**（`AVPacket` 结构体） | C++ 的 `unique_ptr`                  |

**所以正确的配合方式是：**

```cpp
AVPacketPtr pkt = make_packet();
// ... 填数据 ...
queue.put(pkt.get());       // 转移【内部 buffer】的所有权给队列
// pkt 这个 unique_ptr 还活着，但它指向的是一个"空包"了
```

**`pkt` 出作用域时 `av_packet_free` 会释放那个结构体**——而它已经是空的了，所以不会 double free。

> **理解这两层的分工很重要**：`unique_ptr` 管"结构体的生死"，`move_ref`/`unref` 管"里面数据的生死"。  
> 混在一起想就会觉得"转移之后谁负责释放"很乱。

### ⚠️ 坑 4：`sws_freeContext` 接一级指针（唯一的例外）

见 3.3。**如果照着别的删除器写 `sws_freeContext(&p)`，编译不过**（类型不匹配）。

### ⚠️ 坑 5：`avformat_close_input` 会把指针置空


```cpp
void operator()(AVFormatContext* p) const { if (p) avformat_close_input(&p); }
```

注意这里传的是 `&p`（`p` 是 `operator()` 的**局部参数**）。`avformat_close_input` 释放后会把 `p` 置成 `NULL`——但那是**局部变量的置空**，`unique_ptr` 内部持有的那份由它自己管。

**这是"二级指针"设计的典型形态**：函数内部置空是为了**防止调用方继续用**，但 `unique_ptr` 自己会处理，所以这里置不置空都不影响正确性。

> **一个小细节**：因为 `p` 是**按值传入的局部变量**，所以对它取地址是合法的。
> 如果删除器写成 `operator()(AVFormatContext*& p)`（引用），那传进来的就是 `unique_ptr` 内部成员的引用——
> 也能工作，但语义上更绕。**当前的写法更简单。**

### ⚠️ 坑 6：`#error` 里的版本号要写对

```cpp
#if LIBAVCODEC_VERSION_MAJOR < 61
```

**版本对照表（务必记准）：**

| FFmpeg 版本 | libavcodec 主版本 |
|---|---|
| 7.x | 61 |
| 8.x | 62 |
| 9.x | 63 |

**本机实测（FFmpeg 9.0）：**

```
av_version_info()  : 9.0
libavformat        : 63.1.100
libavcodec         : 63.1.100
libavutil          : 61.1.100
libswresample      : 7.1.100
libswscale         : 10.1.100
编译期版本闸门      : 要求 LIBAVCODEC_VERSION_MAJOR >= 61，实际 63 -> 通过
```

**注意：各个库的主版本号是不同的**（`avcodec=63` 但 `avutil=61`、`swscale=10`）。
**闸门要用 `LIBAVCODEC_VERSION_MAJOR`，不要用 `avutil` 或 `avformat` 的**——因为破坏性变更最集中的是 codec 那一块，而且这个宏的对应关系最稳定。

### ⚠️ 坑 7：`AV_ERROR_MAX_STRING_SIZE` 别自己拍数字

```cpp
char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
```

这是 FFmpeg 提供的常量。**自己写 `char buf[64]` 也行，但如果将来 FFmpeg 改大了，你就截断了。**

**用官方常量 = 自动跟随上游。**

### ⚠️ 坑 8：`av_err_string` 传正数会得到奇怪结果

`av_strerror` 期望的是**负的 FFmpeg 错误码**。传正数或 0 得到的是 `"Success"` 或未定义内容。

**调用前要确认拿到的是负值**——项目里的惯例是：`if (ret < 0) av_err_string(ret)`。

---

## 五、实测验证数据

`test_av_utils.cpp` 共 4 组测试。这一组测试与别的模块不同——**它验证的是"地基"，所以更偏向"证明机制成立"而不是"验证功能正确"**。

### 测试 [1]：版本 + 链接 + DLL 加载

```
av_version_info()  : 9.0
libavformat        : 63.1.100
libavcodec         : 63.1.100
libavutil          : 61.1.100
libswresample      : 7.1.100
libswscale         : 10.1.100
编译期版本闸门     : 要求 LIBAVCODEC_VERSION_MAJOR >= 61，实际 63 -> 通过
```

**这一组一次性验证了三件事：**

1. **头文件找到了**（否则编译不过）；
2. **链接成功了**（否则链接器报找不到符号）；
3. **运行期 DLL 能加载**（否则程序启动就报 `找不到 avcodec-63.dll`）。

**三者是三个不同的失败模式，必须分别验证。**

> 这也解释了为什么工程必须有 `vpl_copy_runtime_dlls` 这个 CMake 函数——
> **"编译链接都过、一运行就找不到 DLL"是 Windows 上最常见的部署问题。**

### 测试 [2]：中途 return 也不泄漏

```
调用 allocateThenFailEarly()...
已分配 pkt=000001F3222602C0  frame=000001F322265680
函数返回 false（这是故意的失败路径）
注意：上方打印的两个地址对应的对象，此刻已经被自动回收。
```

**这一组的设计思路**：构造一个"**分配到一半就失败返回**"的函数，验证资源被自动回收。

**为什么这个场景最重要？** 因为**它正是手工管理最容易出错的地方**：

```c
// 手工管理
AVFrame* frame = av_frame_alloc();
AVPacket* pkt = av_packet_alloc();
if (something_fails()) {
    av_frame_free(&frame);
    return false;              // ← 必须记得释放 pkt 和 frame，少一个就泄漏
}
// ...
```

**每加一条 `return`，就要把前面所有资源都释放一遍**——这是线性增长的维护负担。

**而 RAII 版本**：

```cpp
AVFramePtr frame = make_frame();
AVPacketPtr pkt = make_packet();
if (something_fails())
    return false;              // ← 什么都不用写
```

**加多少条 `return` 都不用改。**

### ★ 测试 [3]：怎么"看见"资源真的被释放了

**这是 M2 里设计得最巧妙的一组。**

#### 难点

`AVFrame` 的释放是**静默的**——`printf` 看不出任何东西。你只能说"我相信析构函数被调用了"，但**没有任何证据**。

#### 技巧：用 `AVBufferRef` 的引用计数

```
1) 给 frame 分配真实缓冲区（引用计数 = 1，只有 frame 持有）
2) 我们自己再 av_buffer_ref 一份（引用计数变 2）
3) 让 frame 出作用域 —— 如果它真被释放了，会把自己那份引用还回去，
   引用计数回到 1，此时我们手上这份就"变成唯一持有者"了
4) 用 av_buffer_is_writable() 就能看出当前是不是"唯一持有者"（=1）
```

**判据：**

| 出作用域后 `is_writable(keepAlive)` | 结论 |
|---|---|
| `true`（计数=1） | **frame 确实释放了** ✅ |
| `false`（计数=2） | **frame 泄漏了** ❌ |

#### 实测输出

```
1920x1080 YUV420P 帧缓冲区已分配
frame 还活着时，缓冲区是唯一持有者吗？   否(计数=2)
framePtr 出作用域后，缓冲区是唯一持有者吗？ 是(计数=1) —— frame 已确实释放
```

**这两行是"确定性证据"，不是"我觉得"。**

> **这个手法的普适价值很高**：**当你想验证一个"静默发生"的事情时，去找一个"会被这件事改变的、可观测的量"。**
>
> 引用计数就是这样一个量——它本来是为了内存管理，但在这里**被借用成了"释放与否的探针"**。
>
> 类似的思路在各处都有：
> - 验证文件句柄释放 → 看 `/proc/PID/fd` 的数量
> - 验证锁释放 → 再尝试加锁看会不会阻塞
> - 验证线程退出 → 看线程数或 `joinable()` 的状态
> - 验证数据被拷走（M12）→ 改原缓冲区看副本变没变

### 测试 [4]：错误码转换 + 二级指针置空

```
avformat_open_input 返回: -2
转成人话: No such file or directory
raw 指针是否已被 FFmpeg 置空: 是
```

**这里验证了一个容易忽略的点**：`avformat_open_input` **失败时也会把指针置空**（这是它接二级指针的意义）。

**如果不知道这一点**，可能会写：

```cpp
AVFormatContext* ic = nullptr;
if (avformat_open_input(&ic, ...) < 0) {
    avformat_close_input(&ic);      // ← 其实没必要，失败时 ic 已经是 NULL
    return -1;
}
```

**`avformat_close_input` 能容忍 `NULL`，所以不会崩，但那是多余的。**

---

## 六、踩坑清单汇总

| 类别 | 坑 | 后果 | 修法 |
|---|---|---|---|
| 链接 | 漏写 `extern "C"` | 报"找不到符号"，符号名是乱码 | FFmpeg 头文件全部包进 `extern "C"` |
| 链接 | 忘了拷运行时 DLL | 编译链接都过，一运行报"找不到 avcodec-63.dll" | 用 `vpl_copy_runtime_dlls` |
| 版本 | 用 FFmpeg 4.x 编译 | 几百条成员不存在的报错，极难定位 | `#error` 闸门 + CMake 双重检查 |
| 版本 | 版本号对照记错 | 闸门数值写错 | 7→61 / 8→62 / 9→63，用 `LIBAVCODEC_VERSION_MAJOR` |
| 资源 | 手工 `free` 漏一条 `return` 路径 | 内存泄漏（流式累积，几秒几个 GB） | 一律 RAII |
| 资源 | 用 `shared_ptr` | 每次拷贝/析构都有原子操作，高频路径上是实打实的开销 | 用 `unique_ptr`（所有权本来就唯一） |
| 资源 | 裸指针浅拷贝 | double free 崩溃 | `unique_ptr`（拷贝直接编译不过） |
| 资源 | 搞混 `av_frame_free` 与 `av_frame_unref` | 对象被反复分配（性能）或数据泄漏 | `free` 释放对象+数据；`unref` 只释放数据 |
| 资源 | 以为 `unique_ptr<AVFrame>` 持有"帧数据" | 对释放时机判断错误 | 它持有的是 **AVFrame 对象**，数据挂在里面 |
| 资源 | 混淆 `move_ref` 与 `unique_ptr` 的所有权层次 | 觉得"转移之后谁负责释放"很乱 | `unique_ptr` 管结构体，`move_ref`/`unref` 管数据 |
| 接口 | `sws_freeContext(&p)` | 编译不过（类型不匹配） | 它是唯一接**一级指针**的释放函数 |
| 接口 | 删除器不判空 | 依赖库实现细节 | `if (p) ...` |
| 接口 | 删除器 `operator()` 不标 `const` | 语法上可行但不表达意图 | 标上 `const` |
| 工具 | 用 `strerror` 处理 FFmpeg 错误码 | 得到无意义结果 | 必须用 `av_strerror` |
| 工具 | `char buf[64]` 硬编码 | 上游改大就截断 | 用 `AV_ERROR_MAX_STRING_SIZE` |
| 工具 | `av_err_string` 传正数 | 得到 `"Success"` 或乱内容 | 只在 `ret < 0` 时调用 |

---

## 七、面试常见追问

**Q：为什么需要 RAII？FFmpeg 的手工管理有什么问题？**

FFmpeg 是纯 C 库，资源是"手工配对"的。问题是**只要有任何一条 `return`/`break`/异常路径漏掉释放，就是内存泄漏**。而播放器的泄漏是**流式累积**的——1080p 一帧 3MB，每秒 25 帧就是 75MB/s，几秒就吃光内存。

RAII 把释放绑在对象生命周期上，**无论从哪条路径出去，析构都会被调用**。而且**加多少条 `return` 都不用改代码**。

**Q：为什么用 `unique_ptr` 而不是 `shared_ptr`？**

三条：

1. **零开销**——`unique_ptr` 大小和裸指针一样，析构就是一次函数调用，没有原子引用计数；`shared_ptr` 有控制块 + 每次操作的原子操作；
2. **所有权唯一**——FFmpeg 对象天然只有一个所有者，用 `shared_ptr` 是在表达一个不存在的语义；
3. **不可拷贝**——**这一条最重要**，它把"两个指针指向同一块内存、释放两次"这类 bug **从运行时崩溃变成了编译错误**。

**Q：删除器为什么写成"结构体 + `operator()`"而不是函数指针？**

因为**函数对象是零开销的**——编译器能内联 `operator()`，生成的代码和手写 `free` 完全一样。
用函数指针的话每次析构都要多一次间接调用，而且**编译器无法内联**。

**Q：为什么不用一个模板统一封装 6 个删除器？**

因为 **`sws_freeContext` 是全 FFmpeg 里唯一接"一级指针"的释放函数**，其他都是接二级指针。

统一处理就得为这个特例加分支（特化或 trait），**复杂度立刻超过"手写 6 个小结构体"**。

> **教训：当"通用方案"需要为每个特例加分支时，就说明它不通用。**

**Q：`unique_ptr<AVFrame>` 析构时会释放帧里的像素数据吗？**

**会。** 它的删除器是 `av_frame_free`，同时释放 AVFrame 对象和它挂着的所有 `AVBufferRef`。

注意区分：`av_frame_unref` **只释放数据、保留对象**（可复用）。`FrameQueue` 的槽位复用要的就是后者，所以那里调的是 `av_frame_unref`，而不是让 `unique_ptr` 析构。

**Q：`extern "C"` 漏了会怎样？**

C++ 编译器会按 C++ 规则修饰 FFmpeg 的函数名，链接时报 `unresolved external symbol "?av_frame_alloc@@"` 之类。

**报错形态是被修饰过的乱码符号名**，容易被误判成"库没链接上"。

**Q：为什么要在头文件里加版本 `#error`？CMake 不是已经查过了吗？**

因为是**两道不同的防线**：CMake 在**配置阶段**查，`#if` 在**编译阶段**查。

`#if` 是防止有人**绕过 CMake 直接编译**（比如写个脚本 `cl main.cpp /I...`）。

**核心想法是**：当"错误的使用方式"会产生**大量难以理解的报错**时，主动加一道门槛，把问题提前到"一句话就能看懂"的位置。

**Q：`av_strerror` 和 `strerror` 有什么区别？**

`strerror` 只认识 POSIX 的 `errno`；FFmpeg 除了 POSIX 错误码，还定义了**大量自己的错误码**（如 `-1094995529` = `AVERROR_INVALIDDATA`）。

**看到 `-1094995529` 这种数字人是无法理解的**，必须用 `av_strerror` 转换。

**Q：怎么验证 RAII 真的生效了？**

用一个**"会被释放这件事改变的、可观测的量"**。测试里用的是 `AVBufferRef` 的引用计数：

1. frame 分配缓冲（计数=1）；
2. 自己再 `av_buffer_ref` 一份（计数=2）；
3. 让 frame 出作用域——如果真释放了，计数回到 1；
4. 用 `av_buffer_is_writable()` 判断是不是唯一持有者。

**这就把"静默的释放"变成了"可观测的计数变化"。**

---

*文档对应代码版本：`learn/src/av_utils.h`（M2 完成态）*
