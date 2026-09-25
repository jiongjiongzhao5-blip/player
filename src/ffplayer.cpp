#include "ffplayer.h"

#include <algorithm>
#include <cstring>

// ============================================================================
// ffplayer.cpp —— 播放器内核（M9：音频链路 + 音频主时钟）
// ============================================================================

namespace {
// 队列容量（与 ffplay 保持一致）
constexpr int kVideoQueueSize  = 3;    // 视频帧队列只开 3 个槽（M3 讲过原因）
constexpr int kSampleQueueSize = 9;    // 音频帧小，可以多囤一点

// 视频刷新的【基准】节拍（秒）。注意它不是"帧间隔"，而是"轮询间隔" ——
// 真实睡眠时长会由 videoRefresh 按需缩短，详见那里的说明。
constexpr double kRefreshInterval = 0.01;

// 迟到多久就算"这一帧已经没意义了"，直接丢掉。
// ffplay 用 AV_SYNC_THRESHOLD_MAX = 0.1s 作为"还能忍"的界限，这里取同一个值。
// ⚠ 这是参考工程【没有】的改进，见 videoRefresh 里的说明。
constexpr double kLateDropThreshold = 0.1;
}  // namespace

FFPlayer::FFPlayer()
    : sampQ_(kSampleQueueSize)
    , pictQ_(kVideoQueueSize)
{
}

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
    paused_ = false;
    eof_    = false;
    completedPosted_ = false;
    durationUs_ = AV_NOPTS_VALUE;

    // ★ 把帧队列和它上游的包队列"绑"起来。
    //
    //   为什么要绑？因为 FrameQueue 在"没有空槽可写 / 没有数据可读"时会阻塞，
    //   而它必须能被 abort 唤醒。绑上以后，它的等待条件里就多了一条
    //   "上游包队列已终止"，一次 abort 就能把整条流水线上的线程全叫醒。
    //   （M3 实验台 [6] 专门验证过这个"两步唤醒"机制。）
    sampQ_.setPacketQueue(&audioQ_);
    pictQ_.setPacketQueue(&videoQ_);

    audClk_.init();                 // 时钟复位成 NaN（未锚定状态）
    videoClockInit_ = false;
    completedPosted_ = false;
    msgQ_.start();

    readThr_ = std::thread(&FFPlayer::readThread, this);
    // ★ 刷新线程在 prepare 里就启动（不等文件打开）——
    //   因为它每轮开头就检查"视频流存在吗、暂停吗、有帧吗"，
    //   条件不满足就直接返回，空转成本极低。早点启动可以让代码更简单：
    //   不需要在 openComponent 里判断"线程是否已经起过"。
    refreshThr_ = std::thread(&FFPlayer::videoRefreshThread, this);

    // 注意这里【没有】等音频设备打开、也没等解码线程起来 ——
    // 那些都在 readThread → openComponent 里做（异步）。
    // 上层靠 Prepared 消息判断"可以开始播了"。
    return 0;
}

// ---------------------------------------------------------------------------
// close —— 优雅停机（顺序敏感）
// ---------------------------------------------------------------------------
// 顺序的道理只有一句话：**先让所有可能阻塞的点都能被唤醒，再逐个关闭
// 输出通道，最后 join 线程。** 反过来做（比如先 join 再 abort），
// 线程可能永远阻塞在某个 get() 上，join 就变成死等。
void FFPlayer::close()
{
    abort_ = true;

    // ① 唤醒所有阻塞点
    msgQ_.abort();
    audioQ_.abort();
    videoQ_.abort();
    sampQ_.signal();        // 叫醒睡在音频帧队列上的线程（M3 讲的"第二步"）
    pictQ_.signal();

    // ② 先关音频设备。
    //    ★ 这一步必须在 join 解码线程【之前】做。
    //      SDL_DestroyAudioStream 会同步等待音频回调退出，所以返回之后
    //      pullAudio() 一定不会再被调用。如果不先关它，那个回调可能正
    //      阻塞在 sampQ_.peekReadable() 上 —— 虽然我们 signal 过，
    //      但让"已经没人会再进来"变成确定事实，比推理它更可靠。
    audioDev_.close();

    // ③ join 线程
    if (readThr_.joinable())    readThr_.join();
    if (refreshThr_.joinable()) refreshThr_.join();

    // ④ 停解码器（内部会 abort 队列 + signal 帧队列 + join 解码线程）
    audDec_.abort(sampQ_);
    vidDec_.abort(pictQ_);

    // ⑤ 清队列、释放 FFmpeg 资源
    audioQ_.flush();
    videoQ_.flush();
    sampQ_.flush();
    pictQ_.flush();

    ic_.reset();
    swr_.reset();
    av_channel_layout_uninit(&srcLayout_);
    av_channel_layout_uninit(&tgtLayout_);

    audioSt_ = videoSt_ = nullptr;
    audioIdx_ = videoIdx_ = -1;
    audioBuf_ = nullptr;            // 兼作"缓冲是否有效"的标志
    audioBufSize_ = audioBufIndex_ = 0;
    audioClock_ = std::nan("");
    resampleBuf_.clear();
    videoClockInit_ = false;
    completedPosted_ = false;
    eof_ = false;
}

// ---------------------------------------------------------------------------
// seekMs —— 只是"登记一个请求"
// ---------------------------------------------------------------------------
void FFPlayer::seekMs(int64_t ms)
{
    std::lock_guard<std::mutex> lock(ctlMtx_);
    seekReq_   = true;
    seekPosUs_ = av_rescale(ms, AV_TIME_BASE, 1000);
}

