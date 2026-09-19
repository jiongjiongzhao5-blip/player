#include "decoder.h"

// 析构时确保线程已结束。
// 如果调用方忘了 abort()，这里至少不会让 std::thread 带着"仍可 join"的状态
// 被析构（那会直接 std::terminate 掉整个进程）。
// 注意：只 join 不 abort 是危险的 —— 线程可能永远阻塞在 queue_->get() 上，
//       所以正确的用法是"先 abort，再让对象析构"。
Decoder::~Decoder()
{
    if (thread_.joinable())
        thread_.join();
}

// ---------------------------------------------------------------------------
// 打开解码器
// ---------------------------------------------------------------------------
// 三步走，是所有 FFmpeg 解码器初始化的标准套路：
//   ① avcodec_alloc_context3   —— 分配上下文（传 nullptr 表示"稍后决定用哪个解码器"）
//   ② avcodec_parameters_to_context —— 把流参数（编码格式、分辨率、声道…）拷进去
//   ③ avcodec_find_decoder + avcodec_open2 —— 找到并打开具体解码器
// ---------------------------------------------------------------------------
int Decoder::open(AVStream* stream)
{
    // ① 分配上下文。传 nullptr 是有意的：解码器要等第 ③ 步才根据 codec_id 决定。
    avctx_.reset(avcodec_alloc_context3(nullptr));
    if (!avctx_)
        return AVERROR(ENOMEM);

    // ② 把流的编解码参数搬进上下文。
    //    这一步"看起来像样板代码"，但它不可或缺：分辨率、采样率、声道布局、
    //    以及 H.264 解码必需的 extradata（SPS/PPS）都在里面。
    int ret = avcodec_parameters_to_context(avctx_.get(), stream->codecpar);
    if (ret < 0)
        return ret;

    // ③ 【新版 FFmpeg 的关键一步】显式告知解码器"输入包的时间基"。
    //
    //    pkt_timebase 表示"我喂给你的 AVPacket 的 pts/dts 是以什么为单位的"。
    //    我们喂进去的包直接来自 av_read_frame，所以就是 stream->time_base。
    //
    //    为什么必须设？设置之后，解码器输出的 AVFrame->pts 会统一落在
    //    pkt_timebase 这个时间基上，我们才能据此做换算（比如转成秒）。
    //    不设的话，某些解码器（尤其是带 reorder 的、或硬件解码）会算出
    //    错误甚至无效的帧时间戳。
    //
    //    另一个必须设的场景：用 avcodec_send_packet 送【无时间戳】的裸包时，
    //    解码器需要 pkt_timebase 才能自己推算时间戳。
    avctx_->pkt_timebase = stream->time_base;

    // 找解码器。注意返回类型是【const AVCodec*】——
    // FFmpeg 用 const 表达一个契约："这个结构体是只读的，别改它"。
    // 老版本的 API 返回非 const 指针，很多人会顺手往里面写字段，
    // 那是错的（AVCodec 是全局共享的描述符，一个进程只有一份）。
    const AVCodec* codec = avcodec_find_decoder(avctx_->codec_id);
    if (!codec) {
        av_log(nullptr, AV_LOG_WARNING, "No decoder for %s\n",
               avcodec_get_name(avctx_->codec_id));
        return AVERROR(EINVAL);
    }

    // 打开解码器。第三个参数 options 传 nullptr 表示用默认值
    //（比如"允许帧级多线程解码"这类默认行为）。
    ret = avcodec_open2(avctx_.get(), codec, nullptr);
    if (ret < 0)
        return ret;

    // 从上下文里回读类型，比让调用方再传一遍参数更可靠
    type_ = avctx_->codec_type;
    return 0;
}

// ---------------------------------------------------------------------------
// 启动解码线程
// ---------------------------------------------------------------------------
void Decoder::start(PacketQueue& queue, std::function<void()> worker)
{
    queue_ = &queue;
    // 顺手把上游队列启动起来。
    // 【一个小小的越权】严格说 PacketQueue 什么时候 start 是 FFPlayer 的事，
    // 这里代劳是为了少一处调用点；代价是"看代码时得知道 start 会连带启动队列"。
    queue_->start();
    thread_ = std::thread(std::move(worker));
}

// ---------------------------------------------------------------------------
// 请求退出（两步，缺一不可）
// ---------------------------------------------------------------------------
void Decoder::abort(FrameQueue& frameQueue)
{
    if (queue_)
        queue_->abort();          // ① 置标志位：让阻塞在 get() 的线程退出
    frameQueue.signal();          // ② 叫醒睡在帧队列上的线程
    if (thread_.joinable())
        thread_.join();           // 等它真正结束，之后才能安全销毁对象
    if (queue_)
        queue_->flush();          // 清掉残留的包，避免下次复用时带着旧数据
}

