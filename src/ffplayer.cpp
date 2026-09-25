#include "ffplayer.h"

// ============================================================================
// ffplayer.cpp —— 播放器内核（M8：解复用与流打开）
// ============================================================================

FFPlayer::FFPlayer() = default;

FFPlayer::~FFPlayer()
{
    close();
}

// ---------------------------------------------------------------------------
// prepare —— 启动解复用线程
// ---------------------------------------------------------------------------
int FFPlayer::prepare(const std::string& url)
{
    url_    = url;
    abort_  = false;
    eof_    = false;
    durationUs_ = AV_NOPTS_VALUE;

    // 启动消息队列。注意：MessageQueue 默认是终止态（M4 讲的），
    // 不 start 的话所有 post 都会被静默丢弃 —— 上层就永远收不到事件。
    msgQ_.start();

    readThr_ = std::thread(&FFPlayer::readThread, this);

    // 这里【没有】等待文件打开完成。原因见头文件里的说明：
    // 打开文件可能阻塞很久，让它留在 readThread 里，UI 才不会被卡住。
    // 成败通过 msgQ_ 的 Prepared / Error 通知。
    return 0;
}

// ---------------------------------------------------------------------------
// close —— 生命周期里唯一"顺序敏感"的函数
// ---------------------------------------------------------------------------
// 顺序为什么重要？因为我们要在销毁对象【之前】让所有依赖它们的线程停下来。
// 一旦有一个线程还活着并访问了已释放的成员，就是 use-after-free。
//
// 本阶段的顺序（M9/M10 还会往中间插入更多步骤）：
//   ① 置 abort_ 标志 + 唤醒所有阻塞点
//   ② join 线程（等它们真正结束）
//   ③ 清空队列、释放 FFmpeg 资源
//
// 注意 ① 和 ② 之间不能颠倒：只置标志不 join，线程可能还在跑；
// 先 join 不置标志，线程可能永远阻塞在 get() 上 —— 死等。
void FFPlayer::close()
{
    // ① 置终止标志，并唤醒所有可能阻塞的地方。
    abort_ = true;
    msgQ_.abort();       // 唤醒阻塞在 messages().get() 的上层事件线程
    audioQ_.abort();     // 唤醒阻塞在 audioQ_.get() 的解码线程（M9 会有）
    videoQ_.abort();     // 同上，视频侧（M10）

    // ② 等线程真正结束
    if (readThr_.joinable())
        readThr_.join();

    // ③ 清空队列、释放资源
    audioQ_.flush();
    videoQ_.flush();
    ic_.reset();                       // RAII：自动 avformat_close_input

    audioSt_ = videoSt_ = nullptr;
    audioIdx_ = videoIdx_ = -1;
    durationUs_ = AV_NOPTS_VALUE;
    eof_ = false;
}

// ---------------------------------------------------------------------------
// seekMs —— 只是"登记一个请求"
// ---------------------------------------------------------------------------
void FFPlayer::seekMs(int64_t ms)
{
    std::lock_guard<std::mutex> lock(ctlMtx_);
    seekReq_   = true;
    // 统一换算成微秒（AV_TIME_BASE 单位）。
    // av_seek_frame 要求时间戳以 AV_TIME_BASE 为单位，且当 stream_index 传 -1
    // 时这个单位是强制的 —— 所以要显式换算，不能直接传毫秒。
    seekPosUs_ = av_rescale(ms, AV_TIME_BASE, 1000);
}

// ---------------------------------------------------------------------------
// durationMs
// ---------------------------------------------------------------------------
int64_t FFPlayer::durationMs() const
{
    const int64_t us = durationUs_.load();
    return (us == AV_NOPTS_VALUE || us < 0) ? -1 : us / 1000;
}

