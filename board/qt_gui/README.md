# 板端 Qt 界面（迭代3）

> 目标：在板子的屏幕上跑一个 Qt 界面：**视频区 + ROI/检测框叠加 + 事件列表 + 参数设置页**。
> 本目录是**板端应用**（交叉编译），与 `host/qt_gui`（PC 上位机，Qt 5.11 mingw）是两回事。

## 1. 环境探测结论（2026-09-23，板上实测）

| 项 | 结果 | 对方案的影响 |
|---|---|---|
| Qt 模块 | 完整（Core/Gui/**Widgets**/Multimedia+GstTools/Quick/Qml…） | 用 Qt Widgets 即可 |
| 平台插件 | `libqlinuxfb.so`、`wayland-egl`、`wayland-generic`、offscreen、vnc | **没有 eglfs** + 固件是 Weston → GUI 必须走 **wayland** |
| 屏幕 | fb `1080x1920@32bpp`；**Wayland 逻辑尺寸 `720x1280`（竖屏）** | 纯 CPU raster 渲染：实测 UI 稳定 **30.8 fps** |
| DRM | `card0`/`card1` + `renderD128/129` | 后续要 GPU 加速有基础 |
| GStreamer | `kmssink` / `fbdevsink` / `waylandsink` + `mppvideodec`(硬解) | 视频渲染路径可选 |
| 输入 | `event0~event8`、`mouse*` | 触摸/鼠标交互可行 |
| SDK | staging 里有 **Qt5 头文件 + .so**，但**没有 host qmake/moc** | 自己写编译命令，避开 qmake/moc |
| 实测 | 解码收帧 1463 / UI 显示 1184 / 顶掉 279（≈19%），UI 31.3 fps | 丢帧策略生效，延迟不堆积 |

## 2. 技术路线

**Qt Widgets + Wayland + 自己起 gst pipeline（appsink 取帧 → QImage 自绘）**

- 不用 `QMediaPlayer`：它对 "MPEG-TS over TCP" 的 MRL 支持很差，而且会把 MPP 解码细节挡住（本来就想学）
- 不用 eglfs：板子没这个插件
- **不用 linuxfb / 不去抢 DRM**：厂固件里 weston 持有 DRM master，我们只当 Wayland 客户端
  （见第 4 节“显示路线”里的两次尝试与结论）

```
板端 Qt 应用
├─ 主界面(QWidget)       ← wayland 全屏，CPU raster（实测 30.8 fps @720x1280）
│   ├─ 视频区             ← 自己画的 VideoWidget(QImage)
│   ├─ ROI/检测框叠加      ← 在 QImage 上画
│   ├─ 事件列表           ← 同 PC 上位机的着色规则
│   └─ 参数设置页         ← 复用主程序的 RtParams 语义
└─ 视频源(GstVideoSource) ← 独立线程: 拉流 → 解码 → appsink → 信号/队列给 UI
```

## 3. 分步计划

- [x] **Step 1 最小验证**：全屏测试图 → 确认能显示、方向/分辨率、CPU 刷新帧率
- [x] **Step 2 接视频**：`gstsource.*` —— 拉流 + `mppvideodec` 硬解 → `QImage` → 画到窗口
- [x] **Step 3a 横屏布局 + 叠加显示**：`statelink.*` 接状态通道；
      左侧视频区（ROI 黄框 + 人/车检测框 + 置信度标签）+ 右信息栏（连接状态/帧率/统计/参数/事件列表）
- [x] **Step 3b 触摸拖动调 ROI**（当前）：拖四角=缩放、拖框内=移动；拖动中本地跟手显示，
      **松手才发一条 `roi x y w h`** → 主程序 `Console::submit()` → 与终端命令复用同一套热更新链路
- [ ] Step 3c 参数页 / 按钮（把运行时控制台搬到屏上）

### 触摸交互（Step 3b）

- **输入**：Wayland 下 weston 通常把单指触摸合成成鼠标事件，但版本行为不一 →
  `mouse*Event` 和 `QTouchEvent`（`event()` 里处理）**两条路都接**，走同一套 `onPress/onMove/onRelease`
