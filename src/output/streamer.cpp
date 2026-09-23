// H.264 推流实现：GStreamer appsrc 喂 NV12 → mpph264enc(RK 硬件编码) → MPEG-TS → TCP
//
// 链路：
//   appsrc ! video/x-raw,format=NV12,width,height,framerate
//          ! mpph264enc header-mode=1 gop=.. bps=..
//          ! h264parse config-interval=1 ! mpegtsmux ! tcpserversink sync=false
//
// 参数为什么这么写（全是板子上踩出来的，详见 docs/devlog/2026-09-22.md）：
//   header-mode=1                默认 0=first-frame：SPS/PPS 只在第一帧送出，中途接入的播放器
//                                拿不到参数集 → 报 "non-existing PPS 0 referenced" 黑屏。必须设 1。
//   bps                          这个编码器**没有** bitrate 属性，码率单位是 bit/s
//   h264parse config-interval=1  周期性重发 SPS/PPS，配合 header-mode 让播放器"随时可接入"
//   sync=false                   sink 不做时钟同步 → 降延迟
//   (无 bframes 属性)            RK 编码器不支持 B 帧 —— 对实时推流反而是好事，省一个 GOP 延迟
#include "output/streamer.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// 标准库名字逐个引入
using std::atomic;

#ifdef ENABLE_STREAM

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

namespace {
// 一帧的时长(ns)，由帧率换算
GstClockTime frame_duration_ns(int fps) {
    return GstClockTime(1000000000ULL / (fps > 0 ? fps : 30));
}
}  // namespace

Streamer::~Streamer() { stop(); }

