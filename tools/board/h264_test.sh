#!/bin/sh
# 板端 H.264/H.265 硬件编码验证（GStreamer + Rockchip MPP）
# 用法: ./h264_test.sh [h264|h265] [秒数] [输出文件]
#   例: ./h264_test.sh h264 5 /userdata/aidemo/test_h264.mp4
# 说明: 用 MPP 硬编码器(mpph264enc/mpph265enc)把摄像头画面编码成 MP4,
#       用于验证"编解码链路可用",是后续"真视频流监控"的基础。
set -e

CODEC=${1:-h264}
SECS=${2:-5}
OUT=${3:-/userdata/aidemo/test_${CODEC}.mp4}

case "$CODEC" in
  h264) ENC=mpph264enc ;;
  h265) ENC=mpph265enc ;;
  *) echo "用法: $0 [h264|h265] [秒数] [输出文件]"; exit 1 ;;
esac

echo "== 检查 GStreamer 硬编码插件 =="
gst-inspect-1.0 "$ENC" >/dev/null 2>&1 || { echo "缺少 $ENC 插件"; exit 1; }
echo "OK: $ENC"

# 30fps × 秒数 = 采集帧数
FRAMES=$((30 * SECS))

echo "== 采集 OV13850(/dev/video0) 并用 $ENC 硬编码 $SECS 秒 -> $OUT =="
# 注意: -e 让管道收到 EOS 时正常收尾(否则 mp4 尾部索引不完整)
gst-launch-1.0 -e \
  v4l2src device=/dev/video0 num-buffers="$FRAMES" ! \
  video/x-raw,format=NV12,width=1280,height=720 ! \
  "$ENC" ! \
  h264parse ! mp4mux ! filesink location="$OUT" 2>&1 || {
    # h265 时 parse 元素不同
    gst-launch-1.0 -e \
      v4l2src device=/dev/video0 num-buffers="$FRAMES" ! \
      video/x-raw,format=NV12,width=1280,height=720 ! \
      "$ENC" ! h265parse ! mp4mux ! filesink location="$OUT"
  }

echo "== 结果 =="
ls -lh "$OUT"
BYTES=$(stat -c %s "$OUT" 2>/dev/null || echo 0)
[ "$BYTES" -gt 0 ] && echo "平均码率约: $((BYTES * 8 / SECS / 1000)) kbps"
echo
echo "下一步(迭代2 预览方案):"
echo "  # PC 端直接播放(需同一网络):"
echo "  gst-launch-1.0 -v v4l2src ! ... ! mpph264enc ! rtph264pay config-interval=1 ! udpsink host=<PC_IP> port=5000"
echo "  # PC: ffplay/ffmpeg 接收 udp://@:5000 或 gst 接收显示"