// ---------------------------------------------------------------------------
// handleSeekRequest —— 由 readThread 调用，真正执行跳转
// ---------------------------------------------------------------------------
// 为什么这个函数放在 readThread 里执行而不是直接在 seekMs 里做？
//   av_seek_frame 和 av_read_frame 共享 AVFormatContext 的内部状态
//（读缓冲、当前位置、索引），跨线程并发调用是未定义行为。
//   让 seek 也由读线程串行执行，就天然避免了竞争，连锁都不用加。
//   这是"单线程串行化"这个老套但极其有效的并发设计手法：
//   **与其给共享状态加锁，不如规定只有一条线程能碰它。**
void FFPlayer::handleSeekRequest()
{
    {
        std::lock_guard<std::mutex> lock(ctlMtx_);
        if (!seekReq_)
            return;
        seekReq_ = false;              // 取走请求，避免重复执行
    }

    const int64_t target = seekPosUs_;

    // AVSEEK_FLAG_BACKWARD：如果目标位置不是关键帧，往前找最近的关键帧。
    // 为什么必须往前？因为从关键帧开始才能正确解码 —— 中间帧依赖前面的参考帧。
    // 代价是实际落点通常比目标略靠前（我们的素材 GOP=12 帧=480ms，
    // 所以 seek 到 2000ms 会落到 1920ms 那个关键帧）。
    if (av_seek_frame(ic_.get(), -1, target, AVSEEK_FLAG_BACKWARD) < 0)
        return;

    // ★ 清空缓存并放入 flush 标记（serial +1）。
    //
    //   reset() 做两件事：
    //     ① 清空队列里所有还没被消费的旧包 —— 它们属于 seek 之前的位置；
    //     ② 压入一个 flush 标记，让解码线程知道"到边界了，
    //        请把解码器内部缓存也刷掉"（M6 讲过解码器有内部延迟）。
    //
    //   如果不做这一步，seek 之后你会先看到几帧"跳回去"的旧画面，
    //   然后才跳到新位置 —— 这是 seek 实现最常见的 bug。
    audioQ_.reset();
    videoQ_.reset();

    eof_ = false;                      // 跳走之后就不是 EOF 状态了
}

