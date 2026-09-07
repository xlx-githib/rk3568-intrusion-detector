// 程序入口（D5 / M1 闭环雏形）
// 模式1(单图): rk3568_intrusion <model.rknn> <input.rgb> <w> <h> [yolov5|yolov7] [out.ppm]
// 模式2(视频帧序列): rk3568_intrusion <model.rknn> video <dir> <roi_x> <roi_y> <roi_w> <roi_h> [stay_sec] [yolov5|yolov7]
//   dir 含 meta.txt(4行: w h fps frames) 与 f_0000.rgb...；帧源由 tools/board/video_to_frames.py 生成
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "infer/rknn_engine.hpp"
#include "business/roi_monitor.hpp"

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
                     int stay_sec, bool use_v7) {
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
    mon.configure(roi, stay_sec, {0, 2});
    printf("[main] ROI=(%d,%d,%d,%d) stay=%ds\n", roi.x, roi.y, roi.w, roi.h, stay_sec);
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

int main(int argc, char** argv) {
    // ---- 模式2：视频帧序列 ----
    if (argc >= 3 && strcmp(argv[2], "video") == 0) {
        if (argc < 8) {
            printf("用法: %s <model.rknn> video <dir> <roi_x> <roi_y> <roi_w> <roi_h> [stay_sec] [yolov5|yolov7]\n", argv[0]);
            return -1;
        }
        RoiRect roi;
        roi.x = atoi(argv[4]); roi.y = atoi(argv[5]);
        roi.w = atoi(argv[6]); roi.h = atoi(argv[7]);
        int stay = (argc > 8) ? atoi(argv[8]) : 3;
        bool v7 = (argc < 10 || strcmp(argv[9], "yolov7") == 0);
        return run_video(argv[1], argv[3], roi, stay, v7);
    }

    // ---- 模式1：单图（D4）----
    if (argc < 5) {
        printf("用法: %s <model.rknn> <input.rgb> <w> <h> [yolov5|yolov7] [out.ppm]\n", argv[0]);
        printf("      或 %s <model.rknn> video <dir> <roi_x> <roi_y> <roi_w> <roi_h> [stay_sec] [yolov5|yolov7]\n", argv[0]);
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
