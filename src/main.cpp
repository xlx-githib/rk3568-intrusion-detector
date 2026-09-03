// 程序入口 / 流水线组装（骨架版）
// 当前只加载配置并打印；管线模块按 docs/architecture.md 分阶段接入：
//   D2/D3 读官方 C demo -> D4/D5 业务层(视频文件闭环)
//   -> D6/D7 V4L2+多线程 -> D8/D10 状态机完善 + socket 上报
#include <string>

#include "common/config.hpp"
#include "common/log.hpp"

int main(int argc, char** argv) {
    const std::string cfg_path = (argc > 1) ? argv[1] : "config/intruder.conf";

    Config cfg;
    if (!Config::load(cfg_path, cfg)) {
        LOG_E("load config failed: %s", cfg_path.c_str());
        return -1;
    }

    LOG_I("=== RK3568 Intrusion Detector (skeleton) ===");
    LOG_I("input_type = %s",   cfg.get("input_type").c_str());
    LOG_I("model      = %s",   cfg.get("model_path").c_str());
    LOG_I("input_size = %d",   cfg.getInt("model_input_size"));
    LOG_I("stay_alarm = %d s", cfg.getInt("stay_alarm_sec"));

    // TODO(D7): 采集线程(V4L2) -> FrameQueue(raw) -> 推理线程(RKNN)
    //         -> FrameQueue(result) -> 业务线程(ROI 状态机) -> 输出线程(截图/socket)
    LOG_I("skeleton OK. 管线模块待接入，见 docs/architecture.md");
    return 0;
}
