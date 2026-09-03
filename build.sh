#!/bin/bash
# 一键构建（板端原生或交叉编译环境均可，前提是 CMake + gcc 可用）
set -e
cd "$(dirname "$0")"
mkdir -p build
cd build
cmake ..
make -j"$(nproc)"
echo "== build done: $(pwd)/rk3568_intrusion =="
