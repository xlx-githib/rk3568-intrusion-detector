# 板端 Qt 界面（迭代3）

> 目标：在板子的屏幕上跑一个 Qt 界面：**视频区 + ROI/检测框叠加 + 事件列表 + 参数设置页**。
> 本目录是**板端应用**（交叉编译），与 `host/qt_gui`（PC 上位机，Qt 5.11 mingw）是两回事。

## 1. 环境探测结论（2026-09-23，板上实测）

| 项 | 结果 | 对方案的影响 |
|---|---|---|
| Qt 模块 | 完整（Core/Gui/**Widgets**/Multimedia+GstTools/Quick/Qml…） | 用 Qt Widgets 即可 |
| 平台插件 | `libqlinuxfb.so`、`wayland-egl`、`wayland-generic`、offscreen、vnc | **没有 eglfs** → GUI 走 **linuxfb** |
| 屏幕 | `/dev/fb0` 存在，`1080x1920 @32bpp`（DRM 模拟 fb，`/proc/fb` name 为空） | 纯 CPU raster 渲染，性能要实测 |
| DRM | `card0`/`card1` + `renderD128/129` | 后续要 GPU 加速有基础 |
| GStreamer | `kmssink` / `fbdevsink` / `waylandsink` + `mppvideodec`(硬解) | 视频渲染路径可选 |
| 输入 | `event0~event8`、`mouse*` | 触摸/鼠标交互可行 |
| SDK | staging 里有 **Qt5 头文件 + .so**，但**没有 host qmake/moc** | 自己写编译命令，避开 qmake/moc |

## 2. 技术路线

**Qt Widgets + linuxfb + 自己起 gst pipeline（appsink 取帧 → QImage 自绘）**

- 不用 `QMediaPlayer`：它对 "MPEG-TS over TCP" 的 MRL 支持很差，而且会把 MPP 解码细节挡住（本来就想学）
- 不用 eglfs：板子没这个插件
- 不用 kmssink 直接输出：会与 Qt 抢同一个 fb

```
板端 Qt 应用
├─ 主界面(QWidget)       ← linuxfb 全屏，CPU raster
│   ├─ 视频区             ← 自己画的 VideoWidget(QImage)
│   ├─ ROI/检测框叠加      ← 在 QImage 上画
│   ├─ 事件列表           ← 同 PC 上位机的着色规则
│   └─ 参数设置页         ← 复用主程序的 RtParams 语义
└─ 视频源(GstVideoSource) ← 独立线程: 拉流 → 解码 → appsink → 信号/队列给 UI
```

## 3. 分步计划

- [x] **Step 1 最小验证**（当前）：`main.cpp` —— 全屏测试图，确认能显示、方向/分辨率、CPU 刷新帧率
- [ ] Step 2 接视频：gst 拉流 + `mppvideodec` 硬解 → `QImage` 显示
- [ ] Step 3 界面：ROI/检测框叠加、事件列表、参数页

## 4. 编译与运行

```bash
# Ubuntu（交叉编译）
cd board/qt_gui
bash build.sh                       # 产物 rkqttest

# 推板
adb push rkqttest /userdata/aidemo/

# 板上运行
adb shell
cd /userdata/aidemo
QT_QPA_PLATFORM=linuxfb ./rkqttest
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

### 如果 linuxfb 写不进 fb0（黑屏/没反应）

按顺序试：
1. `QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0 ./rkqttest`
2. `QT_QPA_FB_DRM=1 QT_QPA_PLATFORM=linuxfb ./rkqttest`（Qt 5.15 的 linuxfb 支持走 DRM）
3. 先 `cat /dev/urandom > /dev/fb0` 看屏幕有没有雪花 —— 有雪花说明 fb0 可写，问题在 Qt；没雪花说明 fb0 只是 DRM 的模拟节点
4. 走 wayland：板上启 `weston`，然后 `QT_QPA_PLATFORM=wayland ./rkqttest`
