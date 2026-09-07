// D4 最小检测入口（无 OpenCV，先打通"读图→RKNN→白名单→DetObject"）
// 用法:
//   rk3568_intrusion <model.rknn> <input.rgb> <w> <h> [yolov5|yolov7] [out.ppm]
//   输入 .rgb = 原始 RGB(HWC, 无文件头)；yolov7 为默认(配 yolov7-tiny)
//   out.ppm 可选: 画框结果(P6, 可用常见看图软件打开)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "infer/rknn_engine.hpp"

// 在 RGB 图上画红色矩形边框，写 PPM(P6)
static void draw_boxes_and_write_ppm(const std::string& path,
                                     const std::vector<uint8_t>& rgb,
                                     int w, int h,
                                     const std::vector<DetObject>& objs) {
    std::vector<uint8_t> img = rgb;             // 拷贝一份再画
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
    if (!fp) { printf("[out] 写 %s 失败\n", path.c_str()); return; }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    fwrite(img.data(), 1, img.size(), fp);
    fclose(fp);
    printf("[out] 已写 %s (%dx%d)\n", path.c_str(), w, h);
}

int main(int argc, char** argv) {
    if (argc < 5) {
        printf("用法: %s <model.rknn> <input.rgb> <w> <h> [yolov5|yolov7] [out.ppm]\n", argv[0]);
        return -1;
    }
    const char* model_path = argv[1];
    const char* rgb_path   = argv[2];
    const int   w = atoi(argv[3]);
    const int   h = atoi(argv[4]);
    bool use_v7 = (argc < 6 || strcmp(argv[5], "yolov7") == 0);
    const char* ppm_path  = (argc > 6) ? argv[6] : "out.ppm";

    // ---- 读输入 .rgb ----
    std::ifstream f(rgb_path, std::ios::binary);
    if (!f) { printf("[main] 打不开 %s\n", rgb_path); return -1; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (data.size() != size_t(w) * h * 3) {
        printf("[main] rgb 字节数不符: %zu != %dx%dx3\n", data.size(), w, h);
        return -1;
    }

    // ---- 模型参数：yolov7(默认) / yolov5 ----
    YoloParams p;
    p.num_cls = 80;
    if (!use_v7) {   // yolov5 anchors
        const int v5[3][6] = {{10,13,16,30,33,23},{30,61,62,45,59,119},{116,90,156,198,373,326}};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 6; ++j) p.anchors[i][j] = v5[i][j];
    } else {         // yolov7 anchors (RK_anchors_yolov7.txt)
        const int v7[3][6] = {{12,16,19,36,40,28},{36,75,76,55,72,146},{142,110,192,243,459,401}};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 6; ++j) p.anchors[i][j] = v7[i][j];
    }

    RknnEngine eng;
    std::vector<int> watch = {0, 2};              // 白名单 person/car
    if (!eng.init(model_path, p, watch)) return -1;

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
