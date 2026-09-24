# RK3568 智能安防区域入侵检测与告警系统

> 基于瑞芯微 RK3568 + RKNN NPU 的**端侧**实时目标检测与告警系统（C++17 工程化，**全程不用 OpenCV**）。
> 摄像头实时检测人/车，目标进入或长时间停留在警戒区域(ROI)即告警：本地截图存档 +
> socket 上报 PC 上位机 + **H.264 硬编码推流** + **板端触屏界面**（拖拽调 ROI / 在线调参）。

## 系统能力一览

```
                  ┌──────────────── 板子 (RK3568) ────────────────┐
 OV13850 ─V4L2─> NV12 ─┬─ RGB888 ─> RKNN(NPU) ─> 解码/NMS ─> ROI 状态机
                       │                                        │
                       │                        ┌───────────────┼──────────────────┐
                       │                        ↓               ↓                  ↓
                       │                   截图存盘      JSON+缩略图            状态快照
                       │                                  TCP:9000            TCP:9100
                       │                                      ↓                  ↓
                       │                              PC Qt 上位机        板端 Qt 界面
                       │                                                      ↕ 触屏操作
                       └─ NV12 ─> mpph264enc ─> MPEG-TS ─TCP:5000─> ┌─ 板端 Qt(mppvideodec 硬解+叠加)
                                                                     └─ PC ffplay(延迟 <1s)

 板端 Qt 的触屏操作 ─(同一条 TCP:9100)─> 运行时控制台 ─> RtParams 热更新 ─> 立即生效（不用重启/重编译）
```

**一句话亮点**：采集一帧，**NV12 直送硬编码器、RGB888 送 NPU**，两条链路互不打扰；
控制上行、视频下行，**双通道**架构。

## 功能特性（均为板上实测通过）

### 检测与业务逻辑
- [x] NPU 目标检测：YOLOv7-tiny(RKNN int8)，白名单 person/car，letterbox + 三尺度解码 + NMS
- [x] 警戒区域 ROI：**可在板端屏上手指拖动四角/框体实时调整**
- [x] 入侵判定：检测框**底部中心**进入 ROI → INTRUDE
- [x] 滞留告警：在 ROI 内停留超 N 秒 → ALARM（截图存档）
- [x] 离开/解除 + **离开去抖**（连续 N 帧缺席才算真的离开，抑制抖动误判）
- [x] 事件可视化：**ROI 内框=红/青（会报警）、ROI 外=灰（看到但不报警）**

### 视频链路
- [x] V4L2 采集（Multiplanar NV12 + mmap 4 缓冲 + poll）
- [x] **H.264 硬编码推流**（appsrc → mpph264enc → MPEG-TS → TCP 服务端），延迟 **< 1s**
- [x] 一帧两用：RGB888 → NPU，NV12 → 硬编码器（互不阻塞，队列满即丢帧保实时）
- [x] 板端 Qt 自己拉流硬解显示（tcpclientsrc → mppvideodec → appsink）
- [x] 断线/推流端未启动时**自动重连**

### 交互与控制
- [x] **运行时控制台**（终端）：status / roi / stay / leave / conf / nms / log / report / events / save / reload / quit
- [x] **参数热更新**：原子参数 + version 号，改参立即生效，**不重启、不重编译**
- [x] **板端 Qt 触屏界面**：状态页（帧率/统计/事件列表）+ 参数页（各项 `−/+` 按钮、上报开关、保存/重载）
- [x] 控制命令**单一入口**：屏幕操作与终端手敲走同一套解析与热更新链路

### 输出与上位机
- [x] 告警截图存盘（PPM）+ 事件日志环形缓冲
- [x] PC 上位机（Qt）：实时预览、彩色事件日志、告警照片归档与回查、统计
- [x] 配置持久化：`save`/`reload` 与 `config/intruder.conf` 互通（只接管自己管理的键，不破坏注释）

## 环境（已核实）

