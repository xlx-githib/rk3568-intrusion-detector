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
    ${CMAKE_SOURCE_DIR}/src/infer/rknn_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/business/roi_monitor.cpp
)

add_executable(rk3568_intrusion ${SRC})

target_include_directories(rk3568_intrusion PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${RKNN_INCLUDE_DIR}
)

target_link_libraries(rk3568_intrusion PRIVATE
    pthread
    rknnrt
)

# 告警截图/事件日志等运行时目录统一落在 build 同级的 shots/logs（见配置）
