# models 目录说明

把目标检测模型放在这里。仓库默认通过 `.gitignore` 忽略 `*.rknn`（体积较大），
请在你自己的板端/PC 本地放模型；如需入库可用 Git LFS。

## 需要的模型
- `yolov5s.rknn`：COCO 80 类目标检测模型（需覆盖 person、car 等类别）。
  来源可选：
  1. 官方例程 `rknn_yolov5_demo.zip` 自带的 rknn 模型；
  2. `rknn_model_zoo`（你资料包里的 `rknn_model_zoo-2.0.0.zip`）中 yolov5 例程；
  3. 自己用 rknn-toolkit2 把 onnx 转 rknn（加分项，D15 做）。
- `class_names.txt`：类别名，一行一个（与 yolov5 的 names 对齐）。
  例程或 modelzoo 里通常有，COCO names 顺序固定。

## 注意
- 模型输入尺寸要与 `config/intruder.conf` 的 `model_input_size` 一致。
- 用板上能跑的、官方验证过的 rknn 版本最省事，先用它能跑通，再谈自己转。
