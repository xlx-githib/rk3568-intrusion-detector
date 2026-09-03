cmake_minimum_required(VERSION 3.10)
project(rk3568_intrusion CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------- 依赖（逐步接入时取消注释）----------------
# 1) RKNN runtime：板上 SDK 提供 librknnrt.so 与 rknn_api.h
# set(RKNN_RUNTIME_DIR "/usr/lib/aarch64-linux-gnu")
# set(RKNN_INCLUDE_DIR "/usr/include/rknn")

# 2) OpenCV：用于画框/显示（若板端有）；也可用 RGA/自绘替代
# find_package(OpenCV REQUIRED)

# ---------------- 源文件 ----------------
file(GLOB_RECURSE SRC
    ${CMAKE_SOURCE_DIR}/src/*.cpp
)

add_executable(rk3568_intrusion ${SRC})

target_include_directories(rk3568_intrusion PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    # ${RKNN_INCLUDE_DIR}
)

target_link_libraries(rk3568_intrusion PRIVATE
    pthread
    # rknnrt
    # ${OpenCV_LIBS}
)

# 告警截图/事件日志等运行时目录统一落在 build 同级的 shots/logs（见配置）