- **拾取**：四角的拾取半径按**屏幕像素**算（34px）而不是视频坐标 —— 手指约 10mm，热区小了根本点不中
- **限制**：`normalized()` 处理拖翻；限制在画面内；最小 60x60（太小就再也点不中了）
- **为什么松手才发命令**：拖动中每帧都发会把状态通道和主程序刷爆（触摸事件可达 60Hz）
- **本地优先显示**：拖动中画的是本地那份 ROI（跟手），松手后交回主程序回报的值

## 4. 编译与运行

```bash
# Ubuntu（交叉编译）
cd board/qt_gui
bash build.sh                       # 产物 rkqttest

# 推板
adb push rkqttest /userdata/aidemo/

# 板上运行（必须 Wayland，理由见下面“显示路线”）
adb shell
cd /userdata/aidemo
export XDG_RUNTIME_DIR=/run
export WAYLAND_DISPLAY=wayland-0
QT_QPA_PLATFORM=wayland ./rkqttest          # 可加端口参数，默认 5000
```

要点：
- **不用 qmake**：SDK 没有 host qmake/moc；`build.sh` 直接调 `aarch64-buildroot-linux-gnu-g++`，
  用绝对路径链接 staging 里的 Qt `.so`（**不能把 staging 加进 `-L`**，理由见 `build.sh` 注释）
- **不用 `Q_OBJECT`**：信号槽全用 lambda，绕开 moc。后续要用自定义信号再单独解决
- 运行时若报 **找不到平台插件**：
  ```bash
  export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt/plugins/platforms
  ```
- 文字不显示 = 板端没字体（正常现象，看色条和方块即可）；补字体可试：
  ```bash
  export QT_QPA_FONTDIR=/usr/share/fonts
  ```

### 显示路线：必须用 Wayland，不要去抢 DRM（重要）

板子固件是 **Weston + Wayland** 架构：

```
root  628  /usr/bin/weston -w                 ← 合成器(占着 DRM master)
root  714  /usr/libexec/weston-desktop-shell
root  716  /opt/ui/systemui                   ← 正点原子出厂 UI（Wayland 客户端）
```

我们踩过的两条死路：
- `QT_QPA_PLATFORM=linuxfb` → `Failed to mmap framebuffer (Invalid argument)` +
  `linuxfb: Failed to initialize screen` + `no screens available` → abort。
  原因：`/dev/fb0` 是 **DRM 模拟的 fbdev 节点**，Qt 按 `virtual_size × bpp/8` 算的
  mmap 长度和驱动的 `smem_len` 对不上
- `QT_QPA_FB_DRM=1 QT_QPA_PLATFORM=linuxfb` → 屏幕**亮一下就恢复出厂界面**：
  Qt 确实画上去了，但 weston 是 DRM master，立刻把显示抢回去

✅ 正确做法：**当 Wayland 客户端**，让 weston 负责合成（还能吃到 GPU 加速）：

```bash
export XDG_RUNTIME_DIR=/run         # weston 的 socket 在这；别用 /var/run（符号链接，Qt 会抱怨）
export WAYLAND_DISPLAY=wayland-0
QT_QPA_PLATFORM=wayland ./rkqttest
```

> 这也解释了为什么板子的 Qt 插件里有 `libqwayland-egl.so` / `libqwayland-generic.so`
> 而**没有 eglfs** —— 固件本来就是按 Wayland 场景配的。

### 字体
- 板上字体很全：`/usr/share/fonts/{liberation,dejavu,noto-sans-sc,source-han-sans-cn,...}`
- 但**不指定族名**时 Qt 的 fallback 会失败（`load glyph failed err=24`，一个字都画不出来）
  → 代码里显式用 `Liberation Sans`（`fontFor()` 里带 DejaVu 兜底）

## 5. 视频链路（Step 2）

```
主程序: V4L2 → NV12 → appsrc → mpph264enc → MPEG-TS → tcpserversink:5000
                            │
板端 Qt: tcpclientsrc(127.0.0.1:5000) ! tsdemux ! h264parse ! mppvideodec(硬解)
         ! videoscale ! videoconvert ! video/x-raw,format=RGB,width=640,height=360
         ! appsink(max-buffers=2, drop=true)
         → 拉帧线程取 sample → QImage 副本 → "最新一帧"(mutex)
         → UI 线程 QTimer 每 33ms takeFrame() → drawImage 到视频区
```

