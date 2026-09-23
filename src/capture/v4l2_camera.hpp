#pragma once
// V4L2 采集封装（M2：Multiplanar NV12 + mmap，输出 RGB888 Frame）
// 板上已验证：/dev/video0(rkisp_mainpath) 裸 V4L2 可出 NV12 帧
#include <string>

#include "common/frame.hpp"

// 标准库名字逐个引入(头文件不用 using namespace std，避免污染包含者)
using std::string;

class V4l2Camera {
public:
    // dev: /dev/video0；w/h: 采集分辨率（内部协商 NV12）
    bool open(const string& dev, uint32_t w, uint32_t h);
    void close();
    bool start();   // 入队所有缓冲并开始采集
    void stop();
    // 取回一帧（阻塞 timeout_ms；成功时 out 为 RGB888，含 pts_us）
    // keep_nv12_ 为真时，同时把原始 NV12 拷进 Frame::nv12（供 H.264 硬编码使用）
    bool getFrame(FramePtr& out, int timeout_ms = 1000);

    // 是否随帧多留一份 NV12（默认关：不推流就不付这份拷贝开销）
    void setKeepNv12(bool on) { keep_nv12_ = on; }
    // 协商后的实际采集尺寸（open 之后才有效：驱动可能改动请求的 w/h）
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }

private:
    struct Buf { void* start = nullptr; size_t length = 0; };

    int fd_ = -1;
    uint32_t w_ = 0, h_ = 0;
    unsigned nbufs_ = 0;
    Buf bufs_[8];
    bool started_ = false;
    bool keep_nv12_ = false;

    // NV12 -> RGB888 (BT.601)
    void nv12_to_rgb(const void* nv12, uint8_t* rgb) const;
};
