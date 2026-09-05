# RK3568 智能安防区域入侵检测与告警系统

> 基于瑞芯微 RK3568 + RKNN NPU 的端侧实时目标检测与告警系统（C++ 工程化）。
> 摄像头实时检测画面中的人/车，一旦进入或长时间停留在警戒区域(ROI)，即实时标注、截图存档并通过 socket 上报到 PC 端。

## 为什么做这个项目
- 目标岗位：嵌入式 Linux 应用 / AI 音视频方向
- 技术栈：V4L2 采集 + RKNN C API + 多线程流水线 + 业务事件状态机 + Socket 上报
- 学习方式：先组装官方例程跑通全闭环，再自上而下读代码，最后用自己的代码重写核心模块（业务层 + 线程骨架）

## 功能特性
- [x] 视频输入：MIPI CSI(OV13850) / 视频文件，可配置切换
- [ ] NPU 目标检测：YOLOv5s(COCO，关注 person/car 等白名单)
- [ ] 警戒区域 ROI：配置文件定义矩形区域并叠加显示
- [ ] 入侵判定：目标检测框底部中心进入 ROI → 标记入侵
- [ ] 滞留告警：在 ROI 内停留超 N 秒 → 触发告警
- [ ] 离开/解除：离开 ROI → 事件结束并记录持续时长
- [ ] 输出：画面叠加 + 告警截图存盘 + 本地事件日志
- [ ] 网络上报：事件以 JSON 经 TCP/socket 上报 PC 上位机
- [ ] 配置化 + 一键启动脚本

## 目录结构
```
rk3568-intrusion-detector/
├─ CMakeLists.txt            # 工程构建
├─ build.sh / run.sh         # 一键构建 / 运行（板端）
├─ config/
│   └─ intruder.conf         # 所有可调参数
├─ models/                   # 存放 yolov5s.rknn（见 models/README.md）
├─ src/
│   ├─ main.cpp              # 程序入口 / 流水线组装
│   ├─ common/               # 帧结构/帧队列、日志、配置解析
│   ├─ capture/              # V4L2 采集（MIPI/文件）
│   ├─ infer/                # RKNN 推理封装
│   ├─ postproc/             # YOLO 解码 + NMS
│   ├─ business/             # ROI 判定 + 事件状态机（核心）
│   └─ output/               # 画框 / 截图 / socket 上报
├─ host/                     # PC 端接收程序（上位机）
├─ docs/
│   ├─ architecture.md       # 架构设计：调用链/线程模型/状态机
│   └─ devlog/               # 每日开发日志
└─ tools/
    └─ bench.sh              # 帧率/CPU 实测
```

## 架构图
见 [docs/architecture.md](docs/architecture.md)（含线程模型与事件状态机）。

## 编译与运行（板端，逐步完善）
```bash
# 1. 交叉编译 / 板上原生编译
./build.sh

# 2. 运行（默认读 config/intruder.conf）
./run.sh

# 3. PC 端启动上位机接收
python3 host/receiver.py
```

## 配置
所有参数集中在 `config/intruder.conf`（ROI、置信度、滞留秒数、设备节点、上报地址等）。

## 开发日志
> 每天一档，如实记录完成内容、踩坑与解决过程。

| 日期 | 进展 | 链接 |
|---|---|---|
| 2026-09-03 | 仓库初始化 + 骨架搭建 | [devlog](docs/devlog/2026-09-03.md) |

## 里程碑
- [ ] M1 (~09/07)：视频文件全闭环雏形（检测→ROI 判定→告警→截图）
- [ ] M2 (~09/14)：真 MIPI 摄像头 + 多线程工程化 + socket 上报联调
- [ ] M3 (~09/21)：功能冻结 + README + 演示视频 + Release v0.1

## 明确不做（范围护栏）
- 不训练/微调模型（使用现成 rknn 权重）
- 不做复杂多目标跟踪（第一版用 ROI 内持续检出简化）
- 不做 Web/云大后端（PC 端仅轻量接收）
- 不改内核 / 驱动 / 设备树
