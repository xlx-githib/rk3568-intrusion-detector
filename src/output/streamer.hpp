#pragma once
// H.264 推流模块（GStreamer appsrc → mpph264enc 硬编码 → MPEG-TS → TCP）
//
// 设计要点：
//  1) 头文件**不引入任何 GStreamer 类型**（用 void* 擦除），所以别的 .cpp 编译时
//     不需要 gst 头文件、链接时也不需要 gst 库 —— 依赖被关在 streamer.cpp 里。
//  2) 采集线程每帧调 push()（喂 NV12，非阻塞，队列满即丢帧）→ 与推理线程完全解耦：
//     检测该丢帧就丢帧，推流该流畅就流畅，互不拖累。
//  3) push() 只在采集线程调用，stop() 只在 join 之后调用 → 无并发访问，不需要加锁。
//  4) 未编译推流功能（ENABLE_STREAM 未开）时是空实现，调用方无需任何 #ifdef。

#include <atomic>
#include <cstddef>
#include <cstdint>

// 标准库名字逐个引入(头文件不用 using namespace std，避免污染包含者)
using std::atomic;

class Streamer {
public:
    Streamer() = default;
    ~Streamer();
    // 不可拷贝（只作成员对象使用）
    Streamer(const Streamer&) = delete;
    Streamer& operator=(const Streamer&) = delete;

    // 启动推流；失败返回 false（调用方降级：照常跑检测，只是没有视频流）
    //   w/h          : 帧尺寸（NV12 要求宽高均为偶数）
    //   port         : TCP 监听端口（板子做服务端，PC 主动连）
    //   bitrate_kbps : 目标码率(<=0 用编码器默认)
    //   gop          : I 帧间隔(<=0 用编码器默认；15 大约 0.5s 一个 I 帧，便于随机接入)
    bool start(uint32_t w, uint32_t h, int fps, int port,
               int bitrate_kbps = 2000, int gop = 15);
    void stop();

    // 推一帧 NV12（w*h*3/2 字节）；非阻塞，未启动或队列满时直接丢帧
    void push(const uint8_t* nv12, size_t bytes, uint64_t pts_us);

    bool running() const { return running_.load(); }
    uint64_t pushed() const { return pushed_.load(); }
    uint64_t dropped() const { return dropped_.load(); }

private:
    void* pipeline_ = nullptr;     // GstPipeline*（擦除类型：避免头文件依赖 gst）
    void* appsrc_ = nullptr;       // GstAppSrc*（额外持有一份引用，stop 时归还）
    uint64_t base_pts_us_ = 0;     // 首帧时间戳，后续换算成相对时间（避免 MPEG-TS 时间轴过大）
    int fps_ = 30;
    atomic<bool> running_{false};
    atomic<uint64_t> pushed_{0}, dropped_{0};
};
