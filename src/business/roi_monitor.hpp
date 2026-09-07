#pragma once
// ROI 事件状态机 —— 本项目核心业务层（简历/面试重点）
// 状态迁移：IDLE -> INSIDE(计时) -> ALARMED -> RESOLVED -> IDLE
// 详见 docs/architecture.md 第 4 节
#include <cstdint>
#include <string>
#include <vector>

#include "infer/rknn_engine.hpp"

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
    std::string snapshot;       // 截图路径(输出线程填写)
};

class RoiMonitor {
public:
    void configure(const RoiRect& roi, int stay_alarm_sec,
                   const std::vector<int>& watch_cls);

    // 喂入一帧检测结果(带帧时刻)，产出事件列表
    std::vector<Event> feed(const std::vector<DetObject>& dets, uint64_t now_us);

private:
    enum class State { IDLE, INSIDE, ALARMED };

    State st_ = State::IDLE;
    RoiRect roi_;
    uint64_t stay_us_ = 0;         // 触发告警所需停留时长(us)
    uint64_t ts_start_us_ = 0;     // 进入时刻(us)
    std::vector<int> watch_cls_;   // 关注类别
};