// ---------------------------------------------------------------------------
// 播放控制
// ---------------------------------------------------------------------------

// ★ 这个函数里有一处容易被忽略、但直接决定"暂停久了会不会不同步"的处理。
void FFPlayer::setPaused(bool paused)
{
    paused_ = paused;
    audioDev_.setPaused(paused);        // 让声卡停/走

    // ★ 时钟必须跟着一起冻结/恢复。
    //
    //   参考工程写的是：
    //       if (!paused && !std::isnan(audioClock_))
    //           audClk_.set(audioClock_);
    //   只处理了"恢复时重锚定"，没有处理"暂停时冻结" —— 于是：
    //     · 暂停期间没人调 set()，而 get() 靠系统时间外推，
    //       位置会继续往前涨（实测暂停 600ms，位置涨了 601ms）；
    //     · 恢复时又用 audioClock_（最后一帧的 PTS，比真实播放头还靠前）
    //       重新锚定，于是位置"跳回去"一截。
    //   两个 bug 合起来的表现就是：暂停时进度条还在跑，一恢复又倒退。
    //
    //   M9 的修法是把这两件事都收进 Clock::setPaused()：
    //   暂停时 get() 直接返回冻结值，恢复时以冻结值为基准重新锚定。
    //   一次调用，语义完整。
    audClk_.setPaused(paused);
}

// 音量：界面上的 0~100 在 M12 换算成 0.0~1.0 传进来。
// 校验交给 AudioDevice（内部有 std::clamp）。
void FFPlayer::setVolume(float linear01) { audioDev_.setVolume(linear01); }

// 倍速。
//
// ★ M11 补上了欠了两轮的那一行：`audClk_.setSpeed(ratio)`。
//
//   参考工程只调了 AudioDevice（SDL 的频率比），忘了调时钟 ——
//   M5 发现时我把它记成了"未接线"，现在补上。
//
//   为什么必须有这一行？
//     时钟的值是 get() = pts_drift + now * speed 算出来的，也就是
//     "锚定值 + 流逝时间 × 倍速"。如果 speed 永远是 1，那么两次锚定之间
//     时钟只按 1 倍速外推 —— 而 2 倍速播放时媒体位置每墙钟秒前进 2 秒，
//     时钟就会一直落在后面，视频会认为"我还早"，于是画面越拖越晚。
//
//   没有它的时候，2 倍速下画面会明显滞后于声音。
void FFPlayer::setSpeed(float ratio)
{
    // 防御：非正倍速没有意义。SDL 的频率比传 0 或负数行为未定义
    // （可能变成"倒放"或直接失败），所以在入口就挡掉。
    if (ratio <= 0.0f)
        return;

    audioDev_.setSpeed(ratio);   // 让声卡按倍速消耗 PCM（会变调，见 M7 的说明）
    audClk_.setSpeed(ratio);     // ★ 让时钟也按倍速外推（并且在切换瞬间保持读数连续）
}

double FFPlayer::positionSeconds() const
{
    const double c = audClk_.get();
    // 未锚定时 get() 返回 NaN，这里统一成 0 ——
    // 界面层不需要知道 NaN 这个概念，它只知道"位置是 0"。
    return std::isnan(c) ? 0.0 : std::max(0.0, c);
}

int64_t FFPlayer::durationMs() const
{
    const int64_t us = durationUs_.load();
    return (us == AV_NOPTS_VALUE || us < 0) ? -1 : us / 1000;
}

// ---------------------------------------------------------------------------
// 沙盒统计
// ---------------------------------------------------------------------------
FFPlayer::AudioOutInfo FFPlayer::audioOutInfo() const
{
    std::lock_guard<std::mutex> lock(statMtx_);
    AudioOutInfo info;
    info.targetSampleRate = audioDev_.sampleRate();
    info.targetChannels   = audioDev_.channels();
    info.decodedFrames    = statDecodedFrames_;
    info.swrRebuilds      = statSwrRebuilds_;
    info.pcmBytes         = statPcmBytes_;
    info.pcmPeak          = statPcmPeak_;
    return info;
}

FFPlayer::DemuxInfo FFPlayer::demuxInfo() const
{
    DemuxInfo info;
    info.audioPkts = statAudioPkts_.load();
    info.videoPkts = statVideoPkts_.load();
    info.nullPkts  = statNullPkts_.load();
    return info;
}

// ---------------------------------------------------------------------------
// handleSeekRequest —— 由 readThread 执行（M8 已讲）
// ---------------------------------------------------------------------------
void FFPlayer::handleSeekRequest()
{
    {
        std::lock_guard<std::mutex> lock(ctlMtx_);
        if (!seekReq_)
            return;
        seekReq_ = false;
    }

    const int64_t target = seekPosUs_;
    if (av_seek_frame(ic_.get(), -1, target, AVSEEK_FLAG_BACKWARD) < 0)
        return;

    audioQ_.reset();
    videoQ_.reset();
    sampQ_.flush();
    pictQ_.flush();
    // 纯视频时让外部时钟在 seek 后重新锚定到新位置的 PTS。
    // 这不只是为了"跳到新位置"，更是为了兜住"队列里可能混进残留旧帧"
    // 的情况 —— 详见 videoRefresh 里对条件 ③ 的说明。
    videoClockInit_ = false;
    completedPosted_ = false;
    // seek 之后要允许重新通知"播放结束"（比如 seek 到接近结尾）
    eof_ = false;

    // ★★ M11 补上：暂停状态下 seek，要主动把时钟挪到新位置。
    //
    //   为什么需要？平时时钟是被【音频回调】反复锚定的：
    //     pullAudio 每取一帧就 set(当前播放头) 一次。
    //   但暂停时音频回调根本不跑（SDL 设备停了），于是没有任何人
    //   去更新时钟 —— 它的读数会一直停在暂停前的位置。
    //
    //   后果：用户在【暂停状态】下拖动进度条，声音画面确实跳到新位置了，
    //   可界面上显示的播放时间和进度条纹丝不动。恢复播放的瞬间才会
    //   猛地跳到新位置 —— 体验上像是"拖了没反应，一按播放才生效"。
    //
    //   修法：seek 执行完，如果当前是暂停态，就直接用 seek 目标位置
    //   锚定时钟。之后恢复播放时，音频回调会再用真实 PTS 修正一次。
    if (paused_.load() && target != AV_NOPTS_VALUE)
        audClk_.set(static_cast<double>(target) / AV_TIME_BASE);
}

