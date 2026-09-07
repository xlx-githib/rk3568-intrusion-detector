// RoiMonitor 离开去抖逻辑的 PC 端 host 单测（不依赖 RKNN/板子）。
// 编译(在仓库根目录, 需 g++ >= 11):
//   g++ -std=c++17 -I src -I tools/dev \
//       src/business/roi_monitor.cpp tools/dev/test_debounce.cpp -o tools/dev/test_debounce
//   ./tools/dev/test_debounce
#include <cstdio>
#include <vector>

#include "business/roi_monitor.hpp"

// ---- 帮助函数 ----
static DetObject person_inside() {   // 底部中心落在 ROI (100,100,200,200) 内
    DetObject o; o.cls_id = 0; o.conf = 0.9f; o.bx = 200.f; o.by = 200.f;
    return o;
}
static DetObject person_outside() {  // 底部中心在 ROI 外
    DetObject o; o.cls_id = 0; o.conf = 0.9f; o.bx = 200.f; o.by = 1000.f;
    return o;
}

static int n_type(const std::vector<Event>& evs, EventType t) {
    int n = 0;
    for (const auto& e : evs) if (e.type == t) ++n;
    return n;
}

static bool run_case(const char* name, int leave_confirm, int stay_sec,
                     const std::vector<int>& pattern,  // 0=缺席, 1=在ROI
                     int exp_intrude, int exp_alarm, int exp_leave, int exp_resolve) {
    RoiRect roi; roi.x = 100; roi.y = 100; roi.w = 200; roi.h = 200;
    RoiMonitor mon;
    mon.configure(roi, stay_sec, {0, 2}, leave_confirm);

    uint64_t now = 0;
    int intrude = 0, alarm = 0, leave = 0, resolve = 0;
    for (size_t i = 0; i < pattern.size(); ++i, now += 100000ULL) {  // 10fps
        std::vector<DetObject> dets;
        if (pattern[i]) dets.push_back(person_inside());
        else            dets.push_back(person_outside());
        auto evs = mon.feed(dets, now);
        intrude += n_type(evs, EventType::INTRUDE);
        alarm   += n_type(evs, EventType::ALARM);
        leave   += n_type(evs, EventType::LEAVE);
        resolve += n_type(evs, EventType::RESOLVE);
    }
    bool ok = (intrude == exp_intrude) && (alarm == exp_alarm) &&
              (leave == exp_leave) && (resolve == exp_resolve);
    printf("[%s] %s  INTRUDE=%d ALARM=%d LEAVE=%d RESOLVE=%d (期望 %d/%d/%d/%d)\n",
           ok ? "PASS" : "FAIL", name, intrude, alarm, leave, resolve,
           exp_intrude, exp_alarm, exp_leave, exp_resolve);
    return ok;
}

int main() {
    bool all = true;

    // 场景1【去抖核心】：进入→抖动缺席2帧→回来→停留告警→抖动缺席2帧→回来→真离开。
    // 期望：只有开头 1 次 INTRUDE、1 次 ALARM、末尾 1 次 RESOLVE，抖动不产生 LEAVE/RESOLVE/重复 INTRUDE。
    // 帧序: 0..1 在, 2..3 缺席, 4..30 在(帧30 满 3s 告警), 31..32 缺席, 33..40 在, 41..45 缺席(确认离开)
    {
        std::vector<int> pat(46, 1);
        pat[2] = pat[3] = 0;
        pat[31] = pat[32] = 0;
        for (int i = 41; i <= 45; ++i) pat[i] = 0;
        all &= run_case("进入-抖动-告警-抖动-真离开", 5, 3, pat, 1, 1, 0, 1);
    }

    // 场景2【未告警就抖动离开】：进入→缺席2帧→回来→…→最后真离开。
    // 期望：1 次 INTRUDE、0 ALARM、末尾 1 次 LEAVE；抖动期无重复事件。
    {
        std::vector<int> pat(30, 1);
        for (int i = 1; i <= 2; ++i) pat[i] = 0;      // 早段抖动
        for (int i = 15; i <= 16; ++i) pat[i] = 0;    // 中段抖动
        for (int i = 25; i <= 29; ++i) pat[i] = 0;    // 最后真离开(5帧确认)
        all &= run_case("进入-多次抖动-真离开", 5, 3, pat, 1, 0, 1, 0);
    }

    // 场景3【确认窗内不误告警】：进入后缺席窗口(阈值=50)内时间已过 3s，不应触发 ALARM 或 LEAVE。
    {
        std::vector<int> pat(40, 1);
        pat[0] = 1;
        for (int i = 1; i < 40; ++i) pat[i] = 0;      // 缺席但不足 50 帧
        all &= run_case("缺席窗内不误告警", 50, 3, pat, 1, 0, 0, 0);
    }

    // 场景4【兼容旧行为】：leave_confirm=1 时缺席 1 帧即离开（等同旧逻辑）。
    // 帧序 1/0/1/0/1/1 → 3 次 INTRUDE + 2 次 LEAVE（旧行为每次进出都发事件）。
    {
        std::vector<int> pat(6, 1);
        pat[1] = 0; pat[3] = 0;
        all &= run_case("兼容:确认=1帧即离开", 1, 3, pat, 3, 0, 2, 0);
    }

    printf(all ? "\n全部通过\n" : "\n存在失败\n");
    return all ? 0 : 1;
}
