# 源码地图与端到端流程解析

> 目的：一页看清"代码怎么组织、数据怎么流动"，便于继续开发与面试讲解。
> 设计（为什么这么拆）见 [architecture.md](architecture.md)；逐日进展见 [devlog/](devlog/)。

## 1. 端到端流程（一句话版本）

```
OV13850(MIPI) ─V4L2─> NV12帧 ──> RGB888 ──> letterbox 640x640
   ──> RKNN(NPU) 推理 ──> 三尺度解码+NMS+白名单 ──> DetObject(含底部中心)
   ──> ROI 状态机(含离开去抖) ──> Event(INTRUDE/ALARM/LEAVE/RESOLVE)
   ──> 截图存盘 + 逐行 JSON(+base64 缩略图) over TCP ──> WiFi ──> PC Qt 上位机
                                                            (实时画面/事件日志/照片归档回查)
```

## 2. 源码树（当前实现，与 README 早期骨架图的差异已按实际更新）

```
rk3568-intrusion-detector/
├─ CMakeLists.txt              # 显式源文件列表(不用 GLOB_RECURSE)；-DRKNN_INCLUDE_DIR/-DRKNN_LIB_DIR
├─ config/intruder.conf        # 可调参数(ROI/stay/上报地址等)
├─ models/                     # yolov7-tiny_tk2_RK356X_i8.rknn 等
├─ src/
│  ├─ main.cpp                 # 4 种运行模式 + 四线程流水线组装 + 缩略图/预览帧
│  ├─ common/
│  │  ├─ frame.hpp             # Frame{width,height,fmt,pts_us,data} / FramePtr / FrameQueue
│  │  ├─ block_queue.hpp       # 模板有界阻塞队列(满丢最旧)，流水线解耦核心
│  │  ├─ config.hpp/.cpp       # key=value 配置解析(# 注释)
│  │  ├─ console.hpp/.cpp      # 运行时控制台(stdin 命令) + RtParams 原子参数热更新 + 事件环形日志
│  │  └─ log.hpp               # 极简日志 LOG_I/W/E(带时间戳)
│  ├─ capture/v4l2_camera.hpp/.cpp   # Multiplanar NV12 + mmap(4 buffer) + poll + NV12→RGB888(BT.601)
│  ├─ infer/rknn_engine.hpp/.cpp     # RKNN 加载/查询/推理 + letterbox + 三尺度解码 + NMS + 白名单
│  ├─ business/roi_monitor.hpp/.cpp  # ROI 判定 + 事件状态机 + 离开去抖（业务核心）
│  └─ output/                  # reporter.hpp/.cpp: TCP JSON 上报+缩略图；streamer.hpp/.cpp: H.264 推流
├─ host/
│  ├─ receiver.py              # 最简命令行接收(调试用)
│  └─ qt_gui/                  # Qt 5.11 上位机(.pro/.h/main.cpp)
├─ tools/
│  ├─ board/                   # 板端验证脚本: v4l2_probe.c / video_to_frames.py / test_*.py
│  ├─ dev/                     # PC 侧开发辅助: test_debounce.cpp + rknn_api.h(stub) + 脚本
│  └─ bench.sh                 # 帧率/占用实测
└─ docs/
   ├─ architecture.md          # 设计文档(调用链/线程模型/状态机)
   ├─ source-map.md            # ← 本文件：实现视角的源码地图与流程
   ├─ streaming.md             # H.264 推流：架构/编译开关/低延迟播放/关键参数/踩坑
   ├─ study/                   # 01~04 概念/代码导读
   └─ devlog/                  # 每日开发日志
```

## 3. 模块职责与关键接口

