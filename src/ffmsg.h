#ifndef FFMSG_H
#define FFMSG_H

// ============================================================================
// ffmsg.h —— 播放器内核的内部消息类型定义
//
// 【在项目里的位置】
//   这是"内核 → 上层"这条单向通道的"协议定义"。内核里的各条线程
//   （读线程、解码线程）通过 MessageQueue 投递这些事件，
//   MediaPlayer 的事件线程取出后转成 Qt 信号。
//
//   为什么要单独一个头文件？因为消息类型是【内核和上层都要用的约定】：
//   内核负责 post，上层负责 switch-case 分发。协议独立成文件，
//   双方都只依赖它，不互相依赖。
//
// 【为什么用 enum class 而不是老工程的 #define】
//   老工程（ffmsg.h）写的是：
//       #define FFP_MSG_FLUSH            0
//       #define FFP_MSG_ERROR            100
//       ...
//   宏的问题：
//     1) 不是类型 —— 编译器和 IDE 帮不了你，写错名字只能等到运行时才炸；
//     2) 污染全局命名空间 —— FFP_MSG_ERROR 在整个工程里都是可见的；
//     3) 不能作为函数重载的依据，也不能放进 std::map 做键（没有类型）；
//     4) 调试器里看到的只是一串数字，看不到符号名。
//   enum class 全部解决：有作用域（FFMsg::Prepared）、有类型、
//   禁止隐式转换（想当 int 用必须显式 static_cast，避免误用）。
//
// 【为什么显式写 ": int"】
//   enum class 的默认底层类型就是 int，这里写出来是三个目的：
//     1) 明确表达"我要把它当整数用"（要存进 Message::what、要和 arg1 一起传）；
//     2) 锁定 ABI —— 万一将来编译器默认值变了，行为不会悄悄改变；
//     3) 万一日后想省内存，可以改成 : uint16_t，一眼就能看到改动点。
// ============================================================================

enum class FFMsg : int {
    // ---- 0：队列复位哨兵 ----
    FLUSH = 0,

    // ---- 100 段：错误与事件类（内核 -> 上层）----
    //   arg1 / arg2 是附加参数，含义随 what 而定
    Error                = 100,   // arg1 = FFmpeg 错误码
    Prepared             = 200,   // 打开/解码就绪，可以开始播放
    Completed            = 300,   // 播放完成（读到 EOF）

    // 下面这些是从 ijkplayer 继承的"预留事件"。
    // 本项目当前【没有投递也没有消费】它们，但保留着：一旦要做
    // "播放中显示分辨率变化""音频起播提示"这类功能，直接往里填就行，
    // 不用改动已有消息的编号（这正是下一段"留空隙"的好处）。
    VideoSizeChanged     = 400,   // arg1 = width, arg2 = height
    SarChanged           = 401,   // arg1 = sar.num, arg2 = sar.den
    VideoRenderingStart  = 402,   // 首帧视频即将上屏
    AudioRenderingStart  = 403,   // 首帧音频即将出声
    VideoRotationChanged = 404,   // arg1 = 旋转角度
    AudioDecodedStart    = 405,
    VideoDecodedStart    = 406,

    // 这三个是"进程日志型"事件：内核会投递，但当前上层不消费
    // （switch 里落到 default 分支）。留着是因为做"加载进度提示"
    // 这类体验优化时会很有用。
    OpenInput            = 407,   // 已调用 avformat_open_input
    FindStreamInfo       = 408,   // 已调用 avformat_find_stream_info
    ComponentOpen        = 409,   // 已调用 stream_component_open

    // ---- 20000 段：请求类消息（上层 -> 内核）----
    // 设计上与"事件类"之间留了 19000 多的空隙，将来加新的请求类消息
    // 不会撞号，也不会让"按区间判断类别"的代码失效。
    //
    // 本项目当前的实现里【没有使用】这三个：MediaPlayer 是直接调用
    // player_->setPaused() / seekMs() 这类成员函数的。
    // 留着是为了兼容 ijkplayer 的"一切通过消息队列"的架构风格 ——
    // 那种风格把控制流也串行化进同一个队列，能天然避免控制命令与
    // 数据流的竞态，代价是多一层排队延迟。
    ReqStart = 20001,             // 内核已就绪，请求开始播放
    ReqPause = 20002,             // UI 请求暂停
    ReqSeek  = 20003,             // UI 请求 seek
};

#endif // FFMSG_H
