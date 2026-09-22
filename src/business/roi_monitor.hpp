#pragma once
// ROI 事件状态机 —— 本项目核心业务层（简历/面试重点）
// 状态迁移：IDLE -> INSIDE(计时) -> ALARMED -> RESOLVED -> IDLE
// 详见 docs/architecture.md 第 4 节
#include <cstdint>
#include <string>
#include <vector>

#include "infer/rknn_engine.hpp"

using std::string;
using std::vector;

/**
  * @brief  ROI矩形区域结构体
  * @note   用于描述图像感兴趣区域，包含坐标宽高，提供点包含判断接口
  */
typedef struct
{
    int x = 0;      /*!< 矩形左上角X坐标 */
    int y = 0;      /*!< 矩形左上角Y坐标 */
    int w = 0;      /*!< 矩形区域宽度 */
    int h = 0;      /*!< 矩形区域高度 */

    /**
      * @brief  判断坐标点是否落在当前ROI矩形内部
      * @param  px: 待判断点x坐标
      * @param  py: 待判断点y坐标
      * @retval bool: true 点在矩形内; false 点在矩形外
      */
    bool contains(float px, float py) const
    {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }

} RoiRect;

/**
  * @brief  事件类型枚举
  * @note   用于描述不同类型的事件
  * @param  INTRUDE: 入侵事件
  * @param  ALARM: 告警事件
  * @param  LEAVE: 离开事件
  * @param  RESOLVE: 解决事件
  * @related DetObject: 相关的检测对象结构体
  */
enum class EventType { INTRUDE, ALARM, LEAVE, RESOLVE };


/**
  * @brief  事件结构体
  * @note   用于描述触发的事件信息
  */
struct Event {
    EventType type = EventType::INTRUDE;//事件默认闯入状态
    int    cls_id = -1;
    uint64_t ts_start_us = 0;   // 进入时刻(us)
    uint64_t stay_ms = 0;       // 停留时长
    string snapshot;       // 截图路径(输出线程填写)
};

/**
  * @brief  ROI监控器类
  * @note   用于监控指定ROI区域内的事件
  */
class RoiMonitor {
public:
    // leave_confirm_frames：离开去抖阈值——连续 N 帧“检测不到目标”才算真正离开，
    // 期间目标短暂丢失(边缘抖动/被遮挡 1~2 帧)不误发 LEAVE/RESOLVE。默认 5(≈0.3s@16fps)。
    void configure(const RoiRect& roi, int stay_alarm_sec,
                   const vector<int>& watch_cls,
                   int leave_confirm_frames = 5);

    // 喂入一帧检测结果(带帧时刻)，产出事件列表
    vector<Event> feed(const vector<DetObject>& dets, uint64_t now_us);

    // ---------- 运行时更新（M3 控制台热更新用，不重置当前状态）----------

    /** @brief 更新警戒区矩形 */
    void setRoi(const RoiRect& r) { roi_ = r; }

    /** @brief 更新停留告警秒数 */
    void setStaySec(int s) { stay_us_ = uint64_t(s > 0 ? s : 1) * 1000000ULL; }

    /** @brief 更新离开去抖帧数(连续缺席多少帧才算离开) */
    void setLeaveConfirm(int n) { leave_confirm_ = n > 0 ? n : 1; }

private:
    /**
      * @brief  状态枚举
      * @note   用于描述监控器的当前状态
      */
    enum class State { IDLE, INSIDE, ALARMED };

    State st_ = State::IDLE;       //空闲
    RoiRect roi_;
    uint64_t stay_us_ = 0;         // 触发告警所需停留时长(us)
    uint64_t ts_start_us_ = 0;     // 进入时刻(us)
    int cur_cls_ = -1;             // 当前事件触发的类别(person=0/car=2)
    vector<int> watch_cls_;   // 关注类别
    int leave_confirm_ = 5;        // 离开去抖帧数阈值
    int leave_cnt_ = 0;            // 当前“连续缺席”帧数
};