bool Streamer::start(uint32_t w, uint32_t h, int fps, int port,
                     int bitrate_kbps, int gop) {
    if (running_.load()) return true;
    if (w == 0 || h == 0 || (w % 2) || (h % 2)) {
        printf("[stream] 尺寸非法 %ux%u（NV12 要求宽高都为偶数）\n", w, h);
        return false;
    }
    if (fps <= 0) fps = 30;
    if (!gst_is_initialized()) gst_init(nullptr, nullptr);   // 幂等，只真正初始化一次

    // 可选编码参数拼进描述串（不填就走编码器默认值）
    char enc_opts[96];
    enc_opts[0] = '\0';
    size_t n = 0;
    if (bitrate_kbps > 0)
        n += size_t(snprintf(enc_opts + n, sizeof(enc_opts) - n, " bps=%d", bitrate_kbps * 1000));
    if (gop > 0 && n < sizeof(enc_opts))
        snprintf(enc_opts + n, sizeof(enc_opts) - n, " gop=%d", gop);

    char desc[640];
    snprintf(desc, sizeof(desc),
             "appsrc name=src is-live=true block=false format=time do-timestamp=false "
             "! video/x-raw,format=NV12,width=%u,height=%u,framerate=%d/1 "
             "! mpph264enc header-mode=1%s "
             "! h264parse config-interval=1 "
             "! mpegtsmux "
             "! tcpserversink host=0.0.0.0 port=%d sync=false",
             w, h, fps, enc_opts, port);

    GError* err = nullptr;
    GstElement* pipe = gst_parse_launch(desc, &err);
    if (!pipe || err) {
        printf("[stream] 建管道失败: %s\n",
               err ? err->message : "(未知) 请确认板上装了 rockchipmpp 插件(mpph264enc)");
        if (err) g_error_free(err);
        if (pipe) gst_object_unref(pipe);
        return false;
    }

    GstElement* src = gst_bin_get_by_name(GST_BIN(pipe), "src");   // 带 +1 引用返回
    if (!src) {
        printf("[stream] 管道里没找到 appsrc\n");
        gst_object_unref(pipe);
        return false;
    }

    // caps 必须显式下发：appsrc 得先知道每帧的格式，下游编码器才能协商成功
    GstCaps* caps = gst_caps_new_simple("video/x-raw",
                                        "format", G_TYPE_STRING, "NV12",
                                        "width", G_TYPE_INT, int(w),
                                        "height", G_TYPE_INT, int(h),
                                        "framerate", GST_TYPE_FRACTION, fps, 1,
                                        nullptr);
    gst_app_src_set_caps(GST_APP_SRC(src), caps);
    gst_caps_unref(caps);
    gst_app_src_set_stream_type(GST_APP_SRC(src), GST_APP_STREAM_TYPE_STREAM);
    // 队列上限约 3 帧：满了 push_buff/frame 直接失败 → 我们丢帧，绝不阻塞采集线程(实时性优先)
    gst_app_src_set_max_bytes(GST_APP_SRC(src), guint64(w) * h * 3 / 2 * 3);

    if (gst_element_set_state(pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        printf("[stream] PLAYING 状态切换失败\n");
        gst_object_unref(src);
        gst_object_unref(pipe);
        return false;
    }

    pipeline_ = pipe;
    appsrc_ = src;        // 留着引用，stop() 里归还
    fps_ = fps;
    base_pts_us_ = 0;
    pushed_ = 0;
    dropped_ = 0;
    running_ = true;
    printf("[stream] H.264 推流已启动 tcp://0.0.0.0:%d (%ux%d@%d, %dkbps, gop=%d)\n",
           port, w, h, fps, bitrate_kbps, gop);
    return true;
}

void Streamer::stop() {
    if (!pipeline_) return;
    GstElement* pipe = (GstElement*)pipeline_;
    // 先发 EOS：让已连上的播放器正常收尾（否则客户端会一直干等到超时）
    gst_element_send_event(pipe, gst_event_new_eos());
    gst_element_set_state(pipe, GST_STATE_NULL);
    if (appsrc_) { gst_object_unref(GST_OBJECT(appsrc_)); appsrc_ = nullptr; }
    gst_object_unref(pipe);
    pipeline_ = nullptr;
    running_ = false;
    printf("[stream] 推流已停止: 成功 %llu 帧, 丢 %llu 帧\n",
           (unsigned long long)pushed_.load(), (unsigned long long)dropped_.load());
}

void Streamer::push(const uint8_t* nv12, size_t bytes, uint64_t pts_us) {
    if (!running_.load() || !appsrc_ || !nv12 || bytes == 0) return;
    if (base_pts_us_ == 0) base_pts_us_ = pts_us;        // 首帧当时间轴原点
    uint64_t rel_us = pts_us > base_pts_us_ ? pts_us - base_pts_us_ : 0;

    GstBuffer* buf = gst_buffer_new_allocate(nullptr, bytes, nullptr);
    if (!buf) { dropped_++; return; }
    GstMapInfo map;
    if (gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
        memcpy(map.data, nv12, bytes);
        gst_buffer_unmap(buf, &map);
    }
    GST_BUFFER_PTS(buf) = rel_us * 1000ULL;              // us → ns
    GST_BUFFER_DTS(buf) = GST_BUFFER_PTS(buf);
    GST_BUFFER_DURATION(buf) = frame_duration_ns(fps_);

    // push_buffer 接管 buffer 所有权（成功、失败都会消费掉）→ 之后不能再 unref
    if (gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf) != GST_FLOW_OK)
        dropped_++;                                      // 队列满/管道关闭 → 丢帧保实时
    else
        pushed_++;
}

#else   // ---------------- 未启用推流：空实现，调用方无需 #ifdef ----------------

Streamer::~Streamer() {}

bool Streamer::start(uint32_t, uint32_t, int, int, int, int) {
    printf("[stream] 本二进制未编译推流功能；交叉编译时加 "
           "-DENABLE_STREAM=ON -DGSTREAMER_ROOT=<SDK staging/usr>\n");
    return false;
}

void Streamer::stop() {}

void Streamer::push(const uint8_t*, size_t, uint64_t) {}

#endif
