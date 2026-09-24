#pragma once
// 运行时控制台（M3）：stdin 命令交互 + 参数热更新，免去"改参数→重编译"
//   命令: help / status / roi x y w h / stay s / leave n / conf f / nms f /
//         log n / report on|off / events [n] / save / reload / quit
// 设计: 控制台线程只改 RtParams（原子变量），业务/推理线程检测 version 变化后应用，
//       无锁竞争、无阻塞，符合"运行时配置"的企业级做法。
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

using std::atomic;
using std::deque;
using std::function;
using std::mutex;
using std::string;
using std::thread;

// 运行时参数(控制台线程写，工作线程读)
struct RtParams {
    atomic<int>  roi_x{0}, roi_y{0}, roi_w{0}, roi_h{0};
    atomic<int>  frame_w{0}, frame_h{0};   // 当前画面尺寸(校验/展示用)
    atomic<int>  stay_sec{3};              // 停留多久触发告警(秒)
    atomic<int>  leave_confirm{5};         // 离开去抖：连续缺席多少帧才确认离开
    atomic<int>  conf_pct{25};             // 置信度阈值 ×100 (25 = 0.25)
    atomic<int>  nms_pct{45};              // NMS IoU 阈值 ×100
    atomic<int>  log_level{1};             // 0=DEBUG 1=INFO 2=WARN 3=ERROR
    atomic<bool> report_on{true};          // 是否 socket 上报
    atomic<uint32_t> version{0};           // 任一参数变更即自增，消费方据此重新应用
    void bump() { version.fetch_add(1); }
};

// 最近事件环形记录(供 events 命令回看)
class EventLog {
public:
    void add(const string& line);
    string dump(int n) const;
private:
    mutable mutex m_;
    deque<string> q_;
    enum { kMax = 200 };
};

class Console {
public:
    using StatusFn = function<string()>;       // status 命令回调(由 main 提供)
    using EventsFn = function<string(int)>;    // events N 命令回调

    // 启动后台控制台线程；*quit 置 true 时通知主程序退出
    void start(RtParams* p, const string& conf_path,
               StatusFn status_fn, EventsFn events_fn, atomic<bool>* quit);
    void stop();

    // 外部线程投递一条命令（如板端 Qt 触摸调 ROI），线程安全。
    // 与终端手敲的命令格式完全一致，复用同一套解析与热更新链路。
    void submit(const string& line);

private:
    void loop();
    void onLine(const string& line);
    void doSave();
    void doReload();
    bool applyKv(const string& k, const string& v, bool report);
    string kvOf(const string& k) const;

    RtParams*     p_ = nullptr;
    string        conf_path_;
    StatusFn      status_fn_;
    EventsFn      events_fn_;
    atomic<bool>* quit_ = nullptr;
    atomic<bool>  running_{false};
    thread        th_;

    mutex         ext_m_;      // 保护外部投递队列
    deque<string> ext_q_;
};
