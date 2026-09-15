#ifndef AV_UTILS_H
#define AV_UTILS_H

// ============================================================================
// av_utils.h —— FFmpeg 资源的 RAII 封装（C++17）
//
// 【本文件在整个项目中的位置】
//   它是全项目的"内存安全地基"，被后面几乎所有模块包含：
//     M6  Decoder    -> 用 AVCodecContextPtr
//     M7  ImageScaler-> 用 SwsContextPtr / SwrContextPtr
//     M8  FFPlayer   -> 用 AVFormatContextPtr / AVPacketPtr / AVFramePtr
//   好处是：业务代码里【再也不出现一次手工 free】，所有 FFmpeg 对象
//   都像 std::vector 一样"到点自动销毁"。
//
// 【为什么需要它】
//   FFmpeg 是纯 C 库，资源管理模型是"手工配对"：
//       AVFormatContext* ic = NULL;
//       avformat_open_input(&ic, ...);   // 分配
//       avformat_close_input(&ic);       // 必须配对释放
//   只要有任何一条 return 路径漏掉释放，就是内存泄漏（播放器一次泄漏
//   一个 AVFrame，几小时就把内存吃光）。RAII 让编译器替我们记住这件事。
// ============================================================================

#include <memory>
#include <string>

// FFmpeg 是 C 库，函数符号没有 C++ 名字修饰（name mangling）。
// 不加 extern "C" 的话，C++ 编译器会按 C++ 规则修饰这些函数名，
// 链接时就会报"找不到符号"。所以 FFmpeg 头文件必须包在 extern "C" 里。
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// ---------------------------------------------------------------------------
// 编译期版本闸门
// ---------------------------------------------------------------------------
// FFmpeg 7 是一次"破坏性升级"，删掉/改掉了一批老 API：
//   * AVCodecContext.channels / channel_layout / AVFrame.channels 被移除
//        -> 统一改用 AVChannelLayout ch_layout
//   * swr_alloc_set_opts() 被移除 -> 改用 swr_alloc_set_opts2()
//   * av_get_default_channel_layout() 被移除
//   * avcodec_decode_video2/audio4 早已删除 -> 统一用 send/receive 新 API
//
// 本项目所有代码都基于新 API。如果有人拿 FFmpeg 4.x 来编译，会得到
// 几百条"结构体没有成员 channels"之类的报错，极难定位根因。
// 所以在头文件顶部用 #error 提前拦住，并直接告诉他原因。
//
// 版本对照：FFmpeg 7.x = libavcodec 61，8.x = 62，9.x = 63
//
// 注意：CMakeLists.txt 里也做了一次版本检查 —— 那是"配置阶段"的防线，
//       这里 #if 是"编译阶段"的防线。双保险，防止有人绕过 CMake 直接编译。
#if LIBAVCODEC_VERSION_MAJOR < 61
#error "本项目要求 FFmpeg 7 及以上版本（推荐 FFmpeg 9），请升级 FFmpeg 开发包。"
#endif

// ---------------------------------------------------------------------------
// RAII 删除器
// ---------------------------------------------------------------------------
// 每个 FFmpeg 对象类型写一个"函数对象"，职责只有一件事：怎么释放它。
//   * 命名为 XxxDeleter，便于阅读；
//   * operator() 标记 const（删除器本身不该被修改）；
//   * 都先判断非空再释放 —— 防止对空指针调用释放函数（虽然多数 FFmpeg
//     释放函数能容忍空指针，但显式判断更安全，也表明我们的意图）。
//
// 为什么用"结构体 + operator()"而不是函数指针？
//   unique_ptr 的默认删除器是 std::default_delete。要换成自定义的，
//   最省事的就是给一个可调用对象类型（函数对象是零开销的，编译器能内联）。
// ---------------------------------------------------------------------------

