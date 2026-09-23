cmake_minimum_required(VERSION 3.10)
project(rk3568_intrusion CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------- 依赖 ----------------
# RKNN runtime（D4+ 必需）：SDK/交叉环境提供 librknnrt + rknn_api.h。
# 交叉编译时显式传入：
#   -DRKNN_INCLUDE_DIR=<含 rknn_api.h 的目录> -DRKNN_LIB_DIR=<含 librknnrt.so 的目录>
if(NOT RKNN_INCLUDE_DIR)
    find_path(RKNN_INCLUDE_DIR rknn_api.h
        PATHS /usr/include/rknn /usr/local/include/rknn /oem/usr/include /usr/include)
endif()
if(NOT RKNN_LIB_DIR)
    find_path(RKNN_LIB_DIR librknnrt.so
        PATHS /usr/lib /usr/lib64 /usr/lib/aarch64-linux-gnu /oem/usr/lib)
endif()
message(STATUS "RKNN_INCLUDE_DIR=${RKNN_INCLUDE_DIR}")
message(STATUS "RKNN_LIB_DIR=${RKNN_LIB_DIR}")

# ---------------- 源文件（显式列出，新增 .cpp 记得加进来）----------------
set(SRC
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${CMAKE_SOURCE_DIR}/src/common/config.cpp
    ${CMAKE_SOURCE_DIR}/src/common/console.cpp
    ${CMAKE_SOURCE_DIR}/src/capture/v4l2_camera.cpp
    ${CMAKE_SOURCE_DIR}/src/infer/rknn_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/business/roi_monitor.cpp
    ${CMAKE_SOURCE_DIR}/src/output/reporter.cpp
    ${CMAKE_SOURCE_DIR}/src/output/streamer.cpp
)

# ---------------- 可选：H.264 推流（GStreamer appsrc → mpph264enc）----------------
# 默认关：不推流时产物不依赖任何 gstreamer 库，行为与以前完全一致。
# 交叉编译开启（路径指向 SDK 的 staging/usr，无需板端 .pc）：
#   cmake .. -DENABLE_STREAM=ON \
#            -DGSTREAMER_ROOT=$HOME/rk3568_linux_sdk/buildroot/output/rockchip_rk3568/staging/usr
# 板端原生编译开启：-DENABLE_STREAM=ON（此时走本机 pkg-config）
option(ENABLE_STREAM "启用 GStreamer H.264 推流" OFF)
if(ENABLE_STREAM AND GSTREAMER_ROOT)
    link_directories(${GSTREAMER_ROOT}/lib)   # 用 link_directories 而非 target_link_directories(需 CMake 3.13+)
endif()

add_executable(rk3568_intrusion ${SRC})

target_include_directories(rk3568_intrusion PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${RKNN_INCLUDE_DIR}
)

target_link_libraries(rk3568_intrusion PRIVATE
    pthread
    rknnrt
)

if(ENABLE_STREAM)
    target_compile_definitions(rk3568_intrusion PRIVATE ENABLE_STREAM=1)
    if(GSTREAMER_ROOT)
        message(STATUS "推流: 用 SDK staging = ${GSTREAMER_ROOT}")
        # 交叉编译不走 pkg-config：staging 里的 .pc 前缀指向板端绝对路径，会让链接器找错地方
        target_include_directories(rk3568_intrusion PRIVATE
            ${GSTREAMER_ROOT}/include/gstreamer-1.0
            ${GSTREAMER_ROOT}/include/glib-2.0
            ${GSTREAMER_ROOT}/lib/glib-2.0/include   # glibconfig.h 在这里
            ${GSTREAMER_ROOT}/include                # ffi.h 等
        )
        target_link_libraries(rk3568_intrusion PRIVATE
            gstreamer-1.0 gstapp-1.0 gstbase-1.0 gobject-2.0 glib-2.0)
    else()
        message(STATUS "推流: 用本机 pkg-config")
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0)
        target_include_directories(rk3568_intrusion PRIVATE ${GST_INCLUDE_DIRS})
        target_link_directories(rk3568_intrusion PRIVATE ${GST_LIBRARY_DIRS})
        target_link_libraries(rk3568_intrusion PRIVATE ${GST_LIBRARIES})
    endif()
    message(STATUS "推流: 已启用 (-DENABLE_STREAM=ON)")
endif()

# 告警截图/事件日志等运行时目录统一落在 build 同级的 shots/logs（见配置）
