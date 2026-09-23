// 板端视频源实现（拉流 → 硬解 → appsink → QImage）
//
// 管道：tcpclientsrc host=127.0.0.1 port=P ! tsdemux ! h264parse ! mppvideodec
//       ! videoscale ! videoconvert ! video/x-raw,format=RGB,width=W,height=H
//       ! appsink(name=sink, max-buffers=2, drop=true, sync=false)
//
// 几个刻意的选择：
//   - videoscale/videoconvert 都在**板端**做：缩到小尺寸后再转 RGB，比全分辨率转换省很多 CPU
//   - appsink 用 try_pull_sample 而不是回调：拉帧线程自己控节奏，代码更好读
//   - 不用 gst_video_info_from_caps（那要额外链 libgstvideo）：直接用 caps 的 width/height +
//     buffer size/height 算 stride —— appsink 出来的 RGB 一般是无 padding 的紧密排列
//   - **自动重连**：建管道放在拉帧线程里，连不上/流断(EOS)就拆掉重建，每秒重试。
//     因为 tcpclientsrc 是主动连接，对端没推流时管道会直接进 error —— 这不该让整个 UI 挂掉
#include "gstsource.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <cstdio>
#include <unistd.h>      // usleep：分片睡眠，既控制重试间隔又不阻塞退出

using std::lock_guard;

GstSource::~GstSource() { stop(); }

// 只启动拉流线程，**不要求对端此刻已在推流**（未连上会自动重试）
bool GstSource::start(uint16_t port, int out_w, int out_h) {
    if (running_.load()) return true;
    if (out_w <= 0 || out_h <= 0) { out_w = 640; out_h = 360; }
    if (!gst_is_initialized()) gst_init(nullptr, nullptr);   // 幂等
    port_ = port;
    out_w_ = out_w;
    out_h_ = out_h;
    running_ = true;
    th_ = thread(&GstSource::pullLoop, this);
    printf("[gstsrc] 拉流线程已启动：等待 tcp://127.0.0.1:%u（未连上会自动重试）\n",
           unsigned(port_));
    return true;
}

// 建管道并置 PLAYING。**所有失败都只是“这一轮没成功”**（多半是对端还没推流），
// 不能让调用方当成致命错误 —— 由 pullLoop 隔一秒重试。
bool GstSource::openPipeline() {
    char desc[768];
    snprintf(desc, sizeof(desc),
             "tcpclientsrc host=127.0.0.1 port=%u "
             "! tsdemux ! h264parse ! mppvideodec "
             "! videoscale ! videoconvert ! video/x-raw,format=RGB,width=%d,height=%d "
             "! appsink name=sink max-buffers=2 drop=true sync=false",
             unsigned(port_), out_w_, out_h_);

    GError* err = nullptr;
    GstElement* pipe = gst_parse_launch(desc, &err);
    if (!pipe || err) {
        err_ = err ? err->message : "建管道失败";
        if (err) g_error_free(err);
        if (pipe) gst_object_unref(pipe);
        return false;
    }
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipe), "sink");   // 带 +1 引用
    if (!sink) {
        err_ = "管道里没有 appsink";
        gst_object_unref(pipe);
        return false;
    }
    GstBus* bus = gst_element_get_bus(pipe);

    if (gst_element_set_state(pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        err_ = "PLAYING 状态切换失败（推流端未就绪）";
        gst_object_unref(bus);
        gst_object_unref(sink);
        gst_object_unref(pipe);
        return false;
    }
    pipeline_ = pipe;
    sink_ = sink;
    bus_ = bus;
    return true;
}

void GstSource::closePipeline() {
    if (!pipeline_) return;
    GstElement* pipe = (GstElement*)pipeline_;
    gst_element_set_state(pipe, GST_STATE_NULL);
    if (bus_) { gst_object_unref(GST_OBJECT(bus_)); bus_ = nullptr; }
    if (sink_) { gst_object_unref(GST_OBJECT(sink_)); sink_ = nullptr; }
    gst_object_unref(pipe);
    pipeline_ = nullptr;
}

void GstSource::pullLoop() {
    while (running_.load()) {
        // ---- ① 确保管道存在（首次、或上轮出错重建）----
        if (!pipeline_) {
            if (!openPipeline()) {
                if (!warned_) {      // 只提示一次，避免每秒刷屏
                    printf("[gstsrc] 还没连上推流端(127.0.0.1:%u)：%s；每秒重试…\n",
                           unsigned(port_), err_.c_str());
                    warned_ = true;
                }
                for (int i = 0; i < 10 && running_.load(); ++i) usleep(100000);   // 1s（分片以便退出）
                continue;
            }
            warned_ = false;
            printf("[gstsrc] 已连上，开始解码：%dx%d RGB\n", out_w_, out_h_);
        }

        // ---- ② 拉一帧（200ms 超时：有帧立刻取，退出也能快速响应）----
        GstSample* s = gst_app_sink_try_pull_sample((GstAppSink*)sink_, 200 * GST_MSECOND);
        if (s) {
            GstCaps* caps = gst_sample_get_caps(s);
            GstBuffer* buf = gst_sample_get_buffer(s);
            if (caps && buf && gst_caps_get_size(caps) > 0) {
                const GstStructure* st = gst_caps_get_structure(caps, 0);
                int w = 0, h = 0;
                gst_structure_get_int(st, "width", &w);
                gst_structure_get_int(st, "height", &h);
                gsize bytes = gst_buffer_get_size(buf);
                int stride = (h > 0) ? int(bytes / gsize(h)) : 0;
                if (w > 0 && h > 0 && stride >= w * 3) {
                    GstMapInfo map;
                    if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
                        // 先“包一层”零拷贝视图，再 copy() 深拷贝 —— map 一解除，指针就失效了
                        QImage view((const uchar*)map.data, w, h, stride, QImage::Format_RGB888);
                        QImage copy = view.copy();
                        {
                            lock_guard<mutex> lk(m_);
                            if (has_new_) dropped_++;   // UI 还没取走上一次 → 这一帧把它顶掉
                            latest_ = copy;
                            has_new_ = true;
                        }
                        received_++;
                        gst_buffer_unmap(buf, &map);
                    }
                }
            }
            gst_sample_unref(s);
        }

        // ---- ③ 查 bus：连接失败 / 流中断(EOS) 都要拆掉重建 ----
        GstMessage* msg = gst_bus_pop_filtered(
            (GstBus*)bus_, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (msg) {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                GError* gerr = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(msg, &gerr, &dbg);
                err_ = gerr ? gerr->message : "未知错误";
                printf("[gstsrc] 拉流出错: %s\n", err_.c_str());
                if (gerr) g_error_free(gerr);
                g_free(dbg);
            } else {
                printf("[gstsrc] 流已结束（推流端退出），准备重连\n");
            }
            gst_message_unref(msg);
            closePipeline();
            for (int i = 0; i < 5 && running_.load(); ++i) usleep(100000);   // 0.5s 后重试
        }
    }
}

bool GstSource::takeFrame(QImage& out) {
    lock_guard<mutex> lk(m_);
    if (!has_new_) return false;
    out = latest_;
    has_new_ = false;
    return true;
}

void GstSource::stop() {
    if (!running_.load() && !pipeline_) return;
    running_ = false;
    if (th_.joinable()) th_.join();          // 拉帧线程最多阻塞 200ms + 重试分片
    closePipeline();
    printf("[gstsrc] 已停止：收到 %llu 帧，UI 丢弃 %llu 帧\n",
           (unsigned long long)received_.load(), (unsigned long long)dropped_.load());
}

string GstSource::lastError() const { return err_; }