| 模块 | 职责 | 关键接口 / 类型 | 要点 |
|---|---|---|---|
| `common/block_queue.hpp` | 线程间有界队列 | `BlockQueue<T>::push/pop(timeout)` | 满时**丢最旧**保实时；`pop` 带超时 |
| `common/frame.hpp` | 帧对象与帧队列 | `Frame` / `FramePtr` / `FrameQueue` | 帧用 `shared_ptr` 流转，避免深拷贝 |
| `common/config.*` | 配置解析 | `Config::load/get/getInt/getBool` | `key=value`，忽略 `#` 注释 |
| `capture/v4l2_camera.*` | 采集一帧 | `open(dev,w,h)` / `start` / `getFrame(FramePtr&,timeout_ms)` / `close` | Multiplanar NV12 + mmap 4 缓冲 + poll 超时；转 RGB888 并打 `pts_us` |
| `infer/rknn_engine.*` | 目标检测 | `init(model, YoloParams, watch_cls)` / `infer(FramePtr, vector<DetObject>&)` | RKNN 六连；letterbox；**want_float=1 + fp 解码**；三尺度(strides 8/16/32)+NMS+白名单(person/car) |
| `business/roi_monitor.*` | ROI 事件 | `configure(roi, stay_sec, watch_cls, leave_confirm_frames)` / `feed(dets, now_us)->vector<Event>` | 状态机 IDLE→INSIDE→ALARMED；判定用**检测框底部中心**；离开去抖见下 |
| `common/console.*` | 运行时调参 | `RtParams` / `EventLog` / `Console::start` | 原子参数 + `version` 号热更新：控制路径只写、业务线程比对后统一应用 |
| `output/streamer.*` | H.264 推流 | `start(w,h,fps,port,bps,gop)` / `push(nv12,bytes,pts_us)` / `stop` | `appsrc block=false`+3 帧上限→满即丢帧不阻塞采集；`header-mode=1` 才能随时接入(详 `docs/streaming.md`) |
| `output/reporter.*` | 上报 | `init/connect/report/reportImg/reportPreview` | TCP 逐行 JSON；大消息**循环发送**；断线自动重连 |
| `main.cpp` | 组装与运行 | 4 种模式 + `run_pipe` 四线程 | 见下节 |

## 4. 四线程流水线（`pipe` 模式）

```
[Capture 线程] --FramePtr--> rawQ(2) --> [Infer 线程] --InferOut--> resQ(2) --> [Business 线程]
                                                                                  |
                                                                      EventMsg --> evQ(16)
                                                                                  v
                                                                            [Output 线程]
                       (CAPTURE→INFER→BUSINESS→OUTPUT；nullptr 作 EOF 哨兵逐级收尾)
```

- **Capture**：`cam.getFrame()` → `rawQ.push()`，结束推 `nullptr`
- **Infer**：`rawQ.pop()` → `eng.infer()` → `resQ.push(InferOut{frame,dets})`
- **Business**：`resQ.pop()` → `mon.feed(dets, frame->pts_us)` 产事件；**同时**每 ~333ms 额外推一帧 `EventMsg{preview=true}`（现场预览）
- **Output**：
  - 预览帧 → `downscale_rgb(320×180)` → `rep.reportPreview()`（Qt 持续刷新画面）
  - ALARM → 存全图 `shots/pipe_alarm_*.ppm` + `downscale_rgb(640×360)` → `rep.reportImg()`（Qt 显示并归档）
  - 其它事件 → `rep.report()`

**背压策略**：队列有界（2/2/16），满时丢最旧帧，宁可掉帧也不阻塞整条链。

## 5. 上报协议（与 `host/` 对齐）

| 消息 | 字段 | 说明 |
|---|---|---|
| 事件 | `type`(INTRUDE/ALARM/LEAVE/RESOLVE), `cls`(0=人,2=车), `ts_start_ms`, `stay_ms`, `snapshot` | ALARM 额外带 `thumb_w/thumb_h/img`(base64 RGB) |
| 预览 | `type=PREVIEW`, `thumb_w`, `thumb_h`, `img` | 周期性现场画面，Qt 只刷新画面不入日志 |

Qt 端要点：`QTcpServer` 监听；**每连接独立半包缓冲**(`QHash<QTcpSocket*,QString>`)；按 `\n` 切行；
ALARM → 红框显示 + 存 `alarms/alarm_<时间戳>.png` + 列表项 + 状态栏提示；点历史列表项回看照片。

## 6. 关键设计决策（为什么这么写）

