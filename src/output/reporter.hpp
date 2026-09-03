#pragma once
// 输出层：画面叠加 / 告警截图 / socket 上报（D5/D10 实现）
#include <string>
#include <vector>

#include "business/roi_monitor.hpp"
#include "infer/rknn_engine.hpp"
#include "common/frame.hpp"

class Reporter {
public:
    void init(bool draw, bool save_shot, const std::string& shot_dir,
              bool enable_report, const std::string& server_ip, int port);

    // 每帧调用：叠加检测框/ROI/时间 并显示(D5)
    void onFrame(const FramePtr& frame, const std::vector<DetObject>& dets,
                 const RoiRect& roi);

    // 事件回调：告警截图 + 组 JSON 经 socket 上报(D5/D10)
    void onEvent(const Event& e, const FramePtr& frame);

    void connect();
    void disconnect();

private:
    // TODO(D5)：用 OpenCV(或自绘)画框/ROI/时间
    // TODO(D10)：事件 -> JSON {type,cls,ts,stay_ms,snapshot} 按行发到 server
    // TODO: 断线重连
};