| 项 | 值 |
|---|---|
| 板卡 | 正点原子 ATK-DLRK3568（RK3568，Buildroot） |
| NPU | librknnrt 1.5.0；RKNN C API；模型 `yolov7-tiny_tk2_RK356X_i8.rknn` |
| 摄像头 | OV13850 MIPI CSI → `/dev/video0`（Multiplanar NV12 1280x720@30） |
| 屏幕 | 5.5" MIPI DSI（物理 1080x1920 竖屏）；weston `rotate-90` + `scale=2` → 逻辑 **960x540 横屏** |
| 显示框架 | **Weston(Wayland)**；Qt 5.15.2（有 wayland/linuxfb 插件，**无 eglfs**） |
| 编解码 | GStreamer + rockchipmpp（`mpph264enc` / `mppvideodec` / `mpph265enc`） |
| 触摸屏 | Goodix GT9xx → `/dev/input/event2`（weston 转发给 Wayland 客户端） |
| PC 上位机 | Qt 5.11.1 (mingw53_32) |
| 交叉工具链 | `aarch64-buildroot-linux-gnu-g++` + SDK `staging`（含 Qt5 头文件/库） |

## 目录结构

```
rk3568-intrusion-detector/
├─ CMakeLists.txt              # 显式源文件列表；-DRKNN_INCLUDE_DIR/-DRKNN_LIB_DIR；可选 -DENABLE_STREAM
├─ build.sh / run.sh           # 一键交叉编译 / 板端运行
├─ config/intruder.conf        # 运行时可调参数（控制台 save/reload 与它互通）
├─ models/                     # yolov7-tiny_tk2_RK356X_i8.rknn
├─ src/
│  ├─ main.cpp                 # 4 种运行模式 + 四线程流水线组装 + 叠加/缩略图
│  ├─ common/
│  │  ├─ frame.hpp             # Frame（含 NV12 旁路字段）/ FramePtr / FrameQueue
│  │  ├─ block_queue.hpp       # 模板有界阻塞队列（满丢最旧）
│  │  ├─ config.hpp/.cpp       # key=value 配置解析
│  │  ├─ console.hpp/.cpp      # 运行时控制台 + RtParams 热更新 + EventLog + submit()
│  │  └─ log.hpp               # 极简日志 LOG_I/W/E
│  ├─ capture/v4l2_camera.*    # Multiplanar NV12 + mmap + poll；可选保留 NV12 供推流
│  ├─ infer/rknn_engine.*      # RKNN 六连 + letterbox + 三尺度解码/NMS + 运行时阈值
│  ├─ business/roi_monitor.*   # ROI 判定 + 事件状态机 + 离开去抖（**业务核心**）
│  └─ output/
│     ├─ reporter.*            # PC 上报：逐行 JSON + base64 缩略图（非阻塞连接）
│     ├─ statlink.*            # 板端状态通道(127.0.0.1:9100)：推状态 + 收控制命令
│     └─ streamer.*            # H.264 推流（appsrc→mpph264enc→MPEG-TS→TCP）
├─ board/qt_gui/               # **板端 Qt 界面**（Wayland；自写编译脚本，不用 qmake/moc）
│  ├─ main.cpp                 # 横屏布局 + ROI/检测框叠加 + 触摸拖拽 + 双页签参数页
│  ├─ gstsource.*              # 拉流硬解：tcpclientsrc→mppvideodec→appsink→QImage
│  ├─ statelink.*              # 状态通道客户端（QTcpSocket + QJsonDocument）
│  ├─ build.sh                 # 交叉编译（绝对路径链接 staging 的 Qt/gst）
│  └─ README.md                # 迭代3 全部踩坑与结论（wayland/性能/触摸）
├─ host/
│  ├─ receiver.py              # 最简命令行接收（调试用）
│  └─ qt_gui/                  # PC 上位机（Qt 5.11，事件日志 + 照片归档回查）
├─ tools/
│  ├─ board/                   # 板端验证脚本（h264_test.sh / stream_test.sh / v4l2_probe 等）
│  ├─ dev/                     # PC 侧辅助
│  └─ bench.sh                 # 帧率/占用实测
└─ docs/
   ├─ architecture.md          # 设计文档（调用链/线程模型/状态机）
   ├─ source-map.md            # 源码地图与端到端流程
   ├─ console.md               # 运行时控制台命令手册
   ├─ streaming.md             # H.264 推流：架构/编译开关/低延迟原理/踩坑
   ├─ study/                   # 01~04 概念与代码导读
   └─ devlog/                  # 每日开发日志
```

## 线程模型