| 决策 | 原因 / 收益 |
|---|---|
| NPU 输出用 `want_float=1` + fp 解码 | `want_float=0`(int8) 触发 NPU `OutputOperator size_with_stride` 提交失败 |
| 全程零 OpenCV | 板端无 OpenCV 开发包；自写 NV12→RGB888 与 PPM 输出 |
| CMake 显式源文件列表 | `GLOB_RECURSE` 不会捕获新增 `.cpp`，易漏编 |
| 队列满丢最旧 | 实时性优先，避免慢下游拖垮上游 |
| ROI 用检测框**底部中心**判定 | "人站进区域"才算入侵，框顶越过不算 |
| 离开去抖（连续 N 帧缺席才算离开） | 抑制边缘抖动/遮挡导致的 INTRUDE↔LEAVE 抖动；**缺席窗内不做停留超时判定**，避免人已走却误告警 |
| 缩略图 base64 进同一 JSON 行 | 免额外文件通道；`try_send` 循环发送防大消息被截断 |
| 预览帧降规格（320×180、~3fps） | 弱网(曾 -93dBm)易 TCP 积压 → Qt 卡顿/断开延迟；降带宽后可稳定 |
| Qt 每连接独立缓冲 | 反复重连(弱网断开慢)时多连接共用一个缓冲会让 JSON 行错乱 |
| 板端不开 Qt | 板端只做采集/推理/上报，GUI 放 PC，保持端侧轻量 |

## 7. 运行模式与命令速查

```bash
# 交叉编译(Ubuntu)
cd ~/rk3568-intrusion-detector/build
cmake .. -DRKNN_INCLUDE_DIR=<rknpu2>/runtime/RK356X/Linux/librknn_api/include \
         -DRKNN_LIB_DIR=<rknpu2>/runtime/RK356X/Linux/librknn_api/aarch64
make -j4 && adb push rk3568_intrusion /userdata/aidemo/

# 模式1 单图
./rk3568_intrusion model/yolov7-tiny_tk2_RK356X_i8.rknn frames/f_0000.rgb 640 289 yolov7 out.ppm
# 模式2 视频帧序列(离线，含首帧 ROI 黄线预览 shots/roi_preview_video.ppm)
./rk3568_intrusion model/….rknn video frames 0 192 640 97 3 yolov7 [leave_confirm]
# 模式3 摄像头单线程
./rk3568_intrusion model/….rknn cam  0 0 1280 720 3 yolov7 [max_frames] [leave_confirm]
# 模式4 四线程流水线 + 上报
./rk3568_intrusion model/….rknn pipe 0 0 1280 720 3 yolov7 0 <PC_IP> 9000 [leave_confirm]

# PC 端
python -u host/receiver.py 9000          # 命令行接收
# 或 Qt 上位机: 打开 host/qt_gui/intruder_gui.pro → 运行(监听 9000)

# PC 侧单测(无需板子)
g++ -std=c++17 -I src -I tools/dev src/business/roi_monitor.cpp tools/dev/test_debounce.cpp -o t && ./t
```

## 8. 当前状态与待办

**已达成**：M1 视频闭环、M2 实时流水线与 socket 上报、离开去抖、跨机链路（板 WiFi → PC Qt 实时画面/事件/照片归档回查）。

**待办**：
1. 工作区仍有未提交改动（`src/business/*`、`capture/v4l2_camera.cpp`、`block_queue.hpp`、`infer/*`、`main.cpp` 的 `std::` 前缀清理等）与未入库文件（`docs/devlog/2026-09-09.md`）
2. `.gitignore` 需排除隐私/大文件：简历 `*.docx`、`tools/board/*.mp4`、临时 `_*.txt`
3. **README 仍是早期骨架版**（功能清单未勾选、目录结构过时、里程碑未更新）—— M3 冻结待做
4. Release v0.1 打 tag + 演示视频 + `docs/study/04` 走读文档补完
5. 可选拓展（按价值）：多目标跟踪(轨迹 ID) / 板端 MPP H.264 推流 / 事件 SQLite 落盘与检索 / NTP 对时 / 开机自启与看门狗
