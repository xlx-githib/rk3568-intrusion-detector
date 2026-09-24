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
#include "common/console.hpp"
#include "infer/rknn_engine.hpp"
#include "business/roi_monitor.hpp"
#include "capture/v4l2_camera.hpp"
#include "output/reporter.hpp"
#include "output/statlink.hpp"
#include "output/streamer.hpp"

using namespace std;

// 在 RGB 图上画红色矩形边框，写 PPM(P6)
static void draw_boxes_and_write_ppm(const string& path,
                                     const vector<uint8_t>& rgb,
                                     int w, int h,
                                     const vector<DetObject>& objs) {
    vector<uint8_t> img = rgb;//深拷贝
    const int thick = 3;//厚度
    for (const auto& o : objs) {
        int x1 = int(o.x1), y1 = int(o.y1), x2 = int(o.x2), y2 = int(o.y2);
        auto put = [&](int yy, int xx) {
            if (yy < 0 || yy >= h || xx < 0 || xx >= w) return;
            size_t p = (size_t(yy) * w + xx) * 3;//避免乘法溢出或负数索引问题
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
static void draw_roi_and_save(const string& path, const FramePtr& f,
                              const RoiRect& roi) {
    vector<uint8_t> img = f->data;
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
                          vector<uint8_t>& out) {
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
// 存在意义：帧序列是"死"的，同一段输入跑两遍结果完全一样 → 用于回归测试和调参(stay_sec/leave_confirm)。
// 与 run_camera 的区别只有两点：帧从文件读；时间戳按 fps 自己合成(见下面 now_us)。
// static = 文件内部链接(与 C 相同)：符号不导出，别的 .cpp 有同名函数也不冲突。
// const RoiRect& = 只读引用：不拷贝、不改动；调用方写 run_video(..., roi, ...) 即可，不用取地址。
static int run_video(const char* model, const char* dir, const RoiRect& roi,
                     int stay_sec, bool use_v7, int leave_confirm) {
    // ---- 1) 读 meta.txt: 第1行 w, 第2行 h, 第3行 fps, 第4行 frames ----
    // meta.txt 由 tools/board/video_to_frames.py 生成，字段顺序必须与下面的 >> 顺序一致
    int w = 0, h = 0, fps = 10, frames = 0;
    {   // 裸作用域块：唯一目的是让 ifstream 出块即析构(=自动 close)，不必像 C 那样手写 fclose
        // string(dir) 必须先转：dir 是 const char*，"char* + 字面量" 是指针地址加法(野指针)，
        // 只有 std::string 才重载了 operator+，能做真正的字符串拼接
        ifstream meta(string(dir) + "/meta.txt");//转string 使用str1 + str2
        // ifstream 有 operator bool：打开失败为 false(等价 !meta.is_open())
        if (!meta) { printf("[main] 没有 %s/meta.txt\n", dir); return -1; }
        // >> 是流提取运算符，返回 istream& 所以能链式串写；按空白符自动分隔并转数字
        // 注意：C++11 起抽取失败会把目标变量置 0(不是保留旧值)，所以上面的初值只是表达意图
        meta >> w >> h >> fps >> frames;
        // 真正兜底靠这里。已知缺陷：没校验 fps，若 fps 为 0，下面算 now_us 会除零
        if (w <= 0 || h <= 0 || frames <= 0) { printf("[main] meta 非法\n"); return -1; }
        printf("[main] 帧序列: %dx%d fps=%d frames=%d\n", w, h, fps, frames);
    }

    // ---- 2) 起推理引擎 + 配状态机 ----
    // 栈对象：析构函数 ~RknnEngine() 会自动调 release()，中途 return -1 也不会泄漏 NPU 上下文(RAII)
    RknnEngine eng;
    // make_params(use_v7)：v5/v7 解码公式相同，只有 anchors 数值不同
    // {0, 2} 是花括号初始化列表 → 编译器造临时 vector<int>{0,2} 绑到 const&，
    //        临时量生命周期延长到整条语句，函数内使用安全
    // 白名单语义：COCO 类别 0=person, 2=car，其余 78 类一律丢弃(通用检测 → 入侵检测的关键一步)
    if (!eng.init(model, make_params(use_v7), {0, 2})) return -1;
    RoiMonitor mon;
    // 同样传 {0,2} 不重复：引擎层过滤是性能优化(少算少传)，状态机层过滤是逻辑正确性，各管一段
    mon.configure(roi, stay_sec, {0, 2}, leave_confirm);
    printf("[main] ROI=(%d,%d,%d,%d) stay=%ds 离开去抖=%d帧\n",
           roi.x, roi.y, roi.w, roi.h, stay_sec, leave_confirm);
    // POSIX 调用(<sys/stat.h>)；0755 是八进制权限；目录已存在会返回 -1，此处不检查(幂等写法)
    // 注意是相对路径：必须在期望的工作目录下启动程序，否则截图会散落各处
    mkdir("shots", 0755);

    // ---- 3) 主循环：逐帧 读文件 → 推理 → 状态机 → 存告警图 ----
    int ev_count = 0, alarm_count = 0;
    // buf 放循环外 = 复用堆内存：一帧 1280x720x3 ≈ 2.7MB，放外面只分配一次，之后原地覆盖
    vector<uint8_t> buf;
    for (int i = 0; i < frames; ++i) {
        // 文件名必须与 video_to_frames.py 的命名约定一致(%04d 补零 → f_0000.rgb)
        char path[256];
        snprintf(path, sizeof(path), "%s/f_%04d.rgb", dir, i);   // 带长度版本，防栈溢出
        ifstream f(path, ios::binary);   // ios::binary 必需：否则 \r\n 会被当换行转换，图像字节被改坏
        // 用 break 而非 return：跳出去后仍要走 eng.release() 和最终统计打印
        if (!f) { printf("[main] 读取 %s 失败，提前结束\n", path); break; }
        // 一行读完整个文件(C++ 惯用法)：istreambuf_iterator(f) 指向文件开头，
        // 无参 istreambuf_iterator<char>() 是 EOF 哨兵；不需要预先知道文件大小(对比 fseek+ftell+fread)
        // assign 不释放已有容量 → 复用 buf 内存；外层小括号是"构造函数版"沿袭写法(那里用于防 Most Vexing Parse)
        buf.assign((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());

        // FramePtr = shared_ptr<Frame>：引用计数智能指针，跨函数/线程传递时由最后一个持有者自动回收，
        // 免去"谁负责 free"的扯皮；make_shared 一次分配同时得到对象+控制块(比 shared_ptr<T>(new T) 少一次分配)
        FramePtr frame = make_shared<Frame>();
        frame->width = w; frame->height = h;
        // 必须显式改 RGB888：Frame 默认 fmt 是 BGR888，不改则 infer 会走 BGR→RGB 分支，红蓝颠倒、精度塌陷
        frame->fmt = PixelFormat::RGB888;
        // 深拷贝 2.7MB(vector 的 operator=)：因为 buf 下一轮还要读下一帧，不能 move 走。
        // 对比单图模式用 frame->data.swap(data)：那里只处理一帧，可零拷贝过户。这是本函数可优化点之一。
        frame->data = buf;
        // 注意 pts_us 故意留空(=0)：离线模式没有真实采集时刻，时间戳在下面用 i 和 fps 合成

        if (i == 0)   // 首帧画 ROI 黄框预览，便于定位警戒区(video 模式无摄像头,只能离线出图)
            draw_roi_and_save("shots/roi_preview_video.ppm", frame, roi);

        // 出参风格(继承 C 的 int f(In, Out*))：返回值表达成功/失败，结果用引用带出。
        // 若改成 return vector<DetObject>，就没法区分"失败"和"没检测到目标"，语义会模糊。
        vector<DetObject> dets;
        eng.infer(frame, dets);
        // ★本函数最关键的原理：伪造时间轴(虚拟时钟)
        //   第 i 帧的"剧情时刻" = i/fps 秒 = i*10^6/fps 微秒。
        //   若改用真实时钟：离线跑 300 帧只要几秒，而告警判据是"真实流逝 stay_sec 秒"，永远触发不了告警。
        //   所以业务逻辑只依赖传入的时间戳、绝不读墙上时钟 —— 这是可测试性的前提(状态机因此能加速回放/单测)。
        //   先乘后除：反过来写 i/fps 会因整数除法截断成 0。
        //   uint64_t(i) 显式转换是防雷：若常量将来去掉 ULL 后缀，i*1000000 会以 int 计算并溢出(UB)。
        uint64_t now_us = uint64_t(i) * 1000000ULL / uint64_t(fps);

        // 状态机：IDLE → INSIDE(计时) → ALARMED；离开时靠 leave_confirm 帧去抖
        vector<Event> evs = mon.feed(dets, now_us);
        if (!evs.empty()) {    // empty() 是 O(1)，不用写 size() > 0
            // 范围 for(= begin/end 迭代器循环)；Event 内含 std::string，必须用引用，否则每轮都拷贝
            for (const auto& e : evs) {
                ev_count++;
                // enum class 是强类型枚举：必须带作用域 EventType::，且不能隐式转 int(比 C 的 enum 安全)
                if (e.type == EventType::ALARM) alarm_count++;
                printf("[frame %4d] EVENT %-7s stay=%llu ms\n", i, ev_name(e.type),
                       (unsigned long long)e.stay_ms);   // 显式转型：uint64_t 的底类型跨平台不保证是 unsigned long long
                // 告警帧画框存图(截图雏形, output 模块在 M2 完善)
                if (e.type == EventType::ALARM) {
                    char shot[256];
                    snprintf(shot, sizeof(shot), "shots/alarm_f%04d.ppm", i);
                    // 传的是 frame->data，但函数内部先深拷贝再画 → 原帧保持干净。
                    // (run_pipe 里同一帧还要缩图上报，不能画脏；低频路径多一次 2.7MB 拷贝，这个权衡值得)
                    draw_boxes_and_write_ppm(shot, frame->data, w, h, dets);
                    printf("   -> 告警截图: %s\n", shot);
                }
            }
        }
        // 打印节流：printf 走 write 系统调用，比计算慢几个数量级，每帧刷屏会让 fps 统计失真
        // %zu 对应 size_t(dets.size() 的返回类型)
        if (i % 10 == 0)
            printf("[frame %4d/%d] det=%zu\n", i, frames, dets.size());
    }
    // 显式释放 NPU(析构函数也会调，且 release 内部置空、可重复调用)：目的是确定释放时机
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
    auto t0 = chrono::steady_clock::now();
    while (max_frames <= 0 || got < max_frames) {
        FramePtr f;
        if (!cam.getFrame(f, 1000)) { printf("[cam] 取帧超时\n"); continue; }
        vector<DetObject> dets;
        eng.infer(f, dets);
        vector<Event> evs = mon.feed(dets, f->pts_us);   // 用真实时间戳
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
    auto t1 = chrono::steady_clock::now();
    double dt = chrono::duration<double>(t1 - t0).count();
    printf("[cam] 结束: %d 帧 / %.1f s = %.1f fps, 事件 %d(告警 %d)\n",
           got, dt, dt > 0 ? got / dt : 0, ev_count, alarm_count);
    eng.release();
    cam.close();
    return 0;
}

// ---- M2-3：四线程流水线的中间数据类型 ----
struct InferOut {                        // 推理线程 → 业务线程
    FramePtr frame;
    vector<DetObject> dets;
};
struct EventMsg {                        // 业务线程 → 输出线程
    Event ev;
    FramePtr frame;
    vector<DetObject> dets;
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

    // ---- 可选：H.264 推流 ----
    // 用环境变量开启，不占命令行参数位（不改 argv 索引 → 不会影响已有的参数解析）
    //   STREAM_PORT=5000  监听端口（不设/为 0 = 关）
    //   STREAM_BPS=2000   目标码率 kbps
    //   STREAM_GOP=15     I 帧间隔
    Streamer strm;
    int stream_port = 0, stream_bps = 2000, stream_gop = 15;
    if (const char* v = getenv("STREAM_PORT")) stream_port = atoi(v);
    if (const char* v = getenv("STREAM_BPS"))  stream_bps  = atoi(v);
    if (const char* v = getenv("STREAM_GOP"))  stream_gop  = atoi(v);
    if (stream_port > 0) {
        cam.setKeepNv12(true);   // 采集层多留一份 NV12 给硬编码器
        if (strm.start(cam.width(), cam.height(), 30, stream_port, stream_bps, stream_gop))
            printf("[pipe] PC 侧播放: ffplay -fflags nobuffer -flags low_delay -probesize 32 "
                   "-analyzeduration 0 -framedrop -f mpegts tcp://<板IP>:%d\n", stream_port);
    }

    // ---- 本机状态通道（给板端 Qt 界面）：默认 9100，STAT_PORT=0 可关 ----
    StatLink stat;
    int stat_port = 9100;
    if (const char* v = getenv("STAT_PORT")) stat_port = atoi(v);
    if (stat_port > 0) stat.start(stat_port);

    Reporter rep;
    rep.init(report_ip && report_ip[0], report_ip ? report_ip : "", report_port);
    if (report_ip && report_ip[0]) rep.connect(1200);   // 自带超时：PC 没开上位机也不拖慢启动

    printf("[pipe] 四线程流水线 ROI=(%d,%d,%d,%d) stay=%ds max=%d帧 上报=%s:%d\n",
           roi.x, roi.y, roi.w, roi.h, stay_sec, max_frames,
           (report_ip && report_ip[0]) ? report_ip : "(off)", report_port);

    BlockQueue<FramePtr> rawQ(2);                       // Capture→Infer
    BlockQueue<shared_ptr<InferOut>> resQ(2);      // Infer→Business
    BlockQueue<shared_ptr<EventMsg>> evQ(16);      // Business→Output
    atomic<int> nInfer{0}, nEvent{0}, nAlarm{0};
    uint64_t lastPreviewUs = 0;                         // 现场预览节流(biz 线程写)
    uint64_t lastStatUs = 0;                            // 状态推送节流(biz 线程写)
    string lastEvName;                                  // 最近一条事件(发给板端 Qt 画事件条)
    uint64_t lastEvStay = 0;
    auto t0 = chrono::steady_clock::now();

    // ---- 运行时控制台(M3)：在线调参/热更新，免去改参重编译 ----
    RtParams rt;
    rt.roi_x = roi.x; rt.roi_y = roi.y; rt.roi_w = roi.w; rt.roi_h = roi.h;
    rt.frame_w = 1280; rt.frame_h = 720;
    rt.stay_sec = stay_sec;
    rt.leave_confirm = leave_confirm;
    {
        YoloParams dp = make_params(use_v7);
        rt.conf_pct = int(dp.conf_thresh * 100 + 0.5f);
        rt.nms_pct  = int(dp.nms_thresh * 100 + 0.5f);
    }
    EventLog evlog;
    atomic<bool> quit{false};
    uint32_t applied_ver = 0;
    Console con;
    con.start(&rt, "config/intruder.conf",
              [&]() -> string {
                  double dts = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
                  char b[512];
                  snprintf(b, sizeof(b),
                           "[status] pipe 运行 %.0fs · 帧率 %.1f fps\n"
                           "  ROI=(%d,%d,%d,%d) stay=%ds leave=%d conf=%.2f nms=%.2f log=%d report=%s\n"
                           "  累计 infer=%d 事件=%d 告警=%d | 队列 raw=%zu res=%zu ev=%zu\n"
                           "  上报 %s:%d 连接=%s | 推流 %s 成功=%llu 丢=%llu",
                           dts, dts > 0 ? nInfer.load() / dts : 0.0,
                           rt.roi_x.load(), rt.roi_y.load(), rt.roi_w.load(), rt.roi_h.load(),
                           rt.stay_sec.load(), rt.leave_confirm.load(),
                           rt.conf_pct.load() / 100.0, rt.nms_pct.load() / 100.0,
                           rt.log_level.load(), rt.report_on.load() ? "on" : "off",
                           nInfer.load(), nEvent.load(), nAlarm.load(),
                           rawQ.size(), resQ.size(), evQ.size(),
                           (report_ip && report_ip[0]) ? report_ip : "(off)", report_port,
                           rep.connected() ? "yes" : "no",
                           strm.running() ? "on" : "off",
                           (unsigned long long)strm.pushed(),
                           (unsigned long long)strm.dropped());
                  return string(b);
              },
              [&](int n) { return evlog.dump(n); },
              &quit);

    thread capT([&] {                              // Capture 线程
        int got = 0;
        while (!quit.load() && (max_frames <= 0 || got < max_frames)) {
            FramePtr f;
            if (!cam.getFrame(f, 1000)) continue;
            got++;
            // 推流与推理解耦：这里每帧都推（包括因队列满而没进推理的帧）
            //  → 视频流畅、检测依旧保实时，两个目标不互相掣肘
            if (strm.running() && !f->nv12.empty())
                strm.push(f->nv12.data(), f->nv12.size(), f->pts_us);
            rawQ.push(f);
        }
        rawQ.push(nullptr);                             // EOF 标记
    });

    thread infT([&] {                              // Infer 线程
        while (true) {
            FramePtr f;
            if (!rawQ.pop(f, 200)) continue;
            if (!f) { resQ.push(nullptr); break; }      // 收到 EOF
            auto io = make_shared<InferOut>();
            io->frame = f;
            eng.infer(f, io->dets);
            nInfer++;
            resQ.push(io);
        }
    });

    thread bizT([&] {                              // Business 线程
        while (true) {
            shared_ptr<InferOut> io;
            if (!resQ.pop(io, 200)) continue;
            if (!io) { evQ.push(nullptr); break; }
            uint64_t now = io->frame->pts_us;
            // 运行时参数热更新(控制台改参后下一帧立即生效)
            if (rt.version.load() != applied_ver) {
                applied_ver = rt.version.load();
                RoiRect r; r.x = rt.roi_x; r.y = rt.roi_y; r.w = rt.roi_w; r.h = rt.roi_h;
                mon.setRoi(r);
                mon.setStaySec(rt.stay_sec);
                mon.setLeaveConfirm(rt.leave_confirm);
                eng.setThresh(rt.conf_pct.load() / 100.0f, rt.nms_pct.load() / 100.0f);
                printf("[con] 参数已应用: ROI=(%d,%d,%d,%d) stay=%ds leave=%d conf=%.2f nms=%.2f\n",
                       r.x, r.y, r.w, r.h, rt.stay_sec.load(), rt.leave_confirm.load(),
                       rt.conf_pct.load() / 100.0, rt.nms_pct.load() / 100.0);
            }
            // 周期性现场预览帧 → Qt 端持续刷新(准实时画面)
            // ~3fps + 320x180(约230KB)：手机热点等快网流畅清晰；弱 WiFi 可改回 1s/160x90
            if (now >= lastPreviewUs + 333333ULL) {
                lastPreviewUs = now;
                auto pm = make_shared<EventMsg>();
                pm->preview = true;
                pm->frame = io->frame;
                evQ.push(pm);
            }
            auto evs = mon.feed(io->dets, now);
            for (const auto& e : evs) {
                lastEvName = ev_name(e.type);           // 记给状态通道(板端 Qt)
                lastEvStay = e.stay_ms;
                auto em = make_shared<EventMsg>();
                em->ev = e; em->frame = io->frame; em->dets = io->dets;
                evQ.push(em);
            }
            // 板端 Qt 界面发来的控制命令（触摸调 ROI 等）→ 转交运行时控制台执行。
            // 这样“屏幕拖动”和“终端敲命令”走的是同一条解析与热更新链路，不用写第二份逻辑。
            {
                string cmd;
                while (stat.takeCommand(cmd)) con.submit(cmd);
            }
            // 每 100ms 把状态推给板端 Qt（ROI/检测框/统计/最近事件）
            // 注意快照是值拷贝，拷完就撒手：不阻塞数据路径，也不和 UI 争锁
            if (now >= lastStatUs + 100000ULL) {
                lastStatUs = now;
                double dts = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
                StateSnapshot ss;
                ss.frame_w = int(io->frame->width);
                ss.frame_h = int(io->frame->height);
                ss.roi_x = rt.roi_x; ss.roi_y = rt.roi_y;
                ss.roi_w = rt.roi_w; ss.roi_h = rt.roi_h;
                ss.stay_sec = rt.stay_sec; ss.leave_confirm = rt.leave_confirm;
                ss.conf_pct = rt.conf_pct; ss.nms_pct = rt.nms_pct;
                ss.log_level = rt.log_level; ss.report_on = rt.report_on;
                ss.fps = dts > 0 ? nInfer.load() / dts : 0.0;
                ss.infer = nInfer.load(); ss.events = nEvent.load(); ss.alarms = nAlarm.load();
                ss.pushed = strm.pushed(); ss.dropped = strm.dropped();
                ss.dets = io->dets;
                ss.last_event = lastEvName; ss.last_stay_ms = lastEvStay;
                stat.publish(ss);
            }
        }
    });

    thread outT([&] {                              // Output 线程
        while (true) {
            shared_ptr<EventMsg> em;
            if (!evQ.pop(em, 200)) continue;
            if (!em) break;
            if (em->preview) {                          // 现场预览帧
                // 退出中就别再编码/上报了：预览是周期性产生的，清干净 evQ 才能让 join 立刻返回
                // （曾出过 quit 卡住：上报断开后这里每次都要 connect 重连，旧版阻塞 connect 会等几十秒）
                if (quit.load()) continue;
                vector<uint8_t> thumb;
                const int TW = 320, TH = 180;     // 清晰度优先(快网)
                downscale_rgb(em->frame, TW, TH, thumb);
                if (rt.report_on.load()) rep.reportPreview(TW, TH, thumb);
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
                // 缩略图(RGB)随事件上报，PC Qt 端实时显示并自动存档
                vector<uint8_t> thumb;
                const int TW = 640, TH = 360;     // 1280x720 → 640x360(清晰，便于看清谁)
                downscale_rgb(em->frame, TW, TH, thumb);
                if (rt.report_on.load()) rep.reportImg(em->ev, shot, TW, TH, thumb);
            } else {
                if (rt.log_level.load() <= 1)
                    printf("[pipe] EVENT %-7s stay=%llu ms\n", ev_name(em->ev.type),
                           (unsigned long long)em->ev.stay_ms);
                if (rt.report_on.load()) rep.report(em->ev);
            }
            {   // 事件历史(供控制台 events 命令查阅)
                char hb[256];
                snprintf(hb, sizeof(hb), "%s cls=%d stay=%llu ms",
                         ev_name(em->ev.type), em->ev.cls_id,
                         (unsigned long long)em->ev.stay_ms);
                evlog.add(hb);
            }
            nEvent++;
        }
    });

    capT.join(); infT.join(); bizT.join(); outT.join();
    con.stop();
    strm.stop();          // 停推流（发 EOS，已连上的播放器正常收尾）
    stat.stop();          // 停状态通道
    rep.disconnect();
    auto t1 = chrono::steady_clock::now();
    double dt = chrono::duration<double>(t1 - t0).count();
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

    ifstream f(rgb_path, ios::binary);
    if (!f) { printf("[main] 打不开 %s\n", rgb_path); return -1; }
    vector<uint8_t> data((istreambuf_iterator<char>(f)),
                              istreambuf_iterator<char>());
    if (data.size() != size_t(w) * h * 3) {
        printf("[main] rgb 字节数不符: %zu != %dx%dx3\n", data.size(), w, h);
        return -1;
    }

    RknnEngine eng;
    if (!eng.init(model_path, make_params(use_v7), {0, 2})) return -1;

    FramePtr frame = make_shared<Frame>();
    frame->width = w; frame->height = h;
    frame->fmt = PixelFormat::RGB888;
    frame->data.swap(data);

    vector<DetObject> objs;
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
