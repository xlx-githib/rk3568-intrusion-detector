#!/bin/bash
# 板端 Qt 应用交叉编译（不用 qmake：SDK 没编 host qmake，但 staging 里有完整 Qt5 头文件+库）
#
# 用法：bash build.sh            产物：./rkqttest
#
# ⚠️ 关键坑（与主程序同一套经验）：
#   不要把 $S/lib 加进 -L！staging/usr/lib 里有 glibc 的链接脚本 libc.so，
#   内容是板端绝对路径 GROUP( /lib64/libc.so.6 ... )；-L 一旦命中它，
#   ld 就会去宿主根目录找 /lib64/libc.so.6 → 报错。
#   正确：用绝对路径点名链接 Qt 的 .so，另用 -rpath-link 解析它们的依赖。
set -e
cd "$(dirname "$0")"

SDK="$HOME/rk3568_linux_sdk/buildroot/output/rockchip_rk3568"
CXX="$SDK/host/bin/aarch64-buildroot-linux-gnu-g++"
S="$SDK/staging/usr"
OUT="${1:-rkqttest}"

if [ ! -x "$CXX" ]; then
    echo "找不到交叉编译器: $CXX"; exit 1
fi
if [ ! -f "$S/lib/libQt5Widgets.so" ]; then
    echo "找不到 Qt5 库: $S/lib/libQt5Widgets.so（SDK staging 里没有 Qt 开发文件？）"; exit 1
fi

QT_INC="-I$S/include/qt5 -I$S/include/qt5/QtCore -I$S/include/qt5/QtGui -I$S/include/qt5/QtWidgets -I$S/include/qt5/QtNetwork"
QT_LIB="$S/lib/libQt5Widgets.so $S/lib/libQt5Gui.so $S/lib/libQt5Core.so $S/lib/libQt5Network.so"
# GStreamer：拉流 + mppvideodec 硬解（同样不能把 $S/lib 加进 -L）
GST_INC="-I$S/include/gstreamer-1.0 -I$S/include/glib-2.0 -I$S/lib/glib-2.0/include"
GST_LIB="$S/lib/libgstreamer-1.0.so $S/lib/libgstapp-1.0.so $S/lib/libgobject-2.0.so $S/lib/libglib-2.0.so"
LDFLAGS="-Wl,-rpath-link,$S/lib"

SRCS="main.cpp gstsource.cpp statelink.cpp"

echo "== 编译 $OUT =="
# -fPIC：Qt 头文件里可能有内联的 PIC 相关代码；-O2：性能验证才有意义
"$CXX" -std=c++17 -fPIC -O2 -Wall \
    $QT_INC $GST_INC \
    $SRCS -o "$OUT" \
    $QT_LIB $GST_LIB \
    $LDFLAGS \
    -lpthread -ldl

echo "== done: $(pwd)/$OUT =="
echo "推板:  adb push $OUT /userdata/aidemo/"
echo "运行:  cd /userdata/aidemo && export XDG_RUNTIME_DIR=/run && QT_QPA_PLATFORM=wayland ./$OUT"
