#pragma once
// V4L2 采集封装（D6 起实现）
// 先在板端确认设备： v4l2-ctl --list-devices
#include <string>

#include "common/frame.hpp"

class V4l2Camera {
public:
    // dev: 如 /dev/video0；w/h: 采集分辨率；fmt 目标格式
    bool open(const std::string& dev, uint32_t w, uint32_t h);
    void close();
    bool start();   // 入队所有缓冲并开始采集
    void stop();
    // 取回一帧（阻塞，超时返回 false）
    bool getFrame(FramePtr& out, int timeout_ms = 1000);

private:
    int fd_ = -1;
    // TODO(D6)：按 V4L2 标准流程实现：
    //   VIDIOC_S_FMT -> VIDIOC_REQBUFS -> VIDIOC_QUERYBUF -> mmap
    //   -> VIDIOC_QBUF 全部入队 -> 循环 VIDIOC_DQBUF/处理/VIDIOC_QBUF
};
