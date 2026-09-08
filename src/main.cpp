// 程序入口（D5 / M1 闭环雏形 / M2 实时流水线）
// 模式1(单图): rk3568_intrusion <model.rknn> <input.rgb> <w> <h> [yolov5|yolov7] [out.ppm]
// 模式2(视频帧序列): rk3568_intrusion <model.rknn> video <dir> <roi_x> <roi_y> <roi_w> <roi_h> [stay_sec] [yolov5|yolov7] [leave_confirm]
//   dir 含 meta.txt(4行: w h fps frames) 与 f_0000.rgb...；帧源由 tools/board/video_to_frames.py 生成
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/block_queue.hpp"
#include "infer/rknn_engine.hpp"
#include "business/roi_monitor.hpp"
#include "capture/v4l2_camera.hpp"
#include "output/reporter.hpp"

// 在 RGB 图上画红色矩形边框，写 PPM(P6)
static void draw_boxes_and_write_ppm(const std::string& path,
                                     const std::vector<uint8_t>& rgb,
                                     int w, int h,
                                     const std::vector<DetObject>& objs) {
    std::vector<uint8_t> img = rgb;
    const int thick = 3;
    for (const auto& o : objs) {
        int x1 = int(o.x1), y1 = int(o.y1), x2 = int(o.x2), y2 = int(o.y2);
        auto put = [&](int yy, int xx) {
            if (yy < 0 || yy >= h || xx < 0 || xx >= w) return;
            size_t p = (size_t(yy) * w + xx) * 3;
            img[p] = 255; img[p + 1] = 0; img[p + 2] = 0;   // 红
        };
        for (int t = 0; t < thick; ++t) {
            for (int x = x1 - t; x <= x2 + t; ++x) { put(y1 - t, x); put(y2 + t, x); }
            for (int y = y1 - t; y <= y2 + t; ++y) { put(y, x1 - t); put(y, x2 + t); }
        }
    }
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) { printf("[shot] 写 %s 失败\n", path.c_str()); return; }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    fwrite(img.data(), 1, img.size(), fp);
    fclose(fp);
}

static const char* ev_name(EventType t) {
    switch (t) {
        case EventType::INTRUDE: return "INTRUDE";
        case EventType::ALARM:   return "ALARM";
        case EventType::LEAVE:   return "LEAVE";
        case EventType::RESOLVE: return "RESOLVE";
    }
    return "?";
}

// 画黄色 ROI 框并保存一帧预览(定位警戒区用)
static void draw_roi_and_save(const std::string& path, const FramePtr& f,
                              const RoiRect& roi) {
    std::vector<uint8_t> img = f->data;
    int w = int(f->width), h = int(f->height);
    const int t = 3;
    auto put = [&](int yy, int xx) {
        if (yy < 0 || yy >= h || xx < 0 || xx >= w) return;
        size_t p = (size_t(yy) * w + xx) * 3;
        img[p] = 0; img[p + 1] = 255; img[p + 2] = 255;   // 黄
    };
    for (int k = 0; k < t; ++k) {
        for (int x = roi.x - k; x <= roi.x + roi.w + k; ++x) { put(roi.y - k, x); put(roi.y + roi.h + k, x); }
        for (int y = roi.y - k; y <= roi.y + roi.h + k; ++y) { put(y, roi.x - k); put(y, roi.x + roi.w + k); }
    }
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) return;
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    fwrite(img.data(), 1, img.size(), fp);
    fclose(fp);
    printf("[main] ROI 预览已存 %s (黄框=警戒区, 画面 %dx%d)\n", path.c_str(), w, h);
}

// 生成缩略图(RGB888, 最近邻降采样)：告警帧 → 随事件上报 PC Qt 端显示
static void downscale_rgb(const FramePtr& f, int tw, int th,
                          std::vector<uint8_t>& out) {
    int w = int(f->width), h = int(f->height);
    if (w <= 0 || h <= 0 || tw <= 0 || th <= 0) return;
    out.assign(size_t(tw) * th * 3, 0);
    size_t sx = size_t(w) / tw, sy = size_t(h) / th;   // 抽样步长(整数缩小)
    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;
    for (int ty = 0; ty < th; ++ty) {
        size_t src_y = size_t(ty) * sy;
        if (src_y >= size_t(h)) src_y = size_t(h) - 1;
        const uint8_t* row = f->data.data() + src_y * size_t(w) * 3;
        uint8_t* dst = out.data() + size_t(ty) * tw * 3;
        for (int tx = 0; tx < tw; ++tx) {
            size_t src_x = size_t(tx) * sx;
            if (src_x >= size_t(w)) src_x = size_t(w) - 1;
            memcpy(dst + tx * 3, row + src_x * 3, 3);
        }
    }
}

