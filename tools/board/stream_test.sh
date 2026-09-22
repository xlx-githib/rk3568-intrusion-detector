#!/bin/sh
# 板端 H.264 实时推流验证（OV13850 → MPP 硬编码 → MPEG-TS/UDP）
# 用法:
#   直推 PC:   ./stream_test.sh <PC_IP> [port] [bitrate_kbps] [gop]
#              ./stream_test.sh 192.168.43.45 5000 2000 30
#   板内回环自检(不需要 PC 播放器):
#              ./stream_test.sh self
# PC 端接收(VLC 最省事):
#   VLC → 媒体 → 打开网络串流 → udp://@:5000
#   或 ffplay: ffplay -fflags nobuffer -flags low_delay udp://@:5000
# 说明:
#   - 用 MPEG-TS 封装而非裸 RTP，VLC/ffplay 可直接播放，兼容性最好
#   - 跑之前请确认检测程序(pipe 模式)未占用 /dev/video0
set -e

W=1280; H=720; FPS=30

if [ "$1" = "self" ]; then
  echo "== 自检1: 同进程 编码→解码 环回(先验证 MPP 编解码器是否可用) =="
  gst-launch-1.0 -e v4l2src device=/dev/video0 num-buffers=$((FPS*5)) ! \
    video/x-raw,format=NV12,width=$W,height=$H,framerate=$FPS/1 ! \
    mpph264enc ! h264parse ! mppvideodec ! \
    fpsdisplaysink video-sink=fakesink text-overlay=false sync=false
  echo
  echo "== 自检2: RTP 收发环回(验证网络推流链路; 需 depay→parse→decode) =="
  gst-launch-1.0 -e v4l2src device=/dev/video0 num-buffers=$((FPS*5)) ! \
    video/x-raw,format=NV12,width=$W,height=$H,framerate=$FPS/1 ! \
    mpph264enc ! h264parse ! rtph264pay config-interval=1 pt=96 ! \
    udpsink host=127.0.0.1 port=5000 &
  TX=$!
  sleep 1
  # 接收端后台跑: UDP 无"流结束"通知, 用 SIGINT 触发 EOS 以打印 fps 统计
  gst-launch-1.0 -e udpsrc port=5000 \
    caps="application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96" ! \
    rtph264depay ! h264parse ! mppvideodec ! \
    fpsdisplaysink video-sink=fakesink text-overlay=false sync=false &
  RX=$!
  sleep 6
  kill -INT $RX 2>/dev/null || true
  wait $RX 2>/dev/null || true
  wait $TX 2>/dev/null || true
  echo "== 自检完成: 两段都打印 average fps 即为链路正常 =="
  exit 0
fi

PC_IP=${1:-}
PORT=${2:-5000}
BR=${3:-2000000}
GOP=${4:-30}

if [ -z "$PC_IP" ]; then
  echo "用法: $0 <PC_IP> [port] [bitrate_bps] [gop]   或   $0 self"
  exit 1
fi

echo "== 推流: ${W}x${H}@${FPS} H.264(MPP硬编码 ${BR}bps, GOP ${GOP}) -> udp://${PC_IP}:${PORT} =="
echo "== PC 端用 VLC 打开 udp://@:${PORT}  (Ctrl+C 结束推流) =="
exec gst-launch-1.0 -e \
  v4l2src device=/dev/video0 ! \
  video/x-raw,format=NV12,width=$W,height=$H,framerate=$FPS/1 ! \
  mpph264enc bitrate=$BR gop=$GOP ! \
  h264parse ! mpegtsmux ! \
  udpsink host=$PC_IP port=$PORT
