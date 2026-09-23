#pragma once
// 本机状态通道（默认 127.0.0.1:9100）—— 给板端 Qt 界面用
//
// 与 reporter.hpp 的区别（方向相反、职责不同）：
//   Reporter ：**我们主动连** PC 上位机（TCP 客户端），上报事件 + base64 缩略图
//   StatLink ：**我们当服务端**，等板端 Qt 连过来，周期推"状态快照"（ROI/检测框/统计）
//
// 为什么单开一条通道：9000 那条是"我们连 PC"，方向反了；而且状态是高频小数据
// （100ms 一次、纯文本），不该和事件上报混在一起。
//
// 后续（Step 3b 触摸调 ROI）会在这里加**反向命令**：Qt 发 "roi 100 200 300 400"，
// 交给运行时控制台执行 —— 与 console 的命令格式完全一致，复用同一套解析。
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "business/roi_monitor.hpp"   // DetObject

// 标准库名字逐个引入(头文件不用 using namespace std，避免污染包含者)
using std::atomic;
using std::deque;
using std::mutex;
using std::string;
using std::thread;
using std::vector;

// 一帧状态快照（值语义：publish 时整体拷贝，避免跨线程共享可变对象）
struct StateSnapshot {
    // 坐标系：下面的 ROI 与检测框都用这个尺寸，Qt 端据此做"到视频区"的映射
    int frame_w = 0, frame_h = 0;
    // 当前参数（来自 RtParams，即热更新后的真实值）
    int roi_x = 0, roi_y = 0, roi_w = 0, roi_h = 0;
    int stay_sec = 3, leave_confirm = 5;
    int conf_pct = 25, nms_pct = 45;
    int log_level = 1;
    bool report_on = true;
    // 统计
    double fps = 0;
    int infer = 0, events = 0, alarms = 0;
    unsigned long long pushed = 0, dropped = 0;   // 推流：成功/丢帧
    // 最近一帧的检测框（取前若干条，避免单行过长）
    vector<DetObject> dets;
    // 最近一条事件
    string last_event;                            // 空串=还没有事件
    unsigned long long last_stay_ms = 0;
};

class StatLink {
public:
    StatLink() = default;
    ~StatLink();
    StatLink(const StatLink&) = delete;
    StatLink& operator=(const StatLink&) = delete;

    // 启动监听；port<=0 表示关闭。失败只打印，不影响主流程
    bool start(int port = 9100);
    void stop();

    // 发布状态快照（线程安全）。内部发送线程按 ~100ms 节流发出，不会拖慢调用方
    void publish(const StateSnapshot& s);
    bool connected() const { return fd_.load() >= 0; }

    // 取出最近收到的一条控制命令（一行文本，如 "roi 100 200 300 400"）；无则返回 false
    bool takeCommand(string& out);

private:
    void loop();
    static string buildJson(const StateSnapshot& s);

    atomic<int> fd_{-1};          // 已连接的客户端
    int srv_ = -1;
    int port_ = 0;
    atomic<bool> running_{false};
    thread th_;

    mutex m_;                     // 保护 snap_ / cmds_
    StateSnapshot snap_;
    deque<string> cmds_;
    string recv_line_;            // 收命令的半行缓冲（只在 loop 线程里用）
};