// 生成 YoloParams（yolov5 / yolov7 共用结构，仅 anchors 不同）
static YoloParams make_params(bool use_v7) {
    YoloParams p;
    if (!use_v7) {
        const int v5[3][6] = {{10,13,16,30,33,23},{30,61,62,45,59,119},{116,90,156,198,373,326}};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 6; ++j) p.anchors[i][j] = v5[i][j];
    } else {
        const int v7[3][6] = {{12,16,19,36,40,28},{36,75,76,55,72,146},{142,110,192,243,459,401}};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 6; ++j) p.anchors[i][j] = v7[i][j];
    }
    return p;
}

// ============ 模式2：视频帧序列 → 逐帧 DetObject → ROI 状态机 → 告警截图 ============
static int run_video(const char* model, const char* dir, const RoiRect& roi,
                     int stay_sec, bool use_v7, int leave_confirm) {
    // 读 meta.txt: 第1行 w, 第2行 h, 第3行 fps, 第4行 frames
    int w = 0, h = 0, fps = 10, frames = 0;
    {
        std::ifstream meta(std::string(dir) + "/meta.txt");
        if (!meta) { printf("[main] 没有 %s/meta.txt\n", dir); return -1; }
        meta >> w >> h >> fps >> frames;
        if (w <= 0 || h <= 0 || frames <= 0) { printf("[main] meta 非法\n"); return -1; }
        printf("[main] 帧序列: %dx%d fps=%d frames=%d\n", w, h, fps, frames);
    }

    RknnEngine eng;
    if (!eng.init(model, make_params(use_v7), {0, 2})) return -1;
    RoiMonitor mon;
    mon.configure(roi, stay_sec, {0, 2}, leave_confirm);
    printf("[main] ROI=(%d,%d,%d,%d) stay=%ds 离开去抖=%d帧\n",
           roi.x, roi.y, roi.w, roi.h, stay_sec, leave_confirm);
    mkdir("shots", 0755);

    int ev_count = 0, alarm_count = 0;
    std::vector<uint8_t> buf;
    for (int i = 0; i < frames; ++i) {
        char path[256];
        snprintf(path, sizeof(path), "%s/f_%04d.rgb", dir, i);
        std::ifstream f(path, std::ios::binary);
        if (!f) { printf("[main] 读取 %s 失败，提前结束\n", path); break; }
        buf.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

        FramePtr frame = std::make_shared<Frame>();
        frame->width = w; frame->height = h;
        frame->fmt = PixelFormat::RGB888;
        frame->data = buf;

        if (i == 0)   // 首帧画 ROI 黄框预览，便于定位警戒区(video 模式无摄像头,只能离线出图)
            draw_roi_and_save("shots/roi_preview_video.ppm", frame, roi);

        std::vector<DetObject> dets;
        eng.infer(frame, dets);
        uint64_t now_us = uint64_t(i) * 1000000ULL / uint64_t(fps);

        // 状态机
        std::vector<Event> evs = mon.feed(dets, now_us);
        if (!evs.empty()) {
            for (const auto& e : evs) {
                ev_count++;
                if (e.type == EventType::ALARM) alarm_count++;
                printf("[frame %4d] EVENT %-7s stay=%llu ms\n", i, ev_name(e.type),
                       (unsigned long long)e.stay_ms);
                // 告警帧画框存图(截图雏形, output 模块在 M2 完善)
                if (e.type == EventType::ALARM) {
                    char shot[256];
                    snprintf(shot, sizeof(shot), "shots/alarm_f%04d.ppm", i);
                    draw_boxes_and_write_ppm(shot, frame->data, w, h, dets);
                    printf("   -> 告警截图: %s\n", shot);
                }
            }
        }
        if (i % 10 == 0)
            printf("[frame %4d/%d] det=%zu\n", i, frames, dets.size());
    }
    eng.release();
    printf("[main] 结束: 事件 %d 次(告警 %d)\n", ev_count, alarm_count);
    return 0;
}

