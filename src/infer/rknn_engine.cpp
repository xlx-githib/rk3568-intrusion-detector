// RKNN 推理封装实现（D4）
// 移植自官方 rknn_yolov5_demo（main.cc 的 RKNN 主流程 + yolo.cc 的后处理）
// 逻辑对应关系见 docs/study/03-量化与后处理.md；YOLOv5 与 YOLOv7 解码公式一致。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "infer/rknn_engine.hpp"

namespace {

// ---------- int8/uint8 反量化: f = (q - zp) * scale ----------
inline float deqnt(int8_t q, int32_t zp, float scale) { return (float(q) - zp) * scale; }
inline float deqnt(uint8_t q, int32_t zp, float scale) { return (float(q) - zp) * scale; }
inline float deqnt(float  q, int32_t zp, float scale) { return q; }

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// IoU（与官方 CalculateOverlap 等价）
float calc_iou(float x0, float y0, float x1, float y1,
               float x2, float y2, float x3, float y3) {
    float iw = std::max(0.f, std::min(x1, x3) - std::max(x0, x2));
    float ih = std::max(0.f, std::min(y1, y3) - std::max(y0, y2));
    float inter = iw * ih;
    float u = (x1 - x0) * (y1 - y0) + (x3 - x2) * (y3 - y2) - inter;
    return u <= 0.f ? 0.f : inter / u;
}

// 单个尺度输出上的解码（每格: 3 anchors × 85 = 255 个平面, 排布 [plane, gh, gw]）
template <typename T>
void decode_one_scale(const T* data, int grid_h, int grid_w, int stride,
                      const int* anchors, int num_cls,
                      const rknn_tensor_attr& attr, float conf_th,
                      std::vector<float>& boxes,      // x,y,w,h 平铺
                      std::vector<float>& scores,
                      std::vector<int>& clss) {
    const int grid_len = grid_h * grid_w;
    const int box_size = 5 + num_cls;               // 85
    const int32_t zp   = attr.zp;
    const float  scale = attr.scale;

    for (int a = 0; a < 3; ++a) {
        for (int i = 0; i < grid_h; ++i) {
            for (int j = 0; j < grid_w; ++j) {
                // 第 a 个 anchor 的"置信度平面"(4) 的 (i,j) 格子
                T obj = data[(box_size * a + 4) * grid_len + i * grid_w + j];
                float objf = deqnt(obj, zp, scale);
                if (objf < conf_th) continue;

                const T* ptr = data + (box_size * a) * grid_len + i * grid_w + j;
                // 找分数最高的类
                T best = ptr[5 * grid_len];
                int  best_id = 0;
                for (int k = 1; k < num_cls; ++k) {
                    T v = ptr[(5 + k) * grid_len];
                    if (deqnt(v, zp, scale) > deqnt(best, zp, scale)) { best = v; best_id = k; }
                }
                float cls_p = deqnt(best, zp, scale);
                float score = objf * cls_p;         // 联合分 = obj × 类别分
                if (score < conf_th) continue;

                // YOLOv5/v7 解码（与官方 yolo.cc 相同）
                float tx = deqnt(ptr[0 * grid_len], zp, scale);
                float ty = deqnt(ptr[1 * grid_len], zp, scale);
                float tw = deqnt(ptr[2 * grid_len], zp, scale);
                float th = deqnt(ptr[3 * grid_len], zp, scale);
                float cx = (tx * 2.f - 0.5f + j) * stride;
                float cy = (ty * 2.f - 0.5f + i) * stride;
                float bw = (tw * 2.f) * (tw * 2.f) * anchors[a * 2];
                float bh = (th * 2.f) * (th * 2.f) * anchors[a * 2 + 1];
                float x1 = cx - bw / 2.f, y1 = cy - bh / 2.f;

                boxes.push_back(x1); boxes.push_back(y1);
                boxes.push_back(bw);  boxes.push_back(bh);
                scores.push_back(score);
                clss.push_back(best_id);
            }
        }
    }
}

// 按类别 NMS：删除与高置信框重叠过多的低分框
void nms_per_class(std::vector<float>& boxes, std::vector<float>& scores,
                   std::vector<int>& clss, std::vector<int>& keep,
                   float nms_th) {
    std::vector<int> order(scores.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = int(i);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return scores[a] > scores[b]; });

