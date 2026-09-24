# 运行时控制台（在线调参 / 配置热更新）

> 目的：不用"改参数 → 重编译 → 重新部署"，在程序**运行中**直接调参、看状态、查事件。
> 实现：`src/common/console.hpp/.cpp`（`Console` + `RtParams` + `EventLog`）。

## 1. 为什么这样做

| 传统做法 | 本方案 |
|---|---|
| ROI/阈值写死在代码里 → 每次调参重编译 | 参数放在 `RtParams`（原子变量），控制台改完**下一帧生效** |
| 调试靠 printf 猜状态 | `status` 一条命令看帧率/队列水位/累计事件/上报连接 |
| 事件过去就没了 | `events` 查最近 200 条事件 |
| 参数只在内存里 | `save` 写回 `config/intruder.conf`，`reload` 从文件重载 |

数据流：**控制台线程**（poll stdin）→ 改 `RtParams` → `version++` → **Business 线程**发现版本变化 → 应用到 `RoiMonitor` / `RknnEngine`（无锁竞争，不改流水线结构）。

## 2. 命令一览

| 命令 | 说明 |
|---|---|
| `help` / `?` | 命令帮助 |
| `status` | 运行状态：帧率、ROI/阈值/去抖、队列水位、累计事件、上报连接 |
| `roi <x> <y> <w> <h>` | 在线设置警戒区（会校验不超出画面） |
| `stay <秒>` | 停留多久触发告警 |
| `leave <帧数>` | 离开去抖帧数（连续缺席多少帧才算离开） |
| `conf <0~1>` | 置信度阈值 |
| `nms <0~1>` | NMS IoU 阈值 |
| `log <0~3>` | 日志级别（0=D 1=I 2=W 3=E） |
| `report on\|off` | 开/关 socket 上报（现场排查网络时很有用） |
| `events [n]` | 查看最近 n 条事件（默认 10，最多 200） |
| `save` | 当前参数写回 `config/intruder.conf` |
| `reload` | 从配置文件重新加载参数 |
| `quit` / `exit` | 退出程序（优雅收尾：结束采集→逐级 flush→断开上报） |

## 3. 用法示例

板上（`pipe` 模式自带控制台）：
```bash
cd /userdata/aidemo
./rk3568_intrusion model/yolov7-tiny_tk2_RK356X_i8.rknn pipe 0 0 1280 720 3 yolov7 0 192.168.43.45 9000 12

# 程序运行中直接输入：
status
roi 300 200 600 400          # 现场把警戒区挪到画面中下部
conf 0.4                     # 提高置信度，少误报
leave 15                     # 抖动大时加大去抖
report off                   # 暂停上报做本地排查
events 20                    # 回看最近 20 条事件
save                         # 满意后写回配置，下次开机即用
quit
```

> 通过 `adb shell` 或串口终端运行时可正常交互输入；若以后台/重定向方式运行，控制台自动退化为无输入（不影响主流程）。

## 4. 与配置文件的关系

`save` / `reload` 只接管这些键，**其它行原样保留**（不破坏注释与其它配置）：

```
roi_x / roi_y / roi_w / roi_h
stay_sec / leave_confirm
conf_thresh / nms_thresh
report_enable
```

## 5. 扩展点

- 新增可调参数：在 `RtParams` 加一个 `atomic<...>` 字段 → `applyKv`/`kvOf` 各加一行 → Business 线程应用处加一行
- 需要"交互式画 ROI"时，可加 `roi pick` 子命令（配合 `shots/roi_preview.ppm` 预览图）

## 6. 命令来源：终端 + 板端触摸界面（同一条链路）

命令有两个入口，但**只有一套解析与热更新逻辑**：

```
终端 stdin  ─┐
             ├─→ Console::loop() 串行处理 ─→ onLine() ─→ RtParams ─→ 工作线程应用
状态通道    ─┘
（外部线程 submit）
```

- 终端：`poll(stdin)` 直接读
- 外部（板端 Qt 界面触摸调 ROI）：`Console::submit(line)` 入队 → 控制台线程取出执行
  - 为什么走队列而不是直接执行：`applyKv` / `doSave` 这些逻辑不是线程安全的，
    **让所有命令都在控制台线程里串行执行**，就完全不用加锁
  - 队列有界（16 条），防止对端刷爆内存

数据来源：`StatLink`（`127.0.0.1:9100`）的 `takeCommand()`，由 `run_pipe` 的 Business 线程轮询转交：

```cpp
string cmd;
while (stat.takeCommand(cmd)) con.submit(cmd);   // 与终端手敲的格式完全一致
```

所以板上手指拖一下 ROI，效果和你在终端敲 `roi 300 200 600 400` 一模一样。
这是“**上行走控制、下行走视频**”的双通道设计：视频/状态下沉，控制上行。