// ============ 模式3：真摄像头实时（M2：V4L2 采集 → 推理 → ROI → 告警，单线程先行）============
static int run_camera(const char* model, const RoiRect& roi, int stay_sec,
                      bool use_v7, int max_frames, int leave_confirm) {
    RknnEngine eng;
    if (!eng.init(model, make_params(use_v7), {0, 2})) return -1;
    RoiMonitor mon;
    mon.configure(roi, stay_sec, {0, 2}, leave_confirm);

    V4l2Camera cam;
    if (!cam.open("/dev/video0", 1280, 720)) return -1;
    if (!cam.start()) return -1;
    mkdir("shots", 0755);
    printf("[cam] /dev/video0 1280x720 ROI=(%d,%d,%d,%d) stay=%ds 离开去抖=%d帧 max=%d帧\n",
           roi.x, roi.y, roi.w, roi.h, stay_sec, leave_confirm, max_frames);

    int got = 0, ev_count = 0, alarm_count = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (max_frames <= 0 || got < max_frames) {
        FramePtr f;
        if (!cam.getFrame(f, 1000)) { printf("[cam] 取帧超时\n"); continue; }
        std::vector<DetObject> dets;
        eng.infer(f, dets);
        std::vector<Event> evs = mon.feed(dets, f->pts_us);   // 用真实时间戳
        for (const auto& e : evs) {
            ev_count++;
            if (e.type == EventType::ALARM) {
                alarm_count++;
                char shot[256];
                snprintf(shot, sizeof(shot), "shots/cam_alarm_%06u.ppm",
                         unsigned(f->pts_us % 1000000));
                draw_boxes_and_write_ppm(shot, f->data, int(f->width), int(f->height), dets);
                printf("[cam] EVENT ALARM -> %s\n", shot);
            } else {
                printf("[cam] EVENT %-7s stay=%llu ms\n", ev_name(e.type),
                       (unsigned long long)e.stay_ms);
            }
        }
        if (++got % 10 == 0) printf("[cam] 已处理 %d 帧\n", got);
    }
    auto t1 = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(t1 - t0).count();
    printf("[cam] 结束: %d 帧 / %.1f s = %.1f fps, 事件 %d(告警 %d)\n",
           got, dt, dt > 0 ? got / dt : 0, ev_count, alarm_count);
    eng.release();
    cam.close();
    return 0;
}

// ---- M2-3：四线程流水线的中间数据类型 ----
struct InferOut {                        // 推理线程 → 业务线程
    FramePtr frame;
    std::vector<DetObject> dets;
};
struct EventMsg {                        // 业务线程 → 输出线程
    Event ev;
    FramePtr frame;
    std::vector<DetObject> dets;
    bool preview = false;                // true=周期性现场预览帧(非事件，Qt 持续刷新画面)
};