// ---------------------------------------------------------------------------
// openComponent —— 打开一路流
// ---------------------------------------------------------------------------
int FFPlayer::openComponent(AVStream* stream, AVMediaType type)
{
    Decoder* dec = (type == AVMEDIA_TYPE_AUDIO) ? &audDec_ : &vidDec_;

    const int ret = dec->open(stream);
    if (ret < 0)
        return ret;

    if (type == AVMEDIA_TYPE_AUDIO) {
        AVCodecContext* c = audDec_.ctx();

        // ★ 确定"输出格式"：本项目固定为 ——
        //     采样率 = 源采样率、声道布局 = 源布局、采样格式 = S16（交织）
        //   也就是"除了采样格式，其余尽量不动"。
        //   为什么不统一重采样到 48kHz？因为那会引入一次不必要的质量损失
        //   和 CPU 开销。声卡对 44.1k 和 48k 都能直接播，保持源采样率最省事。
        tgtFreq_ = c->sample_rate;
        av_channel_layout_uninit(&tgtLayout_);
        av_channel_layout_copy(&tgtLayout_, &c->ch_layout);

        // ★ 打开 SDL 音频设备，并把"拉数据"的回调接到 pullAudio 上。
        //   注意这里传的是 lambda，捕获 this —— 回调里会调 FFPlayer 的成员。
        //   生命周期上这是安全的：close() 里先 audioDev_.close() 再销毁对象，
        //   而且 close() 会等回调彻底退出。
        if (!audioDev_.open(tgtFreq_, tgtLayout_.nb_channels,
                            [this](uint8_t* d, int b) { return pullAudio(d, b); }))
            return -1;              // 设备打不开 → 这一路算失败

        audioSt_  = stream;
        audioIdx_ = stream->index;
        audioQ_.start();            // ★ 队列的启动归 FFPlayer（M8 的修正）

        // 启动音频解码线程。主循环体由这里提供（"策略注入"）。
        // 前提：audioQ_ 已经 start 过。
        audDec_.start(audioQ_, [this] { audioDecodeThread(); });

    } else {
        videoSt_   = stream;
        videoIdx_  = stream->index;
        videoTb_   = stream->time_base;
        frameRate_ = av_guess_frame_rate(ic_.get(), stream, nullptr);
        videoQ_.start();

        // 启动视频解码线程（与音频同样的"策略注入"模式）
        vidDec_.start(videoQ_, [this] { videoDecodeThread(); });
    }
    return 0;
}

// ---------------------------------------------------------------------------
// readThread —— 解复用主循环（M8 已实现，这里保持原样）
// ---------------------------------------------------------------------------
void FFPlayer::readThread()
{
    AVFormatContext* raw = nullptr;
    int ret = avformat_open_input(&raw, url_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        av_log(nullptr, AV_LOG_ERROR, "open input failed: %s\n",
               av_err_string(ret).c_str());
        msgQ_.post(FFMsg::Error, ret);
        return;
    }
    ic_.reset(raw);
    msgQ_.post(FFMsg::OpenInput);

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

    // ★ 这里修掉了参考工程的一个真 bug：第 3 个参数是 wanted_stream_nb、
    //   第 4 个才是 related_stream，参考工程把 videoIdx 传错了位置，
    //   导致音频流永远找不到（详见 M8 的说明）。
    const int videoIdx = av_find_best_stream(ic_.get(), AVMEDIA_TYPE_VIDEO,
                                             -1, -1, nullptr, 0);
    const int audioIdx = av_find_best_stream(ic_.get(), AVMEDIA_TYPE_AUDIO,
                                             -1, videoIdx, nullptr, 0);

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
        msgQ_.post(FFMsg::Error, audioOpenErr != 0 ? audioOpenErr : videoOpenErr);
        return;
    }
    if (audioOpenErr != 0 || videoOpenErr != 0) {
        av_log(nullptr, AV_LOG_WARNING,
               "有一路打不开（audio=%d video=%d），将只播放另一路\n",
               audioOpenErr, videoOpenErr);
    }
    msgQ_.post(FFMsg::Prepared);

    // ---- 分发循环 ----
    AVPacketPtr pkt = make_packet();
    while (!abort_.load()) {
        handleSeekRequest();

        ret = av_read_frame(ic_.get(), pkt.get());
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(ic_->pb)) {
                // ★ EOF 时给两路各投一个"空包"当结束信号（M6 发现、M8 修复）
                if (!eof_.exchange(true)) {
                    if (audioIdx_ >= 0) audioQ_.putNullPacket(audioIdx_);
                    if (videoIdx_ >= 0) videoQ_.putNullPacket(videoIdx_);
                    statNullPkts_.fetch_add((audioIdx_ >= 0 ? 1 : 0)
                                            + (videoIdx_ >= 0 ? 1 : 0));
                }
                // ★★ M10 修正：这里【不再】投递 FFMsg::Completed。
                //
                //   参考工程是在这里 post(Completed) 的，但它的语义是错的：
                //   读线程只管【解复用】，它受队列背压限制、但远快于播放 ——
                //   126KB 的素材几十毫秒就能全部读进队列，而播放要 5 秒。
                //   实测：收到 Completed 时才播到 1.4 秒。
                //
                //   后果（在 M12 会直接暴露成 UI bug）：
                //     MediaPlayer 收到 Completed 就把状态置为"播放完成"、
                //     停掉进度条定时器 —— 于是刚点播放一秒，界面就显示
                //     "已结束"，可声音和画面还在继续放 4 秒。
                //
                //   正确的语义：Completed 应该表示【播放真正结束】，
                //   也就是"文件读完 + 队列里的数据也全被消费光"。
                //   这个检测放在 videoRefresh 里做（那里是唯一持续运行的
                //   时序循环，纯音频文件它也一样在跑）。
            }
            if (ic_->pb && ic_->pb->error)
                break;
            av_usleep(10 * 1000);
            continue;
        }
        eof_ = false;

        if (pkt->stream_index == audioIdx_) {
            audioQ_.put(pkt.get());
            statAudioPkts_.fetch_add(1);      // 沙盒统计
        } else if (pkt->stream_index == videoIdx_) {
            videoQ_.put(pkt.get());
            statVideoPkts_.fetch_add(1);      // 沙盒统计
        } else {
            av_packet_unref(pkt.get());
        }
    }
}