```
[Capture]--FramePtr-->rawQ(2)-->[Infer]--InferOut-->resQ(2)-->[Business]--EventMsg-->evQ(16)-->[Output]
    │                                                              │                                 │
    └─ NV12 ─> Streamer(appsrc:5000)                   每100ms 发布状态到 StatLink:9100    截图存盘 / 上报 PC
                                                         取控制命令 → Console::submit()

另有：控制台线程(stdin + 外部命令队列)、StatLink 线程(状态通道收发)、板端 Qt 两个线程(拉流硬解 / UI 轮询)
NULL 指针作 EOF 哨兵逐级传递，保证 join 时优雅收尾
```

## 编译

### 交叉编译（推荐，Ubuntu + SDK 工具链）
```bash
./build.sh                # 自动探测 SDK staging，开启 H.264 推流
ENABLE_STREAM=0 ./build.sh   # 关闭推流（产物不依赖任何 gstreamer 库）

# 或手动指定
cmake .. -DENABLE_STREAM=ON \
         -DGSTREAMER_ROOT=$HOME/rk3568_linux_sdk/buildroot/output/rockchip_rk3568/staging/usr
make -j$(nproc)

# 板端 Qt 界面（独立编译，不走 qmake）
cd board/qt_gui && bash build.sh
```

> ⚠️ 交叉编译两个坑（已在脚本里规避）：
> 1. **绝不要把 `staging/usr/lib` 加进 `-L`** —— 里面有 glibc 的链接脚本 `libc.so`
>    （内容是板端绝对路径 `GROUP( /lib64/libc.so.6 ...)`），`-lc` 一命中就会去找宿主根目录的 `/lib64/libc.so.6`。
>    正确做法：绝对路径点名链接需要的 `.so` + `-Wl,-rpath-link`。
> 2. 板端 Qt 不用 `qmake`/`moc`（SDK 没编）：代码避开了 `Q_OBJECT`，信号槽全用 lambda。

## 运行

```bash
# 板子上
cd /userdata/aidemo
export STREAM_PORT=5000            # 开推流（不设则关）

# 模式：单图 / 视频帧序列 / 摄像头 / 四线程流水线
./rk3568_intrusion <model.rknn> pipe 0 0 1280 720 3 yolov7 0 <PC_IP> 9000 12
#                                        └─ROI─────┘ stay  模型     上报地址:端口 离开去抖

# 板端 Qt 界面（另一个终端）
export XDG_RUNTIME_DIR=/run
export QT_QPA_PLATFORM=wayland
./rkqttest [视频端口=5000] [状态端口=9100] [解码宽度=640]

# PC 上看实时画面（延迟 <1s）
ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 \
       -framedrop -f mpegts tcp://<板IP>:5000

# PC 上位机（事件日志 + 告警照片归档）
# 用 Qt Creator 打开 host/qt_gui/intruder_gui.pro 编译运行
```

### 主要环境变量

| 变量 | 默认 | 作用 |
|---|---|---|
| `STREAM_PORT` | 0（关） | H.264 推流监听端口（板子做服务端，PC 拉流） |
| `STREAM_BPS` | 2000 | 目标码率 kbps |
| `STREAM_GOP` | 15 | I 帧间隔（15 ≈ 0.5s 一个） |
| `STAT_PORT` | 9100 | 板端状态/控制通道端口（0=关） |

## 实测数据

| 指标 | 数值 |
|---|---|
| NPU 推理（纯检测，无推流） | **16.7 fps**（1280x720 输入） |
| 推流：30fps 进编码器 | **1419 帧 / 丢 0 帧**，推理帧率不受影响 |
| 推流码率（2000kbps 设定） | 实测 2~3.3 Mbps |
| **端到端延迟** | **< 1s**（起播 ~1s） |
| 板端 Qt UI（960x540 逻辑） | **~20 fps**，解码收帧 3052 / 显示 1947 / 顶掉 1104 |
| 主程序 + Qt 同时跑 | 检测 11~12 fps，Qt 15~20 fps（CPU 已饱和，见下） |

**关于性能的坦白**：4 核 A55 上同时跑「V4L2 采集 + ISP 3A + NPU 推理 + H.264 硬编码 +
硬解码 + Qt 光栅绘制 + weston 合成」，CPU 已接近饱和（`top` 四核 73~100%，其中
`rkaiq_3A_server` 独占 38.9%）。定位实验表明 UI 的 66ms/帧 主要花在
「1080p 窗口 buffer 上传 + weston CPU 合成」，而不是 Qt 绘制（去掉视频绘制/右栏各只提升 ~25%）。
这是硬件边界，不是代码 bug —— 所以架构上把“清晰实时”交给 PC 端 ffplay（延迟 <1s），
板端保留 15~20fps 的监看 + 交互。