// ============ 模式4：四线程流水线（Capture→Infer→Business→Output→上报）============
static int run_pipe(const char* model, const RoiRect& roi, int stay_sec,
                    bool use_v7, int max_frames,
                    const char* report_ip, int report_port, int leave_confirm) {
    RknnEngine eng;
    if (!eng.init(model, make_params(use_v7), {0, 2})) return -1;
    RoiMonitor mon;
    mon.configure(roi, stay_sec, {0, 2}, leave_confirm);
    V4l2Camera cam;
    if (!cam.open("/dev/video0", 1280, 720)) return -1;
    if (!cam.start()) return -1;
    mkdir("shots", 0755);

    {   // 启动先抓一帧存 ROI 预览，便于定位警戒区
        FramePtr pv;
        if (cam.getFrame(pv, 1000)) draw_roi_and_save("shots/roi_preview.ppm", pv, roi);
    }

    Reporter rep;
    rep.init(report_ip && report_ip[0], report_ip ? report_ip : "", report_port);
    if (report_ip && report_ip[0]) rep.connect();

    printf("[pipe] 四线程流水线 ROI=(%d,%d,%d,%d) stay=%ds max=%d帧 上报=%s:%d\n",
           roi.x, roi.y, roi.w, roi.h, stay_sec, max_frames,
           (report_ip && report_ip[0]) ? report_ip : "(off)", report_port);

    BlockQueue<FramePtr> rawQ(2);                       // Capture→Infer
    BlockQueue<std::shared_ptr<InferOut>> resQ(2);      // Infer→Business
    BlockQueue<std::shared_ptr<EventMsg>> evQ(16);      // Business→Output
    std::atomic<int> nInfer{0}, nEvent{0}, nAlarm{0};
    uint64_t lastPreviewUs = 0;                         // 现场预览节流(biz 线程写)
    auto t0 = std::chrono::steady_clock::now();

    std::thread capT([&] {                              // Capture 线程
        int got = 0;
        while (max_frames <= 0 || got < max_frames) {
            FramePtr f;
            if (!cam.getFrame(f, 1000)) continue;
            got++;
            rawQ.push(f);
        }
        rawQ.push(nullptr);                             // EOF 标记
    });

    std::thread infT([&] {                              // Infer 线程
        while (true) {
            FramePtr f;
            if (!rawQ.pop(f, 200)) continue;
            if (!f) { resQ.push(nullptr); break; }      // 收到 EOF
            auto io = std::make_shared<InferOut>();
            io->frame = f;
            eng.infer(f, io->dets);
            nInfer++;
            resQ.push(io);
        }
    });

    std::thread bizT([&] {                              // Business 线程
        while (true) {
            std::shared_ptr<InferOut> io;
            if (!resQ.pop(io, 200)) continue;
            if (!io) { evQ.push(nullptr); break; }
            uint64_t now = io->frame->pts_us;
            // 周期性现场预览帧 → Qt 端持续刷新(准实时画面)
            // 频率 1s/帧 + 160x90(约58KB)：弱 WiFi 也扛得住，避免 TCP 积压
            if (now >= lastPreviewUs + 1000000ULL) {
                lastPreviewUs = now;
                auto pm = std::make_shared<EventMsg>();
                pm->preview = true;
                pm->frame = io->frame;
                evQ.push(pm);
            }
            auto evs = mon.feed(io->dets, now);
            for (const auto& e : evs) {
                auto em = std::make_shared<EventMsg>();
                em->ev = e; em->frame = io->frame; em->dets = io->dets;
                evQ.push(em);
            }
        }
    });

    std::thread outT([&] {                              // Output 线程
        while (true) {
            std::shared_ptr<EventMsg> em;
            if (!evQ.pop(em, 200)) continue;
            if (!em) break;
            if (em->preview) {                          // 现场预览帧(小图省带宽)
                std::vector<uint8_t> thumb;
                const int TW = 160, TH = 90;
                downscale_rgb(em->frame, TW, TH, thumb);
                rep.reportPreview(TW, TH, thumb);
                continue;                               // 非事件，不计数
            }
            if (em->ev.type == EventType::ALARM) {
                nAlarm++;
                char shot[256];
                snprintf(shot, sizeof(shot), "shots/pipe_alarm_%06u.ppm",
                         unsigned(em->frame->pts_us % 1000000));
                draw_boxes_and_write_ppm(shot, em->frame->data,
                                         int(em->frame->width), int(em->frame->height), em->dets);
                printf("[pipe] ALARM -> %s (stay=%llu ms)\n", shot,
                       (unsigned long long)em->ev.stay_ms);
                // 缩略图(RGB)随事件上报，PC Qt 端实时显示告警画面
                std::vector<uint8_t> thumb;
                const int TW = 320, TH = 180;     // 1280x720 → 320x180
                downscale_rgb(em->frame, TW, TH, thumb);
                rep.reportImg(em->ev, shot, TW, TH, thumb);
            } else {
                printf("[pipe] EVENT %-7s stay=%llu ms\n", ev_name(em->ev.type),
                       (unsigned long long)em->ev.stay_ms);
                rep.report(em->ev);
            }
            nEvent++;
        }
    });

    capT.join(); infT.join(); bizT.join(); outT.join();
    rep.disconnect();
    auto t1 = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(t1 - t0).count();
    printf("[pipe] 结束: infer %d 帧 / %.1f s = %.1f fps, 事件 %d(告警 %d)\n",
           nInfer.load(), dt, dt > 0 ? nInfer.load() / dt : 0,
           nEvent.load(), nAlarm.load());
    eng.release();
    cam.close();
    return 0;
}

