#pragma once
// RKNN 推理封装 + YOLO(v5/v7) 后处理 + 白名单（D4：移植自官方 rknn_yolov5_demo）
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "rknn_api.h"
#include "common/frame.hpp"

using std::atomic;
using std::string;
using std::vector;

// 单目标检测结果（一帧内多个，像素坐标相对原图）
/**
  * @brief  检测对象结构体
  * @note   用于描述检测到的目标信息
  */
struct DetObject {
    int   cls_id = -1;
    float conf   = 0.f;
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    float cx = 0, cy = 0;                  // 框中心
    float bx = 0, by = 0;                  // 框底部中心（ROI 判定用）
};

// YOLOv5/v7 通用后处理参数（解码公式一致，仅 anchor 不同）
struct YoloParams {
    int input_size = 640;                  // 模型输入边长（方图）
    int num_cls    = 80;                   // COCO 80
    int strides[3] = {8, 16, 32};
    int anchors[3][6] = {                  // 默认 yolov5；yolov7 传入对应 18 个值
        {10, 13, 16, 30, 33, 23},
        {30, 61, 62, 45, 59, 119},
        {116, 90, 156, 198, 373, 326}};
    float conf_thresh = 0.25f;             // 可与 config/intruder.conf 对齐
    float nms_thresh  = 0.45f;
};


/**
  * @brief  RKNN引擎类
  * @note   用于封装RKNN推理逻辑
  * infer() 输入一帧原图(RGB888/BGR888, HWC)，输出白名单过滤后的 DetObject 列表
  * init() 传入白名单 watch_cls，如 {0,2}=person,
  * release() 显式释放 NPU(析构函数也会调，且 release 内部置空、可重复调用)：目的是确定释放时机
  */
class RknnEngine {
public:
    RknnEngine() = default;
    ~RknnEngine() { release(); }

    bool init(const string& model_path, const YoloParams& p,
              const vector<int>& watch_cls);   // watch_cls: 白名单(如 {0,2}=person,car)
    void release();

    // 输入一帧原图(RGB888/BGR888, HWC)，输出白名单过滤后的 DetObject 列表
    bool infer(const FramePtr& in, vector<DetObject>& outs);

    /**
      * @brief  运行时更新置信度/NMS 阈值(M3 控制台热更新用)
      * @param  conf 置信度阈值(0~1)
      * @param  nms  NMS IoU 阈值(0~1)
      */
    void setThresh(float conf, float nms);

private:
    bool load_model_and_query(const char* model_path);// 加载模型 + query 输入输出信息

private:
    rknn_context ctx_ = 0;//RKNN上下文类型

    // 模型输入信息（query 得到）
    int in_w_ = 0, in_h_ = 0, in_ch_ = 3;
    rknn_input_output_num io_num_{};
    vector<rknn_tensor_attr> in_attr_, out_attr_;

    // YOLO 后处理参数与白名单
    YoloParams yp_;
    vector<int> watch_cls_;
    atomic<float> live_conf_{0.25f};   // 运行时置信度阈值(控制台可改)
    atomic<float> live_nms_{0.45f};    // 运行时 NMS 阈值
    unsigned char* model_buf_ = nullptr;
    bool ok_ = false;
};

