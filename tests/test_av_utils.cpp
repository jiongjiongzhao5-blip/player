// ============================================================================
// tests/test_av_utils.cpp —— M2 实验台：验证 av_utils.h 的 RAII 封装
//
// 这个文件不用 Qt，纯粹是命令行程序，目的是把 av_utils.h 的行为"跑出来看"。
// 它属于学习沙盒，最终工程里没有它。
// ============================================================================

#include <cstdio>

#include <windows.h>          // 只为了 SetConsoleOutputCP

#include "av_utils.h"

extern "C" {
#include <libavutil/buffer.h>   // av_buffer_ref / av_buffer_is_writable
}

// ---------------------------------------------------------------------------
// 小工具：打印分节标题，让输出好看点
// ---------------------------------------------------------------------------
static void section(const char* title)
{
    std::printf("\n==================================================\n");
    std::printf(" %s\n", title);
    std::printf("==================================================\n");
}

// ---------------------------------------------------------------------------
// [2] 演示：中途失败提前 return，RAII 照样把资源收干净
//
// 对比一下"老写法"有多难写对：
//
//   bool oldWay() {
//       AVPacket* pkt = av_packet_alloc();
//       AVFrame*  frm = av_frame_alloc();
//       if (!pkt || !frm) {              // 这里就得开始清理了
//           av_packet_free(&pkt);        // 少写一行就泄漏
//           av_frame_free(&frm);
//           return false;
//       }
//       if (allocMore() < 0) {           // 每多一个失败点，清理代码就翻一倍
//           av_packet_free(&pkt);
//           av_frame_free(&frm);
//           return false;
//       }
//       ...  // 正常路径还要再写一遍释放
//   }
//
// 用 RAII 之后，函数里一个 free 都不需要出现。
// ---------------------------------------------------------------------------
static bool allocateThenFailEarly()
{
    AVPacketPtr pkt   = make_packet();     // ① 分配，出作用域自动释放
    AVFramePtr  frame = make_frame();      // ② 分配，出作用域自动释放

    if (!pkt || !frame)
        return false;                      // ← 提前返回，不会泄漏

    std::printf("  已分配 pkt=%p  frame=%p\n",
                static_cast<void*>(pkt.get()),
                static_cast<void*>(frame.get()));

    // 模拟"后面某一步失败了"，直接 return。
    // 不需要写任何清理代码：pkt / frame 的析构函数会依次调用
    //   av_packet_free(&p)  和  av_frame_free(&p)
    return false;
}

// ---------------------------------------------------------------------------
// [3] 硬核验证：怎么"看见"资源真的被释放了
//
// 难点：AVFrame 的释放是静默的，printf 看不出任何东西。
//
// 技巧：AVFrame 的实际像素/采样数据放在 AVBufferRef 里，而 AVBufferRef
//       是【带引用计数】的。做法是：
//         1) 给 frame 分配真实缓冲区（引用计数 = 1，只有 frame 持有）；
//         2) 我们自己再 av_buffer_ref 一份（引用计数变 2）；
//         3) 让 frame 出作用域 —— 如果它真被释放了，会把自己那份引用还回去，
//            引用计数回到 1，此时我们手上这份就"变成唯一持有者"了。
//
//       用 av_buffer_is_writable() 就能看出当前是不是"唯一持有者"（=1）。
//       于是：出作用域后 is_writable 变 true  →  frame 确实释放了。
//            出作用域后 is_writable 还是 false →  frame 泄漏了。
// ---------------------------------------------------------------------------
static void proveFrameReallyReleased()
{
    AVBufferRef* keepAlive = nullptr;   // 我们自己额外持有的一份引用

    {
        AVFramePtr frame = make_frame();

        // 给帧一个真实的视频格式和尺寸，才能分配缓冲区
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width  = 1920;
        frame->height = 1080;

        const int ret = av_frame_get_buffer(frame.get(), 32);
        if (ret < 0) {
            std::printf("  av_frame_get_buffer 失败: %s\n", av_err_string(ret).c_str());
            return;
        }
        std::printf("  1920x1080 YUV420P 帧缓冲区已分配\n");

        keepAlive = av_buffer_ref(frame->buf[0]);   // 引用计数 1 -> 2

        std::printf("  frame 还活着时，缓冲区是唯一持有者吗？ %s\n",
                    av_buffer_is_writable(keepAlive) ? "是(计数=1)" : "否(计数=2)");
    }
    // ← ← ← 就是这一行：作用域结束，AVFramePtr 析构，av_frame_free 被调用

    std::printf("  framePtr 出作用域后，缓冲区是唯一持有者吗？ %s\n",
                av_buffer_is_writable(keepAlive) ? "是(计数=1) —— frame 已确实释放"
                                                 : "否(计数=2) —— frame 泄漏了！");

    av_buffer_unref(&keepAlive);   // 我们自己那一份也要还回去
}

