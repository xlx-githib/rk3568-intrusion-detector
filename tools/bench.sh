#!/bin/bash
# 帧率 / CPU / NPU 占用实测（在板端跑）
# 用法：先启动 rk3568_intrusion，再执行 ./tools/bench.sh [采样秒数]
DUR=${1:-10}
echo "== 采样 ${DUR}s，请确认程序正在运行 =="
top -b -n "${DUR}" -d 1 | grep -E "rk3568_intrusion|%Cpu" | tail -n "${DUR}"
echo "== NPU 频率 =="
cat /sys/class/devfreq/fde40000.npu/cur_freq 2>/dev/null || echo "N/A"
echo "== 程序自身打印的 FPS（若有）=="