// ---------------------------------------------------------------------------
// audioDecodeThread —— 音频解码线程：packet -> frame（入 sampQ_）
// ---------------------------------------------------------------------------
void FFPlayer::audioDecodeThread()
{
    AVFramePtr frame = make_frame();

    while (!abort_.load()) {
        const int got = audDec_.decodeFrame(frame.get());
        if (got < 0) break;            // -1：被 abort
        if (!got)  continue;           //  0：EOF（解码器已排空）

        // 取一个可写槽位。队列满时这里会阻塞 ——
        // 这是个天然的背压：解码不会跑得比消费快太多。
        Frame* slot = sampQ_.peekWritable();
        if (!slot) break;              // 上游 abort

        // ★ 算 PTS。注意时间基是 1/sample_rate ——
        //   这正是 M6 里 Decoder::decodeFrame 对音频做的换算：
        //   它把 pts 转成了"采样序号"。所以我们再乘 1/sample_rate 就得到秒。
        const AVRational tb{1, frame->sample_rate};
        slot->pts = (frame->pts == AV_NOPTS_VALUE)
                        ? std::nan("")
                        : frame->pts * av_q2d(tb);

        // 帧时长 = 采样数 / 采样率（音频帧的长度是精确已知的，不像视频要看帧率）
        slot->duration = static_cast<double>(frame->nb_samples) / frame->sample_rate;
        slot->format   = frame->format;   // 这里存的是 AVSampleFormat
        // ★ M11：把 packet 的 serial 带到帧上（供播放侧判断"是不是 seek 之前的残留帧"）
        slot->serial   = audDec_.pktSerial();

        // ★ move_ref 而不是拷贝：把解码帧的数据"搬"进队列槽位，
        //   避免一次几 KB 的 memcpy。槽位里的 AVFrame 对象是复用的（M3 讲过）。
        av_frame_move_ref(slot->frame.get(), frame.get());
        sampQ_.push();

        std::lock_guard<std::mutex> lock(statMtx_);
        ++statDecodedFrames_;
    }
}