int main(int argc, char** argv) {
    // ---- 模式2：视频帧序列 ----
    if (argc >= 3 && strcmp(argv[2], "video") == 0) {
        if (argc < 8) {
            printf("用法: %s <model.rknn> video <dir> <roi_x> <roi_y> <roi_w> <roi_h> [stay_sec] [yolov5|yolov7] [leave_confirm]\n", argv[0]);
            return -1;
        }
        RoiRect roi;
        roi.x = atoi(argv[4]); roi.y = atoi(argv[5]);
        roi.w = atoi(argv[6]); roi.h = atoi(argv[7]);
        int stay = (argc > 8) ? atoi(argv[8]) : 3;
        bool v7 = (argc < 10 || strcmp(argv[9], "yolov7") == 0);
        int lc = (argc > 10) ? atoi(argv[10]) : 5;   // 离开去抖帧数(连续缺席 N 帧才算离开)
        return run_video(argv[1], argv[3], roi, stay, v7, lc);
    }

    // ---- 模式3：真摄像头实时 ----
    if (argc >= 3 && strcmp(argv[2], "cam") == 0) {
        if (argc < 7) {
            printf("用法: %s <model> cam <roi_x> <roi_y> <roi_w> <roi_h> [stay_sec] [yolov5|yolov7] [max_frames] [leave_confirm]\n", argv[0]);
            return -1;
        }
        // argv[3..6]=roi, argv[7]=stay, argv[8]=yolov5/7, argv[9]=max, argv[10]=leave_confirm(注意无 dir 参数)
        RoiRect roi;
        roi.x = atoi(argv[3]); roi.y = atoi(argv[4]);
        roi.w = atoi(argv[5]); roi.h = atoi(argv[6]);
        int stay = (argc > 7) ? atoi(argv[7]) : 3;
        bool v7 = (argc < 9 || strcmp(argv[8], "yolov7") == 0);
        int maxf = (argc > 9) ? atoi(argv[9]) : 300;
        int lc = (argc > 10) ? atoi(argv[10]) : 5;   // 离开去抖帧数
        return run_camera(argv[1], roi, stay, v7, maxf, lc);
    }

    // ---- 模式4：四线程流水线 ----
    if (argc >= 3 && strcmp(argv[2], "pipe") == 0) {
        if (argc < 7) {
            printf("用法: %s <model> pipe <roi_x> <roi_y> <roi_w> <roi_h> [stay_sec] [yolov5|yolov7] [max_frames] [report_ip report_port] [leave_confirm]\n", argv[0]);
            return -1;
        }
        RoiRect roi;
        roi.x = atoi(argv[3]); roi.y = atoi(argv[4]);
        roi.w = atoi(argv[5]); roi.h = atoi(argv[6]);
        int stay = (argc > 7) ? atoi(argv[7]) : 3;
        bool v7 = (argc < 9 || strcmp(argv[8], "yolov7") == 0);
        int maxf = (argc > 9) ? atoi(argv[9]) : 300;
        const char* rip = (argc > 10) ? argv[10] : "";
        int rport = (argc > 11) ? atoi(argv[11]) : 9000;
        int lc = (argc > 12) ? atoi(argv[12]) : 5;   // 离开去抖帧数
        return run_pipe(argv[1], roi, stay, v7, maxf, rip, rport, lc);
    }

    // ---- 模式1：单图（D4）----
    if (argc < 5) {
        printf("用法: %s <model.rknn> <input.rgb> <w> <h> [yolov5|yolov7] [out.ppm]\n", argv[0]);
        printf("      或 %s <model.rknn> video <dir> <roi_x> <roi_y> <roi_w> <roi_h> [stay_sec] [yolov5|yolov7]\n", argv[0]);
        printf("      或 %s <model.rknn> cam <roi_x> <roi_y> <roi_w> <roi_h> [stay_sec] [yolov5|yolov7] [max_frames]\n", argv[0]);
        return -1;
    }
    const char* model_path = argv[1];
    const char* rgb_path   = argv[2];
    const int   w = atoi(argv[3]);
    const int   h = atoi(argv[4]);
    bool use_v7 = (argc < 6 || strcmp(argv[5], "yolov7") == 0);
    const char* ppm_path  = (argc > 6) ? argv[6] : "out.ppm";

    std::ifstream f(rgb_path, std::ios::binary);
    if (!f) { printf("[main] 打不开 %s\n", rgb_path); return -1; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (data.size() != size_t(w) * h * 3) {
        printf("[main] rgb 字节数不符: %zu != %dx%dx3\n", data.size(), w, h);
        return -1;
    }

    RknnEngine eng;
    if (!eng.init(model_path, make_params(use_v7), {0, 2})) return -1;

    FramePtr frame = std::make_shared<Frame>();
    frame->width = w; frame->height = h;
    frame->fmt = PixelFormat::RGB888;
    frame->data.swap(data);

    std::vector<DetObject> objs;
    if (!eng.infer(frame, objs)) { printf("[main] infer 失败\n"); return -1; }

    printf("==== 检测结果(白名单 person=0/car=2) ====\n");
    for (const auto& o : objs)
        printf("  cls=%d conf=%.3f box=(%.0f,%.0f,%.0f,%.0f) bottom=(%.0f,%.0f)\n",
               o.cls_id, o.conf, o.x1, o.y1, o.x2, o.y2, o.bx, o.by);

    draw_boxes_and_write_ppm(ppm_path, frame->data, w, h, objs);
    eng.release();
    printf("done.\n");
    return 0;
}
