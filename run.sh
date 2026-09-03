#!/bin/bash
# 一键运行（板端）：默认读取 config/intruder.conf
cd "$(dirname "$0")"
CONF="${1:-config/intruder.conf}"
echo "== run with config: $CONF =="
./build/rk3568_intrusion "$CONF"