// ---------------------------------------------------------------------------
// ★ decodeOneAudioFrame —— 本模块最核心的一段：重采样
// ---------------------------------------------------------------------------
// 它要解决的矛盾是：
//   解码器给出的格式是【不确定的】——AAC 通常是 FLTP（32 位浮点、分平面），
//   也可能是 S16P、可能是 5.1 声道、可能是 44100 或 48000Hz；
//   而声卡的格式是【我们定死的】——S16 交织。
// 中间就必须有一个转换器，这就是 libswresample。
//
// 【为什么不用 SDL 的 AudioStream 做转换？】
//   SDL3 的 AudioStream 其实也能做格式转换，但我们没有把源格式告诉它 ——
//   它只被配置成"S16 的队列 + 输出"。这是刻意的分工：
//     · swresample 负责【格式/采样率转换】（专业、可控、质量高）
//     · SDL AudioStream 只负责【缓冲 + 送声卡】
//   好处是转换逻辑集中在一处，将来要加 dither、改重采样质量都只改这里。
// ---------------------------------------------------------------------------
int FFPlayer::decodeOneAudioFrame()
{
    // ① 从帧队列取一帧（阻塞等待）。
    //
    //   ★ 但在【文件已经读完】的情况下不能阻塞 —— 那时解码线程已经退出，
    //     队列永远是空的，peekReadable() 会一直睡下去，把 SDL 的音频回调
    //     彻底卡住（表现为播完之后声卡不再被喂数据，可能出杂音或卡住）。
    //     所以先判断"已到 EOF 且队列已空"，直接告诉调用方"没数据了"，
    //     让 AudioDevice 去补静音 —— 播放就能安静、干净地收尾。
    if (eof_.load() && sampQ_.nbRemaining() == 0)
        return -1;

    // ★★ M11：丢弃"过期音频帧"（seek 之前残留的），循环重试直到拿到
    //   当前播放序列的帧。
    //
    //   为什么需要它？seek 之后音频队列已经 reset（serial +1）并清空，
    //   但解码线程手里可能还攥着几帧旧数据，它们会在 flush 之后才被 push
    //   进 sampQ_ —— 于是队列里混进了 serial 较旧的帧。如果照常播放，
    //   用户会先听到一小段"跳回去"的旧声音。
    //
    //   guard 上限 64 只是防御性的：正常情况最多丢两三帧就能拿到新的。
    Frame* af = nullptr;
    for (int guard = 0; guard < 64; ++guard) {
        af = sampQ_.peekReadable();
        if (!af)
            return -1;                       // 上游 abort
        if (af->serial == audioQ_.serial())
            break;                           // 是当前序列的帧，可以用
        sampQ_.next();                       // 过期帧：丢掉，再取下一帧
        af = nullptr;
    }
    if (!af)
        return -1;

    AVFrame* f = af->frame.get();
    const auto inFmt = static_cast<AVSampleFormat>(f->format);

    // ② 源参数变化时（重新）建立重采样器。
    //
    //    为什么要检查？因为同一个流里参数可能中途改变（切换码流、
    //    广告插播等）。判断四项：采样格式、采样率、声道布局，以及
    //    swr 是否还不存在。
    //
    //    ⚠ 注意这里【没有】比较目标参数，因为目标参数在设备打开时就定死了。
    //      如果流中途从 44100 变成 48000，swr 会把新的 48000 转成
    //      设备要的 44100 —— 这是正确的处理方式（设备规格不能中途改）。
    //
    //    这是本函数的性能关键点：swr_init 不便宜，绝不能逐帧调用。
    //    正常情况下整个播放过程只会重建 1 次（首次）。
    if (!swr_ || inFmt != srcFmt_ || f->sample_rate != srcFreq_ ||
        av_channel_layout_compare(&f->ch_layout, &srcLayout_) != 0) {

        swr_.reset();
        SwrContext* rawSwr = nullptr;

        // ★ FFmpeg 7+ 的新 API：swr_alloc_set_opts2（接收 AVChannelLayout*）
        //   老 API swr_alloc_set_opts() 接收裸的 channel_layout 整数，
        //   在 FFmpeg 7 已被移除 —— 这也是我们把版本闸门定在 7+ 的原因之一。
        //
        //   参数顺序：out_ch_layout, out_sample_fmt, out_sample_rate,
        //             in_ch_layout,  in_sample_fmt,  in_sample_rate
        const int r = swr_alloc_set_opts2(&rawSwr,
                                          &tgtLayout_, AV_SAMPLE_FMT_S16, tgtFreq_,
                                          &f->ch_layout, inFmt, f->sample_rate,
                                          0, nullptr);
        if (r < 0 || !rawSwr || swr_init(rawSwr) < 0) {
            av_log(nullptr, AV_LOG_ERROR, "swr init failed: %s\n",
                   av_err_string(r).c_str());
            if (rawSwr) swr_free(&rawSwr);
            // ★ 失败也要 next()：否则永远卡在这一帧上，死循环。
            //   丢弃一帧音频的听感损失远小于卡死。
            sampQ_.next();
            return -1;
        }
        swr_.reset(rawSwr);
        av_channel_layout_uninit(&srcLayout_);
        av_channel_layout_copy(&srcLayout_, &f->ch_layout);
        srcFreq_ = f->sample_rate;
        srcFmt_  = inFmt;

        std::lock_guard<std::mutex> lock(statMtx_);
        ++statSwrRebuilds_;
    }

    // ③ 算输出缓冲要多大。
    //
    //    outCount = 源样本数 × (目标采样率 / 源采样率)，向上取整。
    //    av_rescale_rnd 做的就是有理数换算（内部 64 位整数，避免浮点误差）。
    //
    //    +256 是【余量】。为什么需要？因为重采样滤波器有"分数延迟"，
    //    输出样本数可能比理论值多几个。留点余量就不用每次精确计算，
    //    多出来的部分按实际返回值 converted 处理即可。
    const int outCount = static_cast<int>(av_rescale_rnd(
        f->nb_samples, tgtFreq_, f->sample_rate, AV_ROUND_UP)) + 256;

    // av_samples_get_buffer_size 算"这么多样本、这种格式、紧凑排列"要多少字节。
    // 最后一个参数 1 = 紧凑（interleaved）—— 所有声道的数据交替排列，
    // 正是声卡要的布局。
    const int outBytes = av_samples_get_buffer_size(
        nullptr, tgtLayout_.nb_channels, outCount, AV_SAMPLE_FMT_S16, 1);
    if (outBytes <= 0) { sampQ_.next(); return -1; }

    // ★ 从【自有缓冲】里取空间。resize 在容量够时不会重新分配，
    //   所以逐帧调用不会反复 malloc —— 这是"缓冲复用"。
    //   关键点：这块内存属于我们自己，不属于帧队列。
    resampleBuf_.resize(outBytes);
    uint8_t* outPlanes[1] = { resampleBuf_.data() };

    // ④ 转换。
    //    swr_convert 的参数：
    //      (swr, 输出平面数组, 输出样本数上限, 输入平面数组, 输入样本数)
    //
    //    f->extended_data 是源数据的平面指针数组：
    //      · FLTP（分平面）时，[0]=左声道、[1]=右声道、……各指向独立缓冲；
    //      · S16（交织）时，[0] 指向全部交织数据。
    //    swr_convert 会按我们设置的 in/out 格式自己处理这两种情况。
    //
    //    类型转换说明：extended_data 是 uint8_t**，而 swr_convert 要
    //    const uint8_t** —— 只是 const 修饰的差异，用 const_cast 桥一下。
    const int converted = swr_convert(swr_.get(), outPlanes, outCount,
                                      const_cast<const uint8_t**>(f->extended_data),
                                      f->nb_samples);
    if (converted < 0) { sampQ_.next(); return -1; }

    // 实际产出的字节数 = 输出样本数 × 声道数 × 每样本字节数(S16 = 2)
    const int dataBytes = converted * tgtLayout_.nb_channels
                          * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);

    audioBuf_     = resampleBuf_.data();
    audioBufSize_ = dataBytes;
    // ★★ M9 修正：这里要存帧的【结束 PTS】，不是起始 PTS。
    //
    //   参考工程写的是 `audioClock_ = af->pts;`，配合后面的
    //   `audClk_.set(audioClock_ - remain)` 推出来的播放头会整整
    //   早一个音频帧（1024 采样 @44.1kHz ≈ 23ms）。代入两个极端看：
    //       刚解出一帧（remain = 整帧）: 播放头 = PTS − 帧时长   ← 早了一帧
    //       帧取完时    （remain = 0） : 播放头 = PTS            ← 应该是 PTS + 帧时长
    //
    //   ffplay 的约定是 audio_clock = 帧 PTS + 帧时长（也就是"这一帧播完时
    //   应该到哪儿"），再减去缓冲里还没播的时长：
    //       刚解出一帧: (PTS + 帧时长) − 帧时长 = PTS            ✓
    //       帧取完时  : (PTS + 帧时长) − 0     = PTS + 帧时长    ✓
    //   这样才严丝合缝。帧时长在 audioDecodeThread 里已经算好存在 slot->duration 里。
    audioClock_   = af->pts + af->duration;

    // ★★★ 数据已经拷进自有缓冲了，现在才可以释放帧队列槽位。
    //
    //   这个顺序是【本模块最容易被写错的地方】。
    //   老工程写的是"先 frame_queue_next 再引用帧里的 data"——
    //   而 next() 会 av_frame_unref 把像素/采样数据还回去，
    //   于是后面读到的是已释放内存（use-after-free）。
    //   表现为偶发爆音、或者随机崩溃，极难复现。
    //
    //   正确顺序：先 swr_convert 到自己的缓冲 → 再 next()。
    sampQ_.next();
    return dataBytes;
}