// 解复用上下文：打开文件/网络流后持有
struct AVFormatContextDeleter {
    void operator()(AVFormatContext* p) const { if (p) avformat_close_input(&p); }
};

// 解码器上下文：avcodec_alloc_context3 分配
struct AVCodecContextDeleter {
    void operator()(AVCodecContext* p) const { if (p) avcodec_free_context(&p); }
};

// 一帧解码后的数据（音频/视频通用）
struct AVFrameDeleter {
    void operator()(AVFrame* p) const { if (p) av_frame_free(&p); }
};

// 一个压缩数据包（解复用的输出、解码的输入）
struct AVPacketDeleter {
    void operator()(AVPacket* p) const { if (p) av_packet_free(&p); }
};

// 图像缩放/像素格式转换上下文
// 【注意】sws_freeContext 是全 FFmpeg 里唯一一个接"一级指针"的释放函数，
//         其他都是接二级指针。好在它不需要把指针置空，正好适配 unique_ptr。
//         这就是我们没用"统一模板"方案的原因（详见课上讨论）。
struct SwsContextDeleter {
    void operator()(SwsContext* p) const { if (p) sws_freeContext(p); }
};

// 音频重采样上下文
struct SwrContextDeleter {
    void operator()(SwrContext* p) const { if (p) swr_free(&p); }
};

// ---------------------------------------------------------------------------
// 智能指针别名
// ---------------------------------------------------------------------------
// 用 unique_ptr 而不是 shared_ptr，理由：
//   1) 零开销 —— 大小和裸指针一样，析构就是一次函数调用，没有原子引用计数；
//   2) 所有权唯一 —— FFmpeg 对象天然只有一个所有者，语义更准确；
//   3) 不可拷贝、只能移动 —— 谁想接管资源必须显式 std::move，
//      杜绝"两个指针指向同一块内存、释放两次"的经典崩溃。
//
// 读法提示：AVFormatContextPtr 就是
//   "一个独占的、到点自动调用 avformat_close_input 的 AVFormatContext*"。
using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
using AVCodecContextPtr  = std::unique_ptr<AVCodecContext,  AVCodecContextDeleter>;
using AVFramePtr         = std::unique_ptr<AVFrame,         AVFrameDeleter>;
using AVPacketPtr        = std::unique_ptr<AVPacket,        AVPacketDeleter>;
using SwsContextPtr      = std::unique_ptr<SwsContext,      SwsContextDeleter>;
using SwrContextPtr      = std::unique_ptr<SwrContext,      SwrContextDeleter>;

// ---------------------------------------------------------------------------
// 便捷工厂
// ---------------------------------------------------------------------------
// av_frame_alloc / av_packet_alloc 分配的是"空壳"（没有任何数据），
// 用它们包一层，业务代码里就可以直接写：
//     AVFramePtr frame = make_frame();
// 而不必每次重复写 AVFramePtr(av_frame_alloc())。
// 失败时返回的是持有 nullptr 的智能指针，调用方用 if (!frame) 判断即可。
inline AVFramePtr  make_frame()  { return AVFramePtr(av_frame_alloc()); }
inline AVPacketPtr make_packet() { return AVPacketPtr(av_packet_alloc()); }

// ---------------------------------------------------------------------------
// 工具函数
// ---------------------------------------------------------------------------

// 把 FFmpeg 的负错误码（如 -1094995529）转成人能看懂的字符串
// （如 "Invalid data found when processing input"）。
// FFmpeg 的错误码不是简单的 -errno，必须用 av_strerror 转换。
inline std::string av_err_string(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    return std::string(buf);
}

// 一条龙：直接打印到 FFmpeg 日志（默认输出到 stderr）。
// 参数 where 是"出错位置"的描述，方便在日志里定位。
inline void print_av_error(const char* where, int err)
{
    av_log(nullptr, AV_LOG_ERROR, "%s: %s\n", where, av_err_string(err).c_str());
}

#endif // AV_UTILS_H
