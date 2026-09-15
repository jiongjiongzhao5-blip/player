#include <cstdio>

#include <windows.h>

#include "av_utils.h"

extern "C" {
#include <libavutil/buffer.h>
}

static void section(const char* title)
{
    std::printf("\n==================================================\n");
    std::printf(" %s\n", title);
    std::printf("==================================================\n");
}

static bool allocateThenFailEarly()
{
    AVPacketPtr pkt   = make_packet();
    AVFramePtr  frame = make_frame();

    if (!pkt || !frame)
        return false;

    std::printf("  已分配 pkt=%p  frame=%p\n",
                static_cast<void*>(pkt.get()),
                static_cast<void*>(frame.get()));

    return false;
}

static void proveFrameReallyReleased()
{
    AVBufferRef* keepAlive = nullptr;

    {
        AVFramePtr frame = make_frame();

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

    std::printf("  framePtr 出作用域后，缓冲区是唯一持有者吗？ %s\n",
                av_buffer_is_writable(keepAlive) ? "是(计数=1) —— frame 已确实释放"
                                                 : "否(计数=2) —— frame 泄漏了！");

    av_buffer_unref(&keepAlive);   // 我们自己那一份也要还回去
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);

    std::printf("==================================================\n");
    std::printf(" M2 实验台：av_utils.h（FFmpeg RAII 封装）\n");
    std::printf("==================================================\n");

    section("[1] FFmpeg 版本 —— 验证头文件找到 + 链接成功 + DLL 能加载");
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

    section("[2] RAII 生命周期 —— 中途 return 也不泄漏");
    std::printf("  调用 allocateThenFailEarly()...\n");
    const bool ok = allocateThenFailEarly();
    std::printf("  函数返回 %s（这是故意的失败路径）\n", ok ? "true" : "false");
    std::printf("  注意：上方打印的两个地址对应的对象，此刻已经被自动回收。\n");

    section("[3] 硬核验证 —— 用引用计数证明资源真的被释放了");
    proveFrameReallyReleased();

    section("[4] 错误码转可读字符串");
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