// ---------------------------------------------------------------------------
// pullAudio —— 供 SDL 音频回调拉取 PCM（★ 运行在 SDL 音频线程里）
// ---------------------------------------------------------------------------
int FFPlayer::pullAudio(uint8_t* dst, int bytes)
{
    int copied = 0;

    // SDL 一次可能要很多字节（比如 16KB），而一帧重采样后可能只有 4KB，
    // 所以要循环若干次，跨越多帧来填满。
    while (copied < bytes) {
        // 当前帧的数据取完了 → 解下一帧
        if (audioBufIndex_ >= audioBufSize_) {
            if (decodeOneAudioFrame() < 0)
                break;          // ★ 没数据了：跳出填充循环，但【不 return】
            audioBufIndex_ = 0;
        }

        const int avail = audioBufSize_ - audioBufIndex_;
        const int chunk = std::min(avail, bytes - copied);
        if (audioBuf_)
            std::memcpy(dst + copied, audioBuf_ + audioBufIndex_, chunk);
        else
            std::memset(dst + copied, 0, chunk);   // 理论上不会走到
        audioBufIndex_ += chunk;
        copied += chunk;
    }

    // ★★★ 推进音频主时钟 —— 全项目同步逻辑的源头 ★★★
    //
    //   问题：声卡不会告诉我们"此刻正好播到第几微秒"。SDL 的回调只知道
    //        "我现在还需要多少字节"，这是"未来需求"，不是"当前进度"。
    //
    //   解法：用"当前帧的 PTS 减去尚未被取走的时长"来推算播放头位置。
    //
    //        audioClock_             = 这一帧的起始 PTS
    //        audioBufSize_ - Index_  = 这一帧还剩多少字节没交给声卡
    //        除以 bytesPerSecond      = 还剩多少"秒"的音频没播
    //
    //        播放头 ≈ 帧 PTS − 剩余时长
    //
    //   直觉验证：
    //     · 刚取到一帧时（剩余 = 整帧），播放头 = PTS             ✓
    //     · 帧快播完时（剩余 → 0），播放头 → PTS + 帧时长         ✓
    //   所以这个估计是【平滑且单调】的，误差被限制在一次回调的粒度内
    //  （几毫秒），而且每次回调都重算，不会累积漂移。
    //
    //   ★★ M9 修正：这段【必须无条件执行】，不能因为"没取到新数据"就跳过。
    //      参考工程在没数据时直接 return 了，于是时钟失去了锚定，
    //      只能靠 get() 的系统时间外推一路跑下去 —— 实测播到 5 秒的素材，
    //      位置读数涨到了 7.7 秒还在涨。
    //      现在的行为：没数据时 remain = 0，等于把时钟【按住】在最后一帧的
    //      PTS 上。这也正是 ffplay 的做法：它的音频回调即使遇到 underrun
    //      也会用同一个 audio_clock 反复重锚定，效果就是"把时钟按住"。
    if (!std::isnan(audioClock_) && audioDev_.bytesPerSecond() > 0) {
        const double remain = static_cast<double>(audioBufSize_ - audioBufIndex_)
                              / audioDev_.bytesPerSecond();
        audClk_.set(audioClock_ - remain);
    }

    if (copied == 0)
        return -1;              // 让 AudioDevice 去补静音（M7 的兜底在这里兑现）

    // 顺便统计一下峰值，用来验证"确实有非静音的声音数据经过了这里"。
    // （纯沙盒用途，最终工程里没有这几行）
    {
        const auto* s = reinterpret_cast<const int16_t*>(dst);
        const int n  = copied / 2;
        int peak = statPcmPeak_;
        for (int i = 0; i < n; ++i) {
            const int v = s[i] < 0 ? -s[i] : s[i];
            if (v > peak) peak = v;
        }
        std::lock_guard<std::mutex> lock(statMtx_);
        statPcmBytes_ += copied;
        statPcmPeak_   = peak;
    }

    return copied;
}