// ---------------------------------------------------------------------------
// openComponent —— 打开一路流（解码器 + 记录元信息 + 启动对应队列）
// ---------------------------------------------------------------------------
// M9 会在这里追加：音频输出设备初始化 + 启动音频解码线程
// M10 会在这里追加：启动视频解码线程
int FFPlayer::openComponent(AVStream* stream, AVMediaType type)
{
    // 音频走 audDec_，视频走 vidDec_ —— 用指针选一下，避免写两份重复代码
    Decoder* dec = (type == AVMEDIA_TYPE_AUDIO) ? &audDec_ : &vidDec_;

    const int ret = dec->open(stream);
    if (ret < 0)
        return ret;                    // 打不开解码器，交给调用方决定怎么报错

    // ★ 启动这一路对应的包队列。
    //
    //   为什么必须在这里做？因为 PacketQueue 默认是【终止态】（M3 讲的），
    //   start() 之前所有 put() 都会被静默丢弃。
    //
    //   参考工程把 start() 藏在 Decoder::start() 里，结果 M8 阶段
    //   （还没有解码线程）实测撞出：队列里 0 个包、serial 还是 0，
    //   readThread 投进来的一切都被无声吃掉。
    //   修正是把"队列的启动"归还给创建它的 FFPlayer。
    //
    //   注意执行顺序：先 dec->open() 成功、再 start() 队列。
    //   反过来的话，如果解码器打不开，队列却已经启动、
    //   readThread 会往里灌包而没人消费 → 触发背压把读线程卡住。
    if (type == AVMEDIA_TYPE_AUDIO) {
        audioSt_  = stream;
        audioIdx_ = stream->index;
        audioQ_.start();
    } else {
        videoSt_   = stream;
        videoIdx_  = stream->index;
        videoTb_   = stream->time_base;
        // av_guess_frame_rate 会综合容器声明的帧率和码流里的时基信息
        // 给出最靠谱的帧率 —— 比直接读 stream->avg_frame_rate 稳（有些
        // 容器这两个字段不一致，甚至一个是 0/0）。
        frameRate_ = av_guess_frame_rate(ic_.get(), stream, nullptr);
        videoQ_.start();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// readThread —— 解复用主循环
// ---------------------------------------------------------------------------
// 这是本模块最核心的一段：把文件拆成压缩包，按流分发到两个队列。
// 它同时兼任三件事：打开文件、分发数据、串行处理 seek 请求。
void FFPlayer::readThread()
{
    // ---- 阶段 1：打开输入 ----
    // 注意这里必须传裸指针的地址（二级指针）—— avformat_open_input 要先
    // 分配 AVFormatContext 再交给我们，所以不能用已经分配好的对象。
    // 拿到之后立刻交给 RAII 托管（ic_.reset(raw)），后面就不用管释放了。
    AVFormatContext* raw = nullptr;
    int ret = avformat_open_input(&raw, url_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        av_log(nullptr, AV_LOG_ERROR, "open input failed: %s\n",
               av_err_string(ret).c_str());
        msgQ_.post(FFMsg::Error, ret);
        return;                        // 线程就此结束；上层会收到 Error
    }
    ic_.reset(raw);
    msgQ_.post(FFMsg::OpenInput);

    // ---- 阶段 2：探测流信息 ----
    // avformat_open_input 只读了文件头，还不知道每个流的编码参数
    //（H.264 的 SPS/PPS、AAC 的 AudioSpecificConfig 等）。
    // find_stream_info 会真的去读一段数据来把参数补齐 —— 这是必须的一步，
    // 否则后面 avcodec_open2 会失败。
    ret = avformat_find_stream_info(ic_.get(), nullptr);
    if (ret < 0) {
        av_log(nullptr, AV_LOG_WARNING, "find_stream_info: %s\n",
               av_err_string(ret).c_str());
        msgQ_.post(FFMsg::Error, ret);
        return;
    }
    msgQ_.post(FFMsg::FindStreamInfo);

    if (ic_->duration != AV_NOPTS_VALUE)
        durationUs_.store(ic_->duration);

    // ---- 阶段 3：找音视频流并打开解码器 ----
    //
    // ★★ M8 修掉的一个真 bug（参考工程这里参数传错了位置）★★
    //
    //   av_find_best_stream 的签名是：
    //       (ic, type, wanted_stream_nb, related_stream, decoder_ret, flags)
    //                    ↑ 第3个           ↑ 第4个
    //
    //   参考工程写的是：
    //       av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
    //                           videoIdx >= 0 ? videoIdx : -1,  -1, ...)
    //                           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^   ^^
    //                           这个位置是 wanted_stream_nb      这个才是 related_stream
    //   也就是把 videoIdx 传给了 【wanted_stream_nb】，含义变成了
    //   "我要索引为 videoIdx 的那条音频流"。
    //
    //   而最常见的 mp4 布局是 视频=#0、音频=#1，于是它在找"索引为 0 的音频流"
    //   —— 索引 0 是视频流，自然找不到，返回 AVERROR_STREAM_NOT_FOUND。
    //
    //   实测证据（用同参数直接调 API）：
    //       wanted=-1  related=-1  -> 找到流 #1
    //       wanted=0   related=-1  -> 失败 (-1381258232 Stream not found)  ← 工程传法
    //       wanted=-1  related=0   -> 找到流 #1
    //
    //   后果非常隐蔽：audioIdx 为负 → 音频那一路 openComponent 根本不会被调用
    //   → 音频队列一直是终止态 → 所有音频包被静默丢弃 → **播放器完全没有声音，
    //   而且不报任何错**。这正是 M8 实验里"音频队列 0 个包、serial=0"的根源。
    //
    //   修法：wanted_stream_nb 传 -1（表示自动选择），把 videoIdx 放到
    //   related_stream 上（表达作者本来的意图：优先找与该视频同属一个
    //   节目的音轨；没有 program 信息时它会优雅回退到自动选择）。
    const int videoIdx = av_find_best_stream(ic_.get(), AVMEDIA_TYPE_VIDEO,
                                             -1, -1, nullptr, 0);
    const int audioIdx = av_find_best_stream(ic_.get(), AVMEDIA_TYPE_AUDIO,
                                             -1, videoIdx, nullptr, 0);

    // ★ M8 修正：检查 openComponent 的返回值。
    //
    //   参考工程直接忽略了这个返回值 —— 于是"解码器打不开"这件事
    //   会被完全吞掉：上层收到 Prepared 以为一切正常，结果那一路
    //   永远没有画面/声音，也不报错。
    //
    //   这里的策略是分级的：
    //     · 两路都打不开          -> 致命，post Error 并终止
    //     · 只有一路打不开        -> 打条警告日志，用另一路继续播
    //       （视频能放就放视频、音频能放就放音频，比整个失败更有用）
    int audioOpenErr = 0, videoOpenErr = 0;
    if (audioIdx >= 0)
        audioOpenErr = openComponent(ic_->streams[audioIdx], AVMEDIA_TYPE_AUDIO);
    if (videoIdx >= 0)
        videoOpenErr = openComponent(ic_->streams[videoIdx], AVMEDIA_TYPE_VIDEO);
    msgQ_.post(FFMsg::ComponentOpen);

    if (audioIdx < 0 && videoIdx < 0) {
        msgQ_.post(FFMsg::Error, AVERROR_STREAM_NOT_FOUND);
        return;
    }
    const bool audioUsable = (audioIdx >= 0) && (audioOpenErr == 0);
    const bool videoUsable = (videoIdx >= 0) && (videoOpenErr == 0);
    if (!audioUsable && !videoUsable) {
        // 两路都不行 —— 报第一路遇到的错误
        msgQ_.post(FFMsg::Error, audioOpenErr != 0 ? audioOpenErr : videoOpenErr);
        return;
    }
    if (audioOpenErr != 0 || videoOpenErr != 0) {
        av_log(nullptr, AV_LOG_WARNING,
               "有一路解码器打开失败（audio=%d video=%d），将只播放另一路\n",
               audioOpenErr, videoOpenErr);
    }
    msgQ_.post(FFMsg::Prepared);       // ★ 到这里才算"准备好了"

    // ---- 阶段 4：分发循环 ----
    AVPacketPtr pkt = make_packet();
    while (!abort_.load()) {
        handleSeekRequest();           // 每次都先看看有没有待处理的 seek

        ret = av_read_frame(ic_.get(), pkt.get());
        if (ret < 0) {
            // 读到结尾（或出错）
            if (ret == AVERROR_EOF || avio_feof(ic_->pb)) {
                // ★★ 这是 M6 发现并修复的那个缺陷 ★★
                //
                //   原来的实现只 post 一条 Completed 就 continue 了，从不给
                //   队列投"结束信号"。后果（M6 实验台实测）：
                //     · 解码器永远不会进入 draining 模式，尾部约 2 帧画面
                //       永远不显示；
                //     · Decoder::decodeFrame 里的 AVERROR_EOF 分支永远走不到；
                //     · 解码线程会一直阻塞在 queue_->get() 直到 abort。
                //
                //   修复：给两路队列各投一个"空包"（size==0、data==nullptr）。
                //   avcodec_send_packet 收到 size==0 的包 = "没有更多输入了"，
                //   解码器随即进入 draining，把内部缓存的帧全部吐出来。
                //
                //   用 eof_.exchange(true) 保证只投一次 —— 否则下面那个
                //   continue 会让我们每 10ms 就再投一个空包。
                if (!eof_.exchange(true)) {
                    if (audioIdx_ >= 0) audioQ_.putNullPacket(audioIdx_);
                    if (videoIdx_ >= 0) videoQ_.putNullPacket(videoIdx_);
                    msgQ_.post(FFMsg::Completed);
                }
            }
            // pb->error 非零说明是真的 IO 错误（不是正常读完），直接退出循环。
            if (ic_->pb && ic_->pb->error)
                break;

            // 【一个可以改进的点】EOF 之后这里会以 10ms 周期空转，直到
            // abort。为什么不直接 break 退出？
            //   因为 seek 之后我们要能继续读 —— 所以循环必须活着来响应
            //   seek 请求。10ms 一次的开销极小（约 100 次/秒的空循环），
            //   但更优雅的写法是"阻塞在一个条件变量上，等 seekMs 来唤醒"。
            //   当前实现选择简单，代价是 EOF 后有一点无谓的空转。
            av_usleep(10 * 1000);
            continue;
        }

        // 读到了新包，说明不在 EOF 状态了
        eof_ = false;

        // ---- 按流分发 ----
        // 只保留音视频两路，其余（字幕、数据流等）直接丢弃。
        // put() 内部会 av_packet_move_ref 转移所有权，所以不需要我们再 unref。
        if (pkt->stream_index == audioIdx_)
            audioQ_.put(pkt.get());
        else if (pkt->stream_index == videoIdx_)
            videoQ_.put(pkt.get());
        else
            av_packet_unref(pkt.get());   // 丢弃：必须显式 unref 才不会泄漏
    }
}
