// V4L2 采集实现（M2，基于板上验证通过的 Multiplanar NV12 + mmap 流程）
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <linux/videodev2.h>

#include "capture/v4l2_camera.hpp"

namespace {
int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}
uint64_t now_us() {
    struct timeval tv; gettimeofday(&tv, nullptr);
    return uint64_t(tv.tv_sec) * 1000000ULL + tv.tv_usec;
}
}  // namespace

bool V4l2Camera::open(const std::string& dev, uint32_t w, uint32_t h) {
    w_ = w; h_ = h;
    fd_ = ::open(dev.c_str(), O_RDWR);
    if (fd_ < 0) { printf("[v4l2] open %s 失败\n", dev.c_str()); return false; }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width  = w;
    fmt.fmt.pix_mp.height = h;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field  = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) { perror("[v4l2] S_FMT"); ::close(fd_); fd_ = -1; return false; }
    w_ = fmt.fmt.pix_mp.width; h_ = fmt.fmt.pix_mp.height;

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) { perror("[v4l2] REQBUFS"); return false; }
    nbufs_ = req.count;

    for (unsigned i = 0; i < nbufs_ && i < 8; ++i) {
        struct v4l2_buffer buf; struct v4l2_plane planes[1];
        memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes; buf.length = 1;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) { perror("[v4l2] QUERYBUF"); return false; }
        bufs_[i].length = planes[0].length;
        bufs_[i].start = mmap(nullptr, planes[0].length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd_, planes[0].m.mem_offset);
        if (bufs_[i].start == MAP_FAILED) { perror("[v4l2] mmap"); bufs_[i].start = nullptr; return false; }
    }
    printf("[v4l2] open %s %ux%u NV12, %u buffers\n", dev.c_str(), w_, h_, nbufs_);
    return true;
}

bool V4l2Camera::start() {
    for (unsigned i = 0; i < nbufs_; ++i) {
        struct v4l2_buffer buf; struct v4l2_plane planes[1];
        memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes; buf.length = 1;
        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) { perror("[v4l2] QBUF"); return false; }
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) { perror("[v4l2] STREAMON"); return false; }
    started_ = true;
    return true;
}

bool V4l2Camera::getFrame(FramePtr& out, int timeout_ms) {
    struct pollfd pfd = { fd_, POLLIN, 0 };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return false;               // 超时/无数据

    struct v4l2_buffer buf; struct v4l2_plane planes[1];
    memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes; buf.length = 1;
    if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) { perror("[v4l2] DQBUF"); return false; }

    // NV12(mmap) -> RGB888 Frame（拷贝，后续可换 RGA 减少拷贝）
    FramePtr f = std::make_shared<Frame>();
    f->width = w_; f->height = h_;
    f->fmt = PixelFormat::RGB888;
    f->pts_us = now_us();
    f->data.resize(size_t(w_) * h_ * 3);
    nv12_to_rgb(bufs_[buf.index].start, f->data.data());

    // 归还缓冲
    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) { perror("[v4l2] QBUF2"); }
    out = f;
    return true;
}

void V4l2Camera::stop() {
    if (started_) {
        enum v4l2_buf_type off = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(fd_, VIDIOC_STREAMOFF, &off);
        started_ = false;
    }
}

void V4l2Camera::close() {
    stop();
    for (unsigned i = 0; i < nbufs_; ++i)
        if (bufs_[i].start) { munmap(bufs_[i].start, bufs_[i].length); bufs_[i].start = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    nbufs_ = 0;
}

// NV12(Y 平面 + 交错 UV 平面) -> RGB888(BT.601 简化式)
void V4l2Camera::nv12_to_rgb(const void* nv12, uint8_t* rgb) const {
    const uint8_t* yp = (const uint8_t*)nv12;
    const uint8_t* uv = yp + size_t(w_) * h_;          // UV 平面: CbCr 交错
    const int w = int(w_), h = int(h_);
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            int yy = yp[row * w + col];
            int uv_i = (row / 2) * w + (col & ~1);     // 2x2 共享一个 UV 对
            int u = uv[uv_i];                           // Cb
            int v = uv[uv_i + 1];                       // Cr
            int r = yy + 1.402f * (v - 128);
            int g = yy - 0.344f * (u - 128) - 0.714f * (v - 128);
            int b = yy + 1.772f * (u - 128);
            auto clip = [](int x) { return x < 0 ? 0 : (x > 255 ? 255 : x); };
            uint8_t* p = rgb + (size_t(row) * w + col) * 3;
            p[0] = uint8_t(clip(r)); p[1] = uint8_t(clip(g)); p[2] = uint8_t(clip(b));
        }
    }
}