// ---------------------------------------------------------------------------
// videoDecodeThread —— 视频解码线程：packet -> frame（入 pictQ_）
// ---------------------------------------------------------------------------
// 结构与 audioDecodeThread 几乎一样，只有两处差别值得说：
//   ① 帧时长要靠帧率算（音频可以直接用 nb_samples/sample_rate）
//   ② PTS 用容器时间基换算（音频在 M6 里已经被换算成"采样序号"了）
void FFPlayer::videoDecodeThread()
{
    AVFramePtr frame = make_frame();

    while (!abort_.load()) {
        const int got = vidDec_.decodeFrame(frame.get());
        if (got < 0) break;            // -1：被 abort
        if (!got)  continue;           //  0：EOF

        // 取可写槽位。pictQ_ 只有 3 个槽（M3 讲过为什么），满了会阻塞。
        // ★ 这层背压对同步至关重要：它保证解码不会跑得远远超过显示。
        //   否则队列里堆着几十帧，seek 时要清理的数据量、内存占用、
        //   以及"图像滞后于声音"的时长都会失控。
        Frame* slot = pictQ_.peekWritable();
        if (!slot) break;              // 上游 abort

        // 帧时长 = 1 / 帧率。frameRate_ 来自 av_guess_frame_rate，
        // 某些流里可能是 0/0（没写帧率信息），所以必须做保护，
        // 否则就是除零 —— 会得到 inf 或 nan 污染后面的比较。
        const double duration = (frameRate_.num && frameRate_.den)
                                    ? av_q2d(AVRational{frameRate_.den, frameRate_.num})
                                    : 0.0;

        // 视频 PTS：容器时间基 -> 秒
        const double pts = (frame->pts == AV_NOPTS_VALUE)
                               ? std::nan("") : frame->pts * av_q2d(videoTb_);

        slot->pts      = pts;
        slot->duration = duration;
        slot->width    = frame->width;
        slot->height   = frame->height;
        slot->format   = frame->format;
        // ★ M11：把 packet 的 serial 带到帧上
        slot->serial   = vidDec_.pktSerial();
        av_frame_move_ref(slot->frame.get(), frame.get());
        pictQ_.push();
    }
}

// ---------------------------------------------------------------------------
// videoRefreshThread —— 视频刷新线程
// ---------------------------------------------------------------------------
// 【为什么视频需要一条专门的线程来做"到点显示"？】
//   音频不需要 —— 声卡会主动来要数据，硬件在驱动我们。
//   但显示器不会"来要画面"，所以必须由软件主动决定：
//   "现在这一瞬间，该显示哪一帧？"
//   这条线程就是在反复问这个问题。
void FFPlayer::videoRefreshThread()
{
    while (!abort_.load()) {
        // 每轮的睡眠时长交给 videoRefresh 决定，默认 10ms。
        double remaining = kRefreshInterval;

        videoRefresh(remaining);

        if (remaining > 0.0)
            av_usleep(static_cast<unsigned>(remaining * 1'000'000.0));
    }
}

