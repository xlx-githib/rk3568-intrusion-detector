#!/bin/bash
# 一键构建（板端原生或交叉编译环境均可，前提是 CMake + gcc 可用）
#
# 可选 H.264 推流（GStreamer）：交叉编译时自动探测 SDK 的 staging/usr；
#   关掉：ENABLE_STREAM=0 ./build.sh
#   指定：GSTREAMER_ROOT=/path/to/staging/usr ./build.sh
set -e
cd "$(dirname "$0")"
mkdir -p build
cd build

EXTRA="-DENABLE_STREAM=OFF"
if [ "${ENABLE_STREAM:-1}" != "0" ]; then
    : "${GSTREAMER_ROOT:=$HOME/rk3568_linux_sdk/buildroot/output/rockchip_rk3568/staging/usr}"
    if [ -f "$GSTREAMER_ROOT/lib/libgstapp-1.0.so" ]; then
        echo "== 推流: 开启 (GSTREAMER_ROOT=$GSTREAMER_ROOT) =="
        EXTRA="-DENABLE_STREAM=ON -DGSTREAMER_ROOT=$GSTREAMER_ROOT"
    else
        echo "== 推流: 未找到 SDK staging（期望 $GSTREAMER_ROOT/lib/libgstapp-1.0.so），本次跳过 =="
    fi
fi

cmake .. $EXTRA
make -j"$(nproc)"
echo "== build done: $(pwd)/rk3568_intrusion =="
