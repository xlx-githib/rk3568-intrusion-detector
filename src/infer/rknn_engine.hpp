#pragma once
// RKNN 推理封装 + 检测结果结构（D2/D3 起移植官方 C demo）
#include <string>
#include <vector>

#include "common/frame.hpp"

// 单目标检测结果（一帧内多个）
struct DetObject {
    int   cls_id = -1;
    float conf   = 0.f;
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;  // 像素坐标(相对原图)
    float cx = 0, cy = 0;                  // 框中心
    float bx = 0, by = 0;                  // 框底部中心（ROI 判定用）
};

class RknnEngine {
public:
    bool init(const std::string& model_path, int input_size,
              const std::vector<int>& watch_cls);  // watch_cls: 白名单类别 id
    void release();
    // 输入一帧图，输出本帧检测结果（已做白名单过滤）
    bool infer(const FramePtr& in, std::vector<DetObject>& outs);

private:
    // TODO(D2/D3)：移植官方 C demo 的四处调用：
    //   rknn_init / rknn_query(IO) / rknn_inputs_set / rknn_run / rknn_outputs_get
    // TODO: YOLO 三尺度解码 + NMS（官方 rknn_model_zoo 有现成实现可参考）
};