int main()
{
    // Windows 控制台默认用 GBK 解码输出字节，而我们的源码是 UTF-8，
    // 不切码页的话中文会显示成乱码。切到 UTF-8 代码页(65001)即可。
    // （如果输出被重定向到管道/文件，这行无效，但管道本来就按 UTF-8 读，也对。）
    SetConsoleOutputCP(CP_UTF8);

    std::printf("==================================================\n");
    std::printf(" M2 实验台：av_utils.h（FFmpeg RAII 封装）\n");
    std::printf("==================================================\n");

    // -----------------------------------------------------------------------
    section("[1] FFmpeg 版本 —— 验证头文件找到 + 链接成功 + DLL 能加载");
    // av_version_info() 是【运行时】函数，它在 DLL 里。
    // 能打印出来，说明三件事都成立：头文件可访问、库链接成功、DLL 已就位。
    // -----------------------------------------------------------------------
    std::printf("  av_version_info()  : %s\n", av_version_info());
    std::printf("  libavformat        : %u.%u.%u\n",
                LIBAVFORMAT_VERSION_MAJOR, LIBAVFORMAT_VERSION_MINOR, LIBAVFORMAT_VERSION_MICRO);
    std::printf("  libavcodec         : %u.%u.%u\n",
                LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR, LIBAVCODEC_VERSION_MICRO);
    std::printf("  libavutil          : %u.%u.%u\n",
                LIBAVUTIL_VERSION_MAJOR, LIBAVUTIL_VERSION_MINOR, LIBAVUTIL_VERSION_MICRO);
    std::printf("  libswresample      : %u.%u.%u\n",
                LIBSWRESAMPLE_VERSION_MAJOR, LIBSWRESAMPLE_VERSION_MINOR, LIBSWRESAMPLE_VERSION_MICRO);
    std::printf("  libswscale         : %u.%u.%u\n",
                LIBSWSCALE_VERSION_MAJOR, LIBSWSCALE_VERSION_MINOR, LIBSWSCALE_VERSION_MICRO);
    std::printf("  编译期版本闸门     : 要求 LIBAVCODEC_VERSION_MAJOR >= 61，"
                "实际 %u -> 通过\n", LIBAVCODEC_VERSION_MAJOR);

    // -----------------------------------------------------------------------
    section("[2] RAII 生命周期 —— 中途 return 也不泄漏");
    // -----------------------------------------------------------------------
    std::printf("  调用 allocateThenFailEarly()...\n");
    const bool ok = allocateThenFailEarly();
    std::printf("  函数返回 %s（这是故意的失败路径）\n", ok ? "true" : "false");
    std::printf("  注意：上方打印的两个地址对应的对象，此刻已经被自动回收。\n");

    // -----------------------------------------------------------------------
    section("[3] 硬核验证 —— 用引用计数证明资源真的被释放了");
    // -----------------------------------------------------------------------
    proveFrameReallyReleased();

    // -----------------------------------------------------------------------
    section("[4] 错误码转可读字符串");
    // -----------------------------------------------------------------------
    // avformat_open_input 打开一个不存在的文件，拿一个真实的错误码回来。
    // 顺带说明：失败时 avformat_open_input 会自己把上下文释放并把指针置空，
    //           所以这里用裸指针接是安全的（这也是唯一一个例外——它自己管自己）。
    {
        AVFormatContext* raw = nullptr;
        const int ret = avformat_open_input(&raw, "no_such_file_12345.mp4", nullptr, nullptr);
        std::printf("  avformat_open_input 返回: %d\n", ret);
        std::printf("  转成人话: %s\n", av_err_string(ret).c_str());
        std::printf("  raw 指针是否已被 FFmpeg 置空: %s\n", raw == nullptr ? "是" : "否");
    }

    std::printf("\n==================================================\n");
    std::printf(" 全部测试执行完毕，程序即将退出。\n");
    std::printf(" 退出后你可以观察：没有崩溃、没有异常，资源全部自动回收。\n");
    std::printf("==================================================\n");

    return 0;
}