设计要点：
- **板内自连走 `127.0.0.1`**：不经过 WiFi，排掉网络变量；也方便单机演示
- **只存“最新一帧”**：UI 画得慢就自动丢帧，**永远不会堆积延迟**
- **缩放放在 gst 侧**（`videoscale` → 640x360 后再转 RGB）：比全分辨率转换省 CPU
- **不用 Qt 信号槽跳线程**（需 moc）：用 mutex + QTimer 轮询，效果一样
- 运行时会打印：`解码收帧 / 显示 / 顶掉` 三个计数 —— “顶掉”持续增长说明 UI 跟不上，
  可把 `takeFrame` 的 33ms 调大、或把解码输出尺寸再调小

## 6. 状态通道（Step 3a，用于叠加显示）

叠加数据（ROI/检测框/统计）不走视频流，而是走主程序开的一条**本机文本通道**：

```
主程序 StatLink（服务端 127.0.0.1:9100）──每100ms一行JSON──> 板端 Qt StateLink（QTcpSocket）
   {"type":"STATE","fw":1280,"fh":720,"roi":[0,0,1280,720],"stay":3,...
    "fps":16.2,"events":2,"alarms":1,"pushed":584,"ev":"ALARM","es":3061,
    "dets":[{"c":0,"x1":923,"y1":245,"x2":1280,"y2":433,"p":0.53}]}
```

为什么单开一条而不是复用上报 PC 的 9000：那条是“**主程序主动连 PC**”（方向相反），
且状态是高频小数据，不该和事件上报混在一起。

- 主程序侧：`src/output/statlink.hpp/.cpp`（`STAT_PORT=0` 可关）
- Qt 侧：`statelink.hpp/.cpp`（QJsonDocument 解析，断线每秒重连）
- 叠加坐标映射：`视频区rect = fitAspect(左区, 16:9)`，再按 `视频区宽 / fw` 缩放 ROI/检测框
- 上行命令的接收队列已在主程序侧预留（`StatLink::takeCommand`）→ Step 3b 直接用

## 7. 横屏

板子是 **MIPI DSI 竖屏（`card0-DSI-1` 模式 `1080x1920`）**，要横屏用只能旋转输出：

```bash
# 板上，追加到 /etc/xdg/weston/weston.ini 末尾
[output]
name=DSI-1
transform=rotate-90      # 方向不对就换 rotate-270

killall weston           # 注意：weston 不会自动重启，需 reboot
reboot
```

旋转在 weston 层完成，**Qt 应用代码不用改** —— 但要注意窗口尺寸会变：
`rotate-90` 后宽高互换，我们的窗口从 `720x1280` 变成 **`1920x1080`**（实测值）。
回滚：删掉 `[output]` 那段再 reboot。

## 8. 性能：1920x1080 下的两个必做优化

旋转后窗口从 720x1280 变成 **1920x1080**（像素数 4 倍），实测直接暴露两个瓶颈：

| 问题 | 现象 | 修法 |
|---|---|---|
| `SmoothPixmapTransform` 平滑放大 | 640x360 → ~1500x850 每帧插值 127 万像素，**UI 31fps → 13fps**，顶掉 56% | **关掉**（用最近邻）。粗糙一点但快好几倍 |
| 右栏每帧重绘几十行文字 | 字形渲染很贵，且内容 100ms 才变一次 | 右栏缓存成 **QPixmap**，只内容变化时重画，每帧一次 blit |

实测（两处优化前 / 后）：
```
优化前：UI 重绘 13.4 fps | 解码收帧 2125 显示 941 顶掉 1184   ← 慢一半
优化后：UI 重绘 ~30 fps  | 顶掉 明显下降
```

解码输出尺寸可用第三个参数调（默认 640，越大越清晰但越贵）：
```bash
QT_QPA_PLATFORM=wayland ./rkqttest 5000 9100 960    # 960x540，画面更清楚
```
