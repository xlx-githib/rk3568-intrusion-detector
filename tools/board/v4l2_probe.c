// V4L2 最小取帧探针(M2-0)：验证裸 V4L2(ioctl+mmap)能否从 rkisp mainpath 出帧
// 用法: ./v4l2_probe <device> <w> <h> <frames>
//   例: ./v4l2_probe /dev/video0 1280 720 20
// 说明: rkisp 节点是 Multiplanar，故用 V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE + mmap。
// 编译(交叉): aarch64-buildroot-linux-gnu-gcc -O2 -o v4l2_probe v4l2_probe.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <linux/videodev2.h>

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "用法: %s <dev> <w> <h> <frames>\n", argv[0]); return 1; }
    const char* dev = argv[1];
    int W = atoi(argv[2]), H = atoi(argv[3]), N = atoi(argv[4]);

    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    // 1) 查询能力
    struct v4l2_capability cap;
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { perror("QUERYCAP"); return 1; }
    printf("[probe] driver=%s card=%s\n", cap.driver, cap.card);

    // 2) 设置格式(Multiplanar, NV12)
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width  = W;
    fmt.fmt.pix_mp.height = H;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field  = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return 1; }
    printf("[probe] S_FMT OK: %dx%d fourcc=%c%c%c%c planes=%d\n",
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
           (fmt.fmt.pix_mp.pixelformat) & 0xff,
           (fmt.fmt.pix_mp.pixelformat >> 8) & 0xff,
           (fmt.fmt.pix_mp.pixelformat >> 16) & 0xff,
           (fmt.fmt.pix_mp.pixelformat >> 24) & 0xff,
           fmt.fmt.pix_mp.num_planes);

    // 3) 申请 4 个 buffer 并 mmap
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return 1; }
    printf("[probe] REQBUFS count=%u\n", req.count);

    void* bufs[8] = {0};
    size_t lens[8] = {0};
    for (unsigned i = 0; i < req.count && i < 8; ++i) {
        struct v4l2_buffer buf; struct v4l2_plane planes[1];
        memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes; buf.length = 1;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) { perror("QUERYBUF"); return 1; }
        lens[i] = planes[0].length;
        bufs[i] = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, planes[0].m.mem_offset);
        if (bufs[i] == MAP_FAILED) { perror("mmap"); return 1; }
        printf("[probe] buf%u mmap %zu bytes ok\n", i, lens[i]);
    }

    // 4) 全部入队 + 开始
    for (unsigned i = 0; i < req.count && i < 8; ++i) {
        struct v4l2_buffer buf; struct v4l2_plane planes[1];
        memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes; buf.length = 1;
        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); return 1; }
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }
    printf("[probe] STREAMON OK, 开始抓 %d 帧...\n", N);

    // 5) 取帧循环
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    for (int i = 0; i < N; ++i) {
        struct v4l2_buffer buf; struct v4l2_plane planes[1];
        memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes; buf.length = 1;
        if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) { perror("DQBUF"); break; }
        if (i % 5 == 0)
            printf("[probe] frame %2d  bytes=%u\n", i, planes[0].bytesused);
        // 归还
        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF2"); break; }
    }
    gettimeofday(&t1, NULL);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
    printf("[probe] %d 帧耗时 %.3f s, 平均 %.1f ms/帧 => %s\n",
           N, dt, dt * 1000 / N,
           (dt > 0 && dt < N) ? "裸 V4L2 出帧 OK" : "无帧/异常(见上方报错)");

    // 清理
    enum v4l2_buf_type off = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(fd, VIDIOC_STREAMOFF, &off);
    for (unsigned i = 0; i < req.count && i < 8; ++i)
        if (bufs[i]) munmap(bufs[i], lens[i]);
    close(fd);
    return 0;
}
