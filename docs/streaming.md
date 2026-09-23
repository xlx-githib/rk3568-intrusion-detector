# H.264 视频流（迭代2）：程序内推流

> 目标：让程序自己把摄像头画面编码成 H.264 推出去，PC / 后续板端 Qt 都能看到**实时**视频。
> 结论：`appsrc → mpph264enc(RK 硬件编码) → MPEG-TS → TCP`，**实测端到端延迟 < 1s**。

## 1. 架构：为什么在程序里推，而不是外面跑 gst-launch

```
                      ┌─ RGB888 ─→ RknnEngine(推理) → RoiMonitor → Reporter(事件/缩略图)
V4L2 /dev/video0 ─→ Frame
                      └─ NV12   ─→ Streamer(appsrc) → mpph264enc → mpegtsmux → tcpserversink
```

- 早先 `tools/board/stream_test.sh` 用**外部 gst-launch** 验证链路可行，但它和我们的程序**抢同一个 `/dev/video0`**（同一时刻只能被一个进程打开），无法"边检测边推流"。
- 所以改为**进程内推流**：一次采集，两个消费者（推理要 RGB、编码要 NV12），互不干扰。
- 两个消费者**不需要互相同步**：推流推到队列满就丢帧（保实时），推理该丢帧也丢帧（保实时）。

| 对比项 | 外部 gst-launch | 进程内 appsrc（本方案） |
|---|---|---|
| 与推理并存 | ✗ 抢摄像头 | ✓ 同一帧双用 |
| 参数联动 | ✗ 两套配置 | ✓ 可跟运行时控制台打通 |
| 部署 | 依赖脚本 | ✓ 单二进制 |

## 2. 编译开关（默认关，不影响老构建）

推流依赖 GStreamer，所以做成**可选**：不开时二进制不链接任何 gst 库，行为与以前完全一致。

```bash
# 交叉编译（推荐：build.sh 会自动探测 SDK staging）
./build.sh

# 等价于（手动）
cmake .. -DENABLE_STREAM=ON \
         -DGSTREAMER_ROOT=$HOME/rk3568_linux_sdk/buildroot/output/rockchip_rk3568/staging/usr
make -j$(nproc)

# 关掉推流
ENABLE_STREAM=0 ./build.sh
```

- **交叉编译不走 pkg-config**：staging 里的 `.pc` 前缀指向板端绝对路径，会让链接器找错地方，所以 CMake 里直接给 include/lib 目录。
- 板端原生编译（`-DENABLE_STREAM=ON` 不带 `GSTREAMER_ROOT`）才走 `pkg-config`。
- 实现隔离在 `src/output/streamer.cpp`：**头文件不含任何 gst 类型**（用 `void*` 擦除），所以别的 `.cpp` 编译时不需要 gst 头。

## 3. 运行

```bash
# 板子（build 目录下）
STREAM_PORT=5000 ./rk3568_intrusion model/yolov7-tiny_tk2_RK356X_i8.rknn \
    pipe 0 0 1280 720 3 yolov7 0 <PC_IP> 9000 12
```

| 环境变量 | 默认 | 说明 |
|---|---|---|
| `STREAM_PORT` | 0（关） | TCP 监听端口；板子是**服务端**，PC 主动连（避开 Windows 入站防火墙问题） |
| `STREAM_BPS` | 2000 | 目标码率 kbps（注意属性名是 `bps`，不是 `bitrate`） |
| `STREAM_GOP` | 15 | I 帧间隔；15 ≈ 0.5s 一个 I 帧，便于"随时接入" |

程序启动后会打印可复制的播放命令。运行中 `status` 也会显示推流状态：

```
  上报 192.168.43.45:9000 连接=yes | 推流 on 成功=1234 丢=3
```

## 4. PC 端播放（低延迟关键）

```bash
ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 \
       -framedrop -f mpegts tcp://192.168.43.195:5000
```

**为什么延迟从 4s 掉到 <1s**：ffplay 默认要缓冲 5s 才开始播放，上面这几个参数把"探测/分析/缓冲"关掉，画面就跟手了。

| 参数 | 作用 |
|---|---|
| `-fflags nobuffer` | 不做输入缓冲 |
| `-flags low_delay` | 解码器走低延迟模式 |
| `-probesize 32 -analyzeduration 0` | 不去"先分析几秒流格式"，立刻起播（解决"要等 5s 才有画面"） |
| `-framedrop` | 跟不上就丢帧，不排队（宁可丢帧不积压） |

VLC 对 "MPEG-TS over TCP" 支持较差（MRL 语法苛刻），验证请优先用 ffplay；VLC 想试就写 `tcp://192.168.43.195:5000:demux=ts`。

## 5. 延迟来自哪里

| 环节 | 延迟量 | 我们怎么处理 |
|---|---|---|
| 摄像头 + V4L2 buffer | ~1 帧 | 队列深度 2，满了丢旧帧 |
| 编码 `mpph264enc` | 0 额外帧 | RK 编码器**不支持 B 帧**，没有重排序延迟（反而对实时有利） |
| `appsrc` 队列 | ≤3 帧 | `max_bytes` 限 3 帧，满了就丢（`block=false` 绝不阻塞采集线程） |
| 网络 | 局域网 ~10ms | MPEG-TS over TCP，无重传风暴（有线/5G WiFi 更好） |
| **播放器缓冲** | **曾是 ~4s（瓶颈）** | 低延迟 ffplay 参数 → ~0.1s |

> 教训：调延迟先量"播放器缓冲"，别一上来怀疑编码/网络。

## 6. 关键实现点

```c
appsrc name=src is-live=true block=false format=time do-timestamp=false
! video/x-raw,format=NV12,width=W,height=H,framerate=F/1
! mpph264enc header-mode=1 gop=15 bps=2000000
! h264parse config-interval=1
! mpegtsmux
! tcpserversink host=0.0.0.0 port=P sync=false
```

- **`header-mode=1`（最关键）**：默认 `0=first-frame` 只在第一帧带 SPS/PPS，**中途接入的播放器**拿不到参数集 → 报 `non-existing PPS 0 referenced` 黑屏。设 1 + `h264parse config-interval=1` 才能"随时接入"。
- **`block=false`**：appsrv 队列满时 `push_buffer` 立即失败返回，我们计 `dropped` 丢帧 —— 采集线程绝不会被推流拖住（实时性优先）。
- **`do-timestamp=false` + 自己填 PTS/DTS**：用采集时刻（首帧作原点转相对时间），时间轴更准。
- **`sync=false`**：`tcpserversink` 不做时钟同步，降延迟。
- **NV12 复用**：`V4l2Camera::setKeepNv12(true)` 才多拷一份 NV12（1.38MB/帧，亚毫秒）；不推流时不付这个代价。

## 7. 待办 / 后续

- [ ] H.265（`mpph265enc`）对比：同画质码率约省 30~40%，代价是编码耗时与兼容性
- [ ] 板端 Qt 显示视频（迭代3）：`mppvideodec` 或 EGL 直出到 DRM plane
- [ ] 分辨率/帧率可配（当前硬编码 1280x720@30）
- [ ] 与运行时控制台打通：`stream on|off`、`bps 1500` 在线调码率