// ---------------------------------------------------------------------------
// ★★★ videoRefresh —— 整个项目的同步核心 ★★★
// ---------------------------------------------------------------------------
// 它只回答一个问题：**下一帧到了该显示的时间吗？**
//
// 【三种可选方案，为什么选第二种】
//   方案 A：让视频按自己的帧率播（定时器每 1/fps 秒显示一帧）。
//          简单，但视频和音频各自为政 —— 声卡时钟与系统时钟有微小差异，
//          长时间播放必然越差越多，表现为"越看越不对口型"。
//   方案 B：音频驱动视频（★ 本项目采用）。视频只问"到点了吗"，
//          基准是音频主时钟。因为人耳对声音断续敏感、对画面抖动宽容，
//          所以让画面去适配声音是代价最小的方向。
//   方案 C：外部时钟为主（纯系统时间）。适合纯音频或需要绝对时间同步的场景。
//   ★ 本项目的组合是：有音频用音频主时钟；没音频就退化成
//     "用视频 PTS 锚定的外部时钟"，两条路都通向同一个 audClk_。
//
// 【remainingTime 这个出参的妙处】
//   10ms 不是"固定睡眠"，而是"睡眠上限"。如果下一帧还有 3ms 就该显示了，
//   那就只睡 3ms —— 显示时机精度因此远好于 10ms。
//   而如果还有 500ms 才到下一帧（低帧率、或 seek 之后），也只睡 10ms，
//   因为必须定期回来看时钟和状态有没有变。
//   **这个"按需缩短睡眠"的小机制，是这一整个模块里最精巧的一处设计。**
void FFPlayer::videoRefresh(double& remainingTime)
{
    // ---- ★ 播放结束检测（M10 新增）----
    //
    // "播放结束" = 文件已读完（eof_）+ 所有缓冲都空了。
    //   · 音频那两段：audioQ_（待解码的包）+ sampQ_（已解码待播放的帧）
    //   · 视频那两段：videoQ_ + pictQ_
    //
    // 放在这里而不是 readThread，是因为只有这里在【持续按时间运行】，
    // 能观察到"数据被慢慢消费光"这个过程。放在 readThread 的话，
    // 它早就跑完退到循环里空转了，根本看不到后面的消费过程。
    //
    // 用 completedPosted_.exchange(true) 保证只投一次；
    // seek 会把 eof_ 和 completedPosted_ 都复位，所以能重新发。
    if (eof_.load() && !paused_.load()
        && audioQ_.nbPackets() == 0 && videoQ_.nbPackets() == 0
        && sampQ_.nbRemaining() == 0 && pictQ_.nbRemaining() == 0) {
        if (!completedPosted_.exchange(true))
            msgQ_.post(FFMsg::Completed);
        return;
    }

    // 没有视频流，或者正在暂停 —— 什么都不做（暂停时时钟也被冻住了）
    if (!videoSt_ || paused_.load())
        return;
    if (pictQ_.nbRemaining() == 0)
        return;

    Frame* vp = pictQ_.peek();

    // ★★ M11：serial 检查 —— 丢弃 seek 之前残留的视频帧。
    //
    //   这是 M3 埋下的那个缺口的最后一块拼图。在此之前，工程只能靠
    //   "PTS 比时钟前跳超过 1 秒"这种启发式去猜哪一帧是残留的 ——
    //   阈值既可能误判（真的 PTS 跳变被当成残留）也可能漏判
    //   （残留帧的 PTS 恰好没跳那么多）。
    //
    //   现在有了精确判据：帧的 serial 与队列当前 serial 不一致 = 它是
    //   上一次 seek 之前的产物，直接丢，连显示都不显示。
    //   ffplay 用的就是这个判断（`if (vp->serial != is->videoq.serial)`）。
    if (vp->serial != videoQ_.serial()) {
        pictQ_.next();
        return;
    }

    // ---- 特殊情况：帧没有时间戳 ----
    // 无法参与同步，只能直接显示。真实流里少见，但存在（比如某些裸流）。
    if (std::isnan(vp->pts)) {
        if (videoCb_) videoCb_(*vp);
        pictQ_.next();
        return;
    }

    // ---- 取主时钟 ----
    double master = audClk_.get();

    // ---- 纯视频（没有音频流）的降级路径 ----
    if (audioIdx_ < 0) {
        // 没有音频来推进时钟，那就反过来：用视频自己的 PTS 去锚定它，
        // 之后 get() 靠系统时间线性外推，就得到一个"外部时钟"。
        //
        // 什么时候需要重新锚定？三个条件：
        //   ① !videoClockInit_      ：还没锚定过（播放刚开始）
        //   ② isnan(master)         ：时钟是空的（M5 修好 NaN 语义后这条才有意义）
        //   ③ vp->pts - master > 1.0：PTS 比时钟【前跳超过 1 秒】
        //
        // ★ 第 ③ 条的角色在 M11 变了，值得说清楚：
        //
        //   它原本是参考工程用来【兜 serial 缺口】的启发式 ——
        //   seek 之后队列里可能混进 PTS 很旧的残留帧，把时钟错误地锚回旧位置；
        //   等真正的新帧到达时，它比时钟大很多，靠这条规则重新锚定、纠正回来。
        //
        //   M11 在 videoRefresh 开头加了精确的 serial 检查（残留帧直接被丢弃，
	//   根本走不到这里），所以"糊住缺口"这个职责已经不需要它了。
        //
        //   现在它保留下来只做一件事：应对【真实的 PTS 跳变】——
        //   比如某些流中间有断点、或时间戳不连续。这时时钟确实会远远落后，
        //   靠这条规则重新锚定比"慢慢追"更合理。它从"补丁"变成了"安全网"。
        if (!videoClockInit_ || std::isnan(master) || vp->pts - master > 1.0) {
            audClk_.set(vp->pts);
            videoClockInit_ = true;
            master = vp->pts;
        }
    }

    // ---- 核心比较：这一帧相对于主时钟是早了还是晚了 ----
    const double diff = vp->pts - master;

    if (diff > 0.0) {
        // 还没到显示时间。把睡眠缩短到"刚好够等到这一帧"，
        // 但不会超过本轮的默认上限（10ms）—— 因为还要定期回来检查状态。
        remainingTime = std::min(remainingTime, diff);
        return;
    }

    // ---- 已经到时间了（diff <= 0）----
    //
    // ★ M10 改进（参考工程没有这一段）：如果这一帧已经迟到太多，
    //   说明解码/渲染跟不上播放速度了。此时【丢掉它】比【补显它】更好：
    //     · 硬把它显示出来，只会让后面的帧也一起往后拖，越拖越远；
    //     · 观众看到的是"慢放的画面 + 正常的声音"，比偶尔跳一下更难受。
    //   阈值取 100ms（ffplay 的 AV_SYNC_THRESHOLD_MAX 也是这个量级）。
    //
    //   `pictQ_.nbRemaining() > 1` 是一个必要的护栏：只有当后面还有帧
    //   可以显示时才允许丢。否则极端情况下会把所有帧都丢光，变成纯黑屏。
    if (diff < -kLateDropThreshold && pictQ_.nbRemaining() > 1) {
        statDroppedFrames_.fetch_add(1);
        pictQ_.next();
        return;
    }

    // ---- 到点了，交给上层显示 ----
    // ⚠ 回调必须在返回前把数据拷走：下面这一行 next() 会立刻释放这个槽位。
    if (videoCb_) videoCb_(*vp);
    pictQ_.next();
}
