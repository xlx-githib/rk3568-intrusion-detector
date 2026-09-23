#pragma once
// 板端视频源：GStreamer 拉流(MPEG-TS over TCP) → mppvideodec 硬解 → appsink → QImage
//
// 设计要点：
//   1) start() 后内部起一个"拉帧线程"，循环 gst_app_sink_try_pull_sample()
//   2) 只保存**最新一帧**（mutex 保护）：UI 画得慢就自动丢帧，**永远不会堆积延迟**
//   3) UI 线程用 QTimer 按自己的节奏 takeFrame()，与解码线程完全解耦（慢的一方不影响快的一方）
//
// 为什么不用 Qt 信号槽跨线程：那需要 Q_OBJECT + moc，而 SDK 里没编 moc（见 README）。
// 用 mutex + QTimer 轮询效果一样，还少一层依赖。
//
// 依赖：只用到 libgstreamer / libgstapp / glib / gobject（不依赖 gstvideo、不用 gst_parse 里的元素插件 API）
#include <QImage>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// 标准库名字逐个引入(避免污染包含者)
using std::mutex;
using std::string;
using std::thread;

class GstSource {
public:
    GstSource() = default;
    ~GstSource();
    // 不可拷贝（持有线程与 gst 对象）
    GstSource(const GstSource&) = delete;
    GstSource& operator=(const GstSource&) = delete;

    // port        : 推流端口（板内自连走 127.0.0.1，省掉 WiFi 这个变量）
    // out_w/out_h : 解码后缩放到的尺寸（越小，后续转换/绘制越省 CPU）
    bool start(uint16_t port, int out_w = 640, int out_h = 360);
    void stop();

    bool running() const { return running_.load(); }
    // 取最新帧（有新帧才返回 true）；UI 线程调用
    bool takeFrame(QImage& out);
    uint64_t received() const { return received_.load(); }   // 解码线程收到的总帧数
    uint64_t dropped() const { return dropped_.load(); }     // 被 UI 顶掉的帧数(UI 跟不上)
    string lastError() const;

private:
    void pullLoop();

    void* pipeline_ = nullptr;     // GstPipeline*（擦除类型：头文件不依赖 gst）
    void* sink_ = nullptr;         // GstAppSink*（额外持有引用）
    thread th_;
    std::atomic<bool> running_{false};

    mutex m_;
    QImage latest_;
    bool has_new_ = false;

    std::atomic<uint64_t> received_{0};
    std::atomic<uint64_t> dropped_{0};
    string err_;
};
