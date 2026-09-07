// ROI 事件状态机实现（M1 业务层雏形，逻辑见 docs/architecture.md 第 4 节）
// 简化约定：用"ROI 内是否存在白名单检出(框底部中心在 ROI 内)"做帧级判定；
//           连续停留计时近似停留时长；离开/中断则结束并回 IDLE。
#include <algorithm>

#include "business/roi_monitor.hpp"

void RoiMonitor::configure(const RoiRect& roi, int stay_alarm_sec,
                           const std::vector<int>& watch_cls) {
    roi_        = roi;
    stay_us_    = uint64_t(stay_alarm_sec) * 1000000ULL;
    watch_cls_  = watch_cls;
    st_         = State::IDLE;
    ts_start_us_ = 0;
    cur_cls_    = -1;
}

std::vector<Event> RoiMonitor::feed(const std::vector<DetObject>& dets,
                                    uint64_t now_us) {
    std::vector<Event> evs;

    // 帧级判定：是否有白名单目标，且其"框底部中心(bx,by)"落在 ROI 内；
    // 记录触发目标的类别(cls)，供事件带上 person/car
    bool inside = false;
    int  trig_cls = -1;
    for (const auto& o : dets) {
        if (std::find(watch_cls_.begin(), watch_cls_.end(), o.cls_id) == watch_cls_.end())
            continue;
        if (roi_.contains(o.bx, o.by)) { inside = true; trig_cls = o.cls_id; break; }
    }

    if (inside) {
        switch (st_) {
        case State::IDLE: {                       // 首次进入
            st_ = State::INSIDE;
            ts_start_us_ = now_us;
            cur_cls_ = trig_cls;
            Event e; e.type = EventType::INTRUDE; e.cls_id = trig_cls;
            e.ts_start_us = now_us; e.stay_ms = 0;
            evs.push_back(e);
            break;
        }
        case State::INSIDE: {                     // 已进入，检查是否达到告警时长
            if (now_us - ts_start_us_ >= stay_us_) {
                st_ = State::ALARMED;
                Event e; e.type = EventType::ALARM; e.cls_id = cur_cls_;
                e.ts_start_us = ts_start_us_;
                e.stay_ms = (now_us - ts_start_us_) / 1000ULL;
                evs.push_back(e);
            }
            break;
        }
        case State::ALARMED:                      // 已告警，持续停留不重复告警
            break;
        }
    } else {
        switch (st_) {
        case State::INSIDE: {                     // 未超时就离开 → 记录离开
            Event e; e.type = EventType::LEAVE; e.cls_id = cur_cls_;
            e.ts_start_us = ts_start_us_;
            e.stay_ms = (now_us - ts_start_us_) / 1000ULL;
            evs.push_back(e);
            st_ = State::IDLE;
            break;
        }
        case State::ALARMED: {                    // 告警后离开 → 解除告警
            Event e; e.type = EventType::RESOLVE; e.cls_id = cur_cls_;
            e.ts_start_us = ts_start_us_;
            e.stay_ms = (now_us - ts_start_us_) / 1000ULL;
            evs.push_back(e);
            st_ = State::IDLE;
            break;
        }
        default:
            break;                                // IDLE：本来就没目标，无事件
        }
    }
    return evs;
}