    std::vector<char> removed(scores.size(), 0);
    for (size_t i = 0; i < order.size(); ++i) {
        int n = order[i];
        if (removed[n]) continue;
        keep.push_back(n);
        for (size_t j = i + 1; j < order.size(); ++j) {
            int m = order[j];
            if (removed[m]) continue;
            if (clss[n] != clss[m]) continue;      // per-class
            float x0 = boxes[n*4],   y0 = boxes[n*4+1];
            float x1 = boxes[n*4]+boxes[n*4+2], y1 = boxes[n*4+1]+boxes[n*4+3];
            float x2 = boxes[m*4],   y2 = boxes[m*4+1];
            float x3 = boxes[m*4]+boxes[m*4+2], y3 = boxes[m*4+1]+boxes[m*4+3];
            if (calc_iou(x0, y0, x1, y1, x2, y2, x3, y3) > nms_th) removed[m] = 1;
        }
    }
}

}  // namespace

// ================= 加载模型 + 查询输入输出属性 =================
bool RknnEngine::load_model_and_query(const char* model_path) {
    FILE* fp = fopen(model_path, "rb");
    if (!fp) { printf("[rknn] 打开模型失败: %s\n", model_path); return false; }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    model_buf_ = new unsigned char[size];
    fread(model_buf_, 1, size, fp);
    fclose(fp);

    int ret = rknn_init(&ctx_, model_buf_, size, 0, nullptr);
    if (ret < 0) { printf("[rknn] rknn_init fail ret=%d\n", ret); return false; }

    ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num_, sizeof(io_num_));
    if (ret < 0) { printf("[rknn] query IN_OUT_NUM fail ret=%d\n", ret); return false; }

    in_attr_.resize(io_num_.n_input);
    out_attr_.resize(io_num_.n_output);
    for (int i = 0; i < io_num_.n_input; ++i) {
        in_attr_[i].index = i;
        rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attr_[i], sizeof(rknn_tensor_attr));
    }
    for (int i = 0; i < io_num_.n_output; ++i) {
        out_attr_[i].index = i;
        rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &out_attr_[i], sizeof(rknn_tensor_attr));
    }

    // 解析输入尺寸（与官方一致: dims 顺序随 fmt）
    const rknn_tensor_attr& a = in_attr_[0];
    if (a.fmt == RKNN_TENSOR_NCHW) { in_w_ = a.dims[0]; in_h_ = a.dims[1]; in_ch_ = a.dims[2]; }
    else                          { in_h_ = a.dims[1]; in_w_ = a.dims[2]; in_ch_ = a.dims[3]; }
    printf("[rknn] model input %dx%dx%d, out=%d\n", in_w_, in_h_, in_ch_, io_num_.n_output);
    if (io_num_.n_output != 3) { printf("[rknn] 期望 3 个输出(YOLO 三尺度), 实际 %d\n", io_num_.n_output); return false; }
    return true;
}

bool RknnEngine::init(const std::string& model_path, const YoloParams& p,
                      const std::vector<int>& watch_cls) {
    yp_ = p;
    watch_cls_ = watch_cls;
    if (!load_model_and_query(model_path.c_str())) return false;
    ok_ = true;
    return true;
}

void RknnEngine::release() {
    if (ctx_) { rknn_destroy(ctx_); ctx_ = 0; }
    if (model_buf_) { delete[] model_buf_; model_buf_ = nullptr; }
    ok_ = false;
}

