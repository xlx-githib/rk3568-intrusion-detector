# RKNN YOLOv5 官方 demo 导读（study/01）

> 配套阅读：`rknn_yolov5_demo\src\main_annotated.cc`（逐行中文注释版）
> 原始路径：`d:\正点原子 Linux 资料\01、程序源码\01、程序源码\01、AI例程\01、源码\06_yolov5\rknn_yolov5_demo\`
> 学习目标：把 RKNN 主流程看懂到"能讲 + 能移植到自己的工程"，不被 OpenCV/RGA/结构体噪音卡住。

---

## 0. 一句话记住管道

```
抓帧(1280x720 BGR)
  → cvtColor(RGB) → RGA 缩放成 640x640 (letterbox)
  → rknn_inputs_set 喂图 → rknn_run 推理 → rknn_outputs_get 取3路输出
  → post_process(反量化+解码+NMS) → 得到一批框 → 画框/显示
```

RKNN 使用口诀：**`init → query → (set → run → get → release) 循环 → destroy`**

---

## 1. 纯净主流程（本质只有这些）

```cpp
rknn_context ctx;

// ① 加载 .rknn 进内存 → 建 NPU 会话
unsigned char* model = read_file("yolov5s.rknn", &size);
rknn_init(&ctx, model, size, 0, NULL);

// ② 查模型长啥样（输入/输出个数、维度、量化参数）
rknn_input_output_num io_num;
rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, ...);
// → n_input=1, n_output=3

// ③ 每帧重复
while (抓到一帧缩好的 640x640 RGB 图 img_640) {
    rknn_input in = {};                // 结构体 = "填参数表"
    in.index = 0;  in.type = RKNN_TENSOR_UINT8;
    in.fmt  = RKNN_TENSOR_NHWC;  in.size = 640*640*3;
    in.buf  = img_640;
    rknn_inputs_set(ctx, 1, &in);      // 喂输入

    rknn_run(ctx, NULL);               // 跑一次

    rknn_output out[3];
    out[i].want_float = 0;             // 0=int8(快)  1=float(runtime反量化)
    rknn_outputs_get(ctx, 3, out, NULL);   // 取3路输出

    boxes = post_process(out, scale, zp);  // 反量化→解码→NMS(在 yolo.cc)

    rknn_outputs_release(ctx, 3, out);     // 释放本帧
}
rknn_destroy(ctx);                     // 结束
```

> 其余所有代码都是为了解决两件事：a) `img_640` 从哪来（摄像头+缩放）
> b) 框怎么显示（画框+窗口）。这两件跟"NPU 推理本身"无关。

---

## 2. 结构体速查表（看到不认识就回来查）

### 2.1 RKNN API 自带（`rknn_api.h`）

| 结构体 | 作用 | 只记这些字段 |
|---|---|---|
| `rknn_input` | 告诉 NPU"我喂什么" | `index`(第几个输入)、`type`(UINT8)、`fmt`(NHWC)、`size`(字节数)、`buf`(**数据指针**) |
| `rknn_output` | 告诉 NPU"我拿什么" | `want_float`(0=int8 / 1=float)、`buf`(结果指针) |
| `rknn_tensor_attr` | 张量"身份证"(查询结果) | `dims[]`(维度)、`fmt`、`type`、`zp`、`scale`(反量化) |
| `rknn_context` | NPU 会话句柄 | 当成"钥匙"，每步调用都带上它 |
| `rknn_input_output_num` | 输入输出个数 | `n_input`、`n_output` |

### 2.2 demo 自定义（`yolo.h`）

| 结构体 | 作用 | 记法 |
|---|---|---|
| `MODEL_INFO` | 命令行参数+模型信息打包 | 心里当"一堆全局配置"，不用背 |
| `LETTER_BOX` | 原图→640 缩放比例/补边记录 | 只为把框坐标**映射回原图** |
| `detect_result_t` | **单个检测框** | `name`(类别)、`box`(坐标)、`prop`(置信度) |
| `detect_result_group_t` | **一帧所有框** | `count`(几个框)、`results[]`(每框) |

> 记忆：`detect_result_t` = 一个框；加 `_group_t` = 一帧所有框。
> 后处理完程序拿到的就是 `group`（count + results[]）。

---

## 3. 读 main.cc / main_annotated.cc 的"跳过清单"

| 代码段 | 处理 |
|---|---|
| opencv / rga / drm 头文件 include | **跳过**（本项目用 V4L2，不用这些） |
| `drm_init`、RGA 的 src/dst/rect 初始化 | **跳过**（显示 + 硬件缩放细节） |
| `printRKNNTensor` / `load_model` | 扫一眼标题即可 |
| `query_model_info` 内部 | **读**（查模型属性，用 dims/zp/scale） |
| 抓帧 / `rotate` / `cvtColor` / `imresize` | 知道"在准备 640×640 RGB"即可，别细抠 |
| 主循环 3 个 `rknn_` 调用 + `post_process` | **精读**（配第一节纯净版） |
| 画框 / `imshow` / 所有 `free()` 清理 | 跳过 |

---

## 4. 与自有工程 `src/main.cpp` 的差异（移植时注意）

| demo(main.cc) | 本工程(要写的) | 说明 |
|---|---|---|
| `cv::VideoCapture(0)` | V4L2 `v4l2_camera.hpp` | MIPI OV13850 用 OpenCV 打不开 → 自己写 V4L2 |
| OpenCV 画框 + `imshow` | 截图存盘 + socket 上报 | 板子可能"无屏"运行 |
| 单线程 while 主循环 | 四线程流水线 | 采集/推理/业务/输出 解耦（见 architecture.md） |
| 打印全部 80 类 | 只留 person/car 白名单 + ROI 判定 | 在 yolo.cc 后处理加过滤 |
| 每帧都从摄像头读 | 输入源可配 video/mipi | `config/intruder.conf` 已留 `input_type` |

---

## 5. 待补充（读到哪、学到哪就加进来）

- [ ] `yolo.cc` 导读：反量化 `deqnt_affine_to_f32`、三尺度解码 `process_i8/process_fp`、NMS、坐标回映射
- [ ] RKNN 输入/输出 零拷贝版本 (`rknn_create_mem`) —— 高性能工程化可选
- [ ] letterbox 手工推导 + 画框坐标映射公式
- [ ] 面试口径：为什么 NPU 快 / 什么是 int8 量化 / q8 与 fp 区别