## 关键设计（面试可讲）

1. **一帧两用**：V4L2 的一帧 NV12 同时给两个消费者 —— 转 RGB888 送 NPU、原样送硬编码器。
   靠 `Frame::nv12` 旁路字段 + `setKeepNv12()` 按需开启，不推流时不付额外拷贝。
2. **上行控制 / 下行视频的双通道**：视频走 H.264/TCP:5000，控制与状态走本机 TCP:9100
   （只听 `127.0.0.1`，与 WiFi 无关）。
3. **参数热更新无锁**：`RtParams` 原子参数 + `version` 版本号，控制路径只写、
   工作线程比对后统一应用 —— 不阻塞数据路径，也不用加锁。
4. **保实时优先**：所有队列有界、满时**丢最旧**；推流 `appsrc block=false` 队列满即丢帧，
   采集线程永不被阻塞。监控场景里“追上现在”比“不丢帧”重要。
5. **命令单一入口**：终端与触屏走同一套解析（`Console::submit()` 入队 →
   控制台线程串行执行），因此 `applyKv/doSave` 无需加锁，也不会出现两份逻辑不一致。
6. **失败不致命**：推流、上报、板端拉流全部可失败降级 + 自动重连；
   `tcpclientsrc` 连不上会让管道直接 error —— 靠 bus 监听 + 拆掉重建解决。
7. **任何网络路径上的阻塞调用都要有超时**：阻塞 `connect()` 曾导致「启动卡住」和
   “`quit` 退不出来”（PC 未监听时内核 SYN 重试几十秒）→ 改非阻塞 + poll。
8. **业务与时间解耦**：ROI 状态机只依赖传入时间戳、不读墙上时钟，所以同一段帧序列
   可加速回放、可回归测试（`video` 模式用合成时间轴）。

## 文档索引

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 设计文档：调用链 / 线程模型 / 状态机 |
| [docs/source-map.md](docs/source-map.md) | 源码地图与端到端流程（含命令速查） |
| [docs/console.md](docs/console.md) | 运行时控制台命令手册 |
| [docs/streaming.md](docs/streaming.md) | H.264 推流：架构 / 编译开关 / 低延迟原理 / 踩坑 |
| [board/qt_gui/README.md](board/qt_gui/README.md) | 板端 Qt：环境探测 / Wayland 路线 / 性能定位 |

## 开发日志

| 日期 | 进展 |
|---|---|
| 2026-09-03 | 仓库初始化 + 骨架搭建 |
| 2026-09-22 | 运行时控制台 + 参数热更新（迭代1，板上验证） |
| 2026-09-23 | 推流延迟 4s→<1s；进程内 H.264 推流；板端 Qt Step1~3a（Wayland 路线 + 叠加显示） |
| 2026-09-24 | 板端触屏交互：Step3b 拖拽调 ROI + Step3c 参数页/按钮 |

完整日志见 [docs/devlog/](docs/devlog/)。

## 里程碑

- [x] **M1** 视频文件全闭环（检测→ROI 判定→告警→截图）
- [x] **M2** 真 MIPI 摄像头 + 四线程工程化 + socket 上报联调（含离开去抖、跨机 Qt 上位机）
- [x] **迭代1** 运行时控制台 + 参数热更新
- [x] **迭代2** H.264 硬编码推流（端到端 <1s）
- [x] **迭代3** 板端 Qt 界面（Wayland 全屏 + 触屏拖 ROI + 在线调参）
- [x] **v0.1 功能冻结**（2026-09-24）—— 上面全部能力均已在板上实测通过
- [ ] 演示视频（设备在寝室不便拍摄，待补）

## 明确不做（范围护栏）

- 不训练/微调模型（使用现成 rknn 权重）
- 不做多目标跟踪（用 ROI 内持续检出简化，去抖代替跟踪）
- 不做 Web/云后端（PC 端仅轻量接收与归档）
- 不改内核/驱动/设备树
- 不追求板端高帧率 UI（CPU 饱和已知；清晰实时交给 PC 端）