// ---------------------------------------------------------------------------
// ★ 核心：取一帧解码结果
// ---------------------------------------------------------------------------
// 这是"新解码 API"（send/receive 推拉模型）的完整用法，也是本模块最需要
// 反复看的一段。先理解为什么 FFmpeg 要改成这个模型：
//
//   老 API（avcodec_decode_video2）：
//       avcodec_decode_video2(ctx, frame, &got, pkt);   // 一次调用，进一个包出一帧
//     它的致命问题是【无法表达真实情况】：
//       * 一个包可能解出多帧（比如某些容器会把多帧塞进一个包）；
//       * 一帧可能需要多个包才能解出来（需要更多数据）；
//       * 解码器内部可能还有缓存的帧要吐出来，而你没有输入包了 ——
//         老 API 靠"送一个空包"这种别扭手段来表达；
//       * 无法支持"解码器内部并行"。
//
//   新 API 把"送输入"和"取输出"拆成两个独立动作：
//       avcodec_send_packet()    把压缩数据交给解码器（可能只是入队，不产出）
//       avcodec_receive_frame()  从解码器取一帧（可能说"我还需要更多输入"）
//
//   黄金规则（官方文档明确要求）：
//       必须先 receive 直到拿到 AVERROR(EAGAIN) 或一帧，
//       才可以再次 send。反过来也一样：send 返回 EAGAIN 时，
//       说明还有帧没取走，得先 receive。
//
//   所以主循环的形状就固定成：**先反复 receive，receive 说"要更多输入"了
//   才去 send 一个包，然后回到 receive**。下面就是这个形状。
// ---------------------------------------------------------------------------
int Decoder::decodeFrame(AVFrame* frame)
{
    AVPacket pkt{};               // 栈上零初始化：AVPacket 是 C 结构体，没有构造函数

    for (;;) {
        // 上游被 abort 了就直接退出。放在循环顶部，保证每轮都检查一次。
        if (queue_ && queue_->isAborted())
            return -1;

        // ---- 第一步：先尝试取输出 ----
        int ret = avcodec_receive_frame(avctx_.get(), frame);
        if (ret >= 0) {
            // 取到一帧。音频需要做一次时间基换算，视频不需要 —— 原因：
            //
            //   解码器吐出的帧，其 pts 落在 pkt_timebase（= stream->time_base）上。
            //   * 视频：我们将保持这个时间基，由 FFPlayer 乘 av_q2d(videoTb_) 换成秒。
            //     因为视频的"一帧"在时间上没有固定长度（帧率可能变化），
            //     用容器给的时间基最自然。
            //   * 音频：一帧固定对应 nb_samples 个采样，所以【把时间基统一到
            //     1/sample_rate（也就是"采样序号"）】最方便：
            //       - 帧时长直接 = nb_samples / sample_rate，不用查表；
            //       - 与重采样、声卡字节数计算天然对齐；
            //       - ffplay 也是这么做的，保持一致。
            //
            //   用 av_rescale_q（有理数换算，内部用 64 位整数运算）
            //   而不是浮点乘除，避免累积精度误差。
            if (type_ == AVMEDIA_TYPE_AUDIO) {
                const AVRational tb{1, frame->sample_rate};
                if (frame->pts != AV_NOPTS_VALUE)
                    frame->pts = av_rescale_q(frame->pts, avctx_->pkt_timebase, tb);
            }
            return 1;
        }

        // ---- 第二步：分情况处理"没取到" ----
        if (ret == AVERROR_EOF) {
            // 输入已耗尽且解码器内部也排空了。
            // 【必须 flush】否则解码器的内部状态会停在"EOF"，
            // 之后再喂包它也解不出东西 —— 那样 seek 之后就彻底播不动了。
            // flush 把解码器恢复到"刚打开"的状态，可以重新接收数据。
            avcodec_flush_buffers(avctx_.get());
            return 0;
        }
        if (ret != AVERROR(EAGAIN)) {
            // 其他负数都是真错误，记一笔日志。
            // ⚠ 这里【不返回】，而是继续往下走尝试喂包 ——
            //   因为有些解码器在遇到可恢复的错误（比如坏帧）后，
            //   喂下一个包就能继续。真正致命的错误会在后续循环里反复出现，
            //   最终被上层发现。
            av_log(avctx_.get(), AV_LOG_ERROR, "receive_frame: %s\n",
                   av_err_string(ret).c_str());
        }

        // ---- 第三步：解码器要新数据了，从包队列阻塞取一个 ----
        // 返回 -1 表示队列被 abort，直接退出。
        bool isFlush = false;
        if (queue_->get(&pkt, true, &pktSerial_, &isFlush) < 0)
            return -1;

        if (isFlush) {
            // 这是一个 seek 边界标记（不是真实数据包）。
            // 清空解码器内部缓存，然后 continue 回去重新 receive ——
            // 注意【不能】去 send 它，它没有数据。
            avcodec_flush_buffers(avctx_.get());
            continue;
        }

        // ---- 第四步：送包 ----
        ret = avcodec_send_packet(avctx_.get(), &pkt);
        if (ret == AVERROR(EAGAIN)) {
            // 【这个分支正常不该发生】
            // 我们刚刚在第一步 receive 到 EAGAIN 才走到这里，也就是说解码器
            // 的输出已经取空了，send 理应接受。真出现 EAGAIN 说明违反了 API 契约。
            //
            // ⚠ 更麻烦的是下面的 av_packet_unref：send 返回 EAGAIN 时
            //   文档明确说"包没有被消费，应当重发"，此时 unref 会
            //   把这一包数据丢掉（表现为画面/声音出现一次跳变）。
            //   要做到严格正确，应该在 EAGAIN 时不 unref 并跳回 receive；
            //   当前实现选择"记日志 + 继续"，属于偏防御的写法。
            //   考虑到第一步已经排空了输出，这个分支实际不会走到。
            av_log(avctx_.get(), AV_LOG_ERROR,
                   "send/receive both returned EAGAIN (API violation)\n");
        }

        // 送完之后立刻释放包。
        // 这是新 API 的一个便利点：send_packet 内部已经 av_packet_ref 复制了
        // 需要的数据（或直接引用并接管引用计数），调用方送完就能放手。
        // 对比老 API：你得保证包活到 avcodec_decode_video2 返回之后。
        av_packet_unref(&pkt);
    }
}