// ================= 一帧检测 =================
bool RknnEngine::infer(const FramePtr& in, std::vector<DetObject>& outs) {
    outs.clear();
    if (!ok_ || !in || in->data.empty()) return false;

    // ---- 1) 转成 RGB(如输入是 BGR)，并 letterbox 到模型输入尺寸(无 OpenCV, 最近邻缩放) ----
    const uint32_t sw = in->width, sh = in->height;
    std::vector<uint8_t> src;                       // RGB, HWC
    if (in->fmt == PixelFormat::RGB888) {
        src.assign(in->data.begin(), in->data.end());
    } else if (in->fmt == PixelFormat::BGR888) {
        src.resize(in->data.size());
        for (size_t i = 0; i + 2 < in->data.size(); i += 3) {  // BGR->RGB
            src[i] = in->data[i+2]; src[i+1] = in->data[i+1]; src[i+2] = in->data[i];
        }
    } else {
        printf("[rknn] 暂只支持 RGB/BGR 输入\n"); return false;
    }

    const int dst = in_w_;                           // 640
    float scale = std::min(float(dst) / sw, float(dst) / sh);
    int nw = int(sw * scale), nh = int(sh * scale);
    int padx = (dst - nw) / 2, pady = (dst - nh) / 2;

    std::vector<uint8_t> input(dst * dst * 3, 114);  // 灰边 114
    for (int y = 0; y < nh; ++y) {                   // 最近邻缩放(演示够用, 后续可换 RGA)
        int sy = int(y / scale);
        for (int x = 0; x < nw; ++x) {
            int sx = int(x / scale);
            const uint8_t* p = &src[(size_t(sy) * sw + sx) * 3];
            uint8_t* d = &input[((size_t(pady + y) * dst + (padx + x))) * 3];
            d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
        }
    }

    // ---- 2) RKNN 推理（六连的 set/run/get）----
    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt  = RKNN_TENSOR_NHWC;
    inputs[0].size = size_t(in_w_) * in_h_ * in_ch_;
    inputs[0].buf  = input.data();
    if (rknn_inputs_set(ctx_, 1, inputs) < 0) { printf("[rknn] inputs_set fail\n"); return false; }
    if (rknn_run(ctx_, nullptr) < 0) { printf("[rknn] run fail\n"); return false; }

    rknn_output outputs[3];
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < 3; ++i) {
        bool quant = (out_attr_[i].type == RKNN_TENSOR_INT8 ||
                      out_attr_[i].type == RKNN_TENSOR_UINT8);
        outputs[i].want_float = quant ? 0 : 1;      // 量化模型拿原始 int8
    }
    if (rknn_outputs_get(ctx_, 3, outputs, nullptr) < 0) { printf("[rknn] outputs_get fail\n"); return false; }

    // ---- 3) 三尺度解码 -> NMS -> letterbox 逆映射 -> 白名单 ----
    std::vector<float> boxes, scores;
    std::vector<int>   clss;
    for (int s = 0; s < 3; ++s) {
        int gh = in_h_ / yp_.strides[s];
        int gw = in_w_ / yp_.strides[s];
        const int* anchors = yp_.anchors[s];
        if (out_attr_[s].type == RKNN_TENSOR_INT8)
            decode_one_scale((const int8_t*) outputs[s].buf, gh, gw, yp_.strides[s],
                             anchors, yp_.num_cls, out_attr_[s], yp_.conf_thresh, boxes, scores, clss);
        else if (out_attr_[s].type == RKNN_TENSOR_UINT8)
            decode_one_scale((const uint8_t*) outputs[s].buf, gh, gw, yp_.strides[s],
                             anchors, yp_.num_cls, out_attr_[s], yp_.conf_thresh, boxes, scores, clss);
        else
            decode_one_scale((const float*) outputs[s].buf, gh, gw, yp_.strides[s],
                             anchors, yp_.num_cls, out_attr_[s], yp_.conf_thresh, boxes, scores, clss);
    }
    rknn_outputs_release(ctx_, 3, outputs);

    std::vector<int> keep;
    nms_per_class(boxes, scores, clss, keep, yp_.nms_thresh);

    for (int idx : keep) {
        // 白名单过滤（可配置，如 {0,2}=person,car）
        if (std::find(watch_cls_.begin(), watch_cls_.end(), clss[idx]) == watch_cls_.end())
            continue;
        // 640 坐标 -> 原图坐标
        float x1 = (boxes[idx*4]   - padx) / scale;
        float y1 = (boxes[idx*4+1] - pady) / scale;
        float x2 = (boxes[idx*4] + boxes[idx*4+2] - padx) / scale;
        float y2 = (boxes[idx*4+1] + boxes[idx*4+3] - pady) / scale;
        x1 = clampf(x1, 0, float(sw)); y1 = clampf(y1, 0, float(sh));
        x2 = clampf(x2, 0, float(sw)); y2 = clampf(y2, 0, float(sh));

        DetObject o;
        o.cls_id = clss[idx];
        o.conf   = scores[idx];
        o.x1 = x1; o.y1 = y1; o.x2 = x2; o.y2 = y2;
        o.cx = (x1 + x2) * 0.5f; o.cy = (y1 + y2) * 0.5f;
        o.bx = o.cx; o.by = y2;               // 底部中心（ROI 判定用）
        outs.push_back(o);
    }
    return true;
}
