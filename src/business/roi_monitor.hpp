#pragma once
// ROI 事件状态机 —— 本项目核心业务层（简历/面试重点）
// 状态迁移：IDLE -> INSIDE(计时) -> ALARMED -> RESOLVED -> IDLE
// 详见 docs/architecture.md 第 4 节
#include <cstdint>
#include <string>
#include <vector>

#include "infer/rknn_engine.hpp"

// 标准库名字逐个引入(头文件不用 using namespace std，避免污染包含者)
using std::string;
using std::vector;

struct RoiRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }
};

enum class EventType { INTRUDE, ALARM, LEAVE, RESOLVE };

struct Event {
    EventType type = EventType::INTRUDE;
    int    cls_id = -1;
    uint64_t ts_start_us = 0;   // 进入时刻(us)
    uint64_t stay_ms = 0;       // 停留时长
    string snapshot;       // 截图路径(输出线程填写)
};

class RoiMonitor {
public:
    // leave_confirm_frames：离开去抖阈值——连续 N 帧“检测不到目标”才算真正离开，
    // 期间目标短暂丢失(边缘抖动/被遮挡 1~2 帧)不误发 LEAVE/RESOLVE。默认 5(≈0.3s@16fps)。
    void configure(const RoiRect& roi, int stay_alarm_sec,
                   const vector<int>& watch_cls,
                   int leave_confirm_frames = 5);

    // 喂入一帧检测结果(带帧时刻)，产出事件列表
    vector<Event> feed(const vector<DetObject>& dets, uint64_t now_us);

private:
    enum class State { IDLE, INSIDE, ALARMED };

    State st_ = State::IDLE;
    RoiRect roi_;
    uint64_t stay_us_ = 0;         // 触发告警所需停留时长(us)
    uint64_t ts_start_us_ = 0;     // 进入时刻(us)
    int cur_cls_ = -1;             // 当前事件触发的类别(person=0/car=2)
    vector<int> watch_cls_;   // 关注类别
    int leave_confirm_ = 5;        // 离开去抖帧数阈值
    int leave_cnt_ = 0;            // 当前“连续缺席”帧数
};
