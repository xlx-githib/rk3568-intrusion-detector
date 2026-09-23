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
#include "gstsource.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <cstdio>

using std::lock_guard;

GstSource::~GstSource() { stop(); }

bool GstSource::start(uint16_t port, int out_w, int out_h) {
    if (running_.load()) return true;
    if (out_w <= 0 || out_h <= 0) { out_w = 640; out_h = 360; }
    if (!gst_is_initialized()) gst_init(nullptr, nullptr);   // 幂等

    char desc[768];
    snprintf(desc, sizeof(desc),
             "tcpclientsrc host=127.0.0.1 port=%u "
             "! tsdemux ! h264parse ! mppvideodec "
             "! videoscale ! videoconvert ! video/x-raw,format=RGB,width=%d,height=%d "
             "! appsink name=sink max-buffers=2 drop=true sync=false",
             unsigned(port), out_w, out_h);

    GError* err = nullptr;
    GstElement* pipe = gst_parse_launch(desc, &err);
    if (!pipe || err) {
        err_ = err ? err->message : "建管道失败";
        printf("[gstsrc] %s\n", err_.c_str());
        if (err) g_error_free(err);
        if (pipe) gst_object_unref(pipe);
        return false;
    }

    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipe), "sink");   // 带 +1 引用
    if (!sink) {
        err_ = "管道里没有 appsink";
        printf("[gstsrc] %s\n", err_.c_str());
        gst_object_unref(pipe);
        return false;
    }

    pipeline_ = pipe;
    sink_ = sink;
    running_ = true;
    th_ = thread(&GstSource::pullLoop, this);

    // 注意：PLAYING 只是"准备好"，真正的数据要等主程序那边有客户端连上才开始流
    if (gst_element_set_state(pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        err_ = "PLAYING 状态切换失败";
        printf("[gstsrc] %s\n", err_.c_str());
        running_ = false;
        if (th_.joinable()) th_.join();
        gst_object_unref(GST_OBJECT(sink_)); sink_ = nullptr;
        gst_object_unref(pipe); pipeline_ = nullptr;
        return false;
    }

    printf("[gstsrc] 已启动：tcp://127.0.0.1:%u → 解码后 %dx%d RGB\n",
           unsigned(port), out_w, out_h);
    return true;
}

void GstSource::pullLoop() {
    GstAppSink* sink = (GstAppSink*)sink_;
    while (running_.load()) {
        // 200ms 超时：既有帧就立刻取，stop() 时也能在 200ms 内退出
        GstSample* s = gst_app_sink_try_pull_sample(sink, 200 * GST_MSECOND);
        if (!s) continue;                                   // 超时/无数据 → 继续等

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
                    // 先"包一层"零拷贝视图，再 copy() 深拷贝 —— map 一解除，指针就失效了
                    QImage view((const uchar*)map.data, w, h, stride, QImage::Format_RGB888);
                    QImage copy = view.copy();
                    {
                        lock_guard<mutex> lk(m_);
                        if (has_new_) dropped_++;   // UI 还没取走上一次 → 这一帧把它顶掉了
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
}

bool GstSource::takeFrame(QImage& out) {
    lock_guard<mutex> lk(m_);
    if (!has_new_) return false;
    out = latest_;
    has_new_ = false;
    return true;
}

void GstSource::stop() {
    if (!pipeline_) return;
    running_ = false;
    if (th_.joinable()) th_.join();          // 拉帧线程最多阻塞 200ms
    GstElement* pipe = (GstElement*)pipeline_;
    gst_element_set_state(pipe, GST_STATE_NULL);
    if (sink_) { gst_object_unref(GST_OBJECT(sink_)); sink_ = nullptr; }
    gst_object_unref(pipe);
    pipeline_ = nullptr;
    printf("[gstsrc] 已停止：收到 %llu 帧，UI 丢弃 %llu 帧\n",
           (unsigned long long)received_.load(), (unsigned long long)dropped_.load());
}

string GstSource::lastError() const { return err_; }
