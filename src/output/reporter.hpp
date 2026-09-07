#pragma once
// 输出层：事件 JSON over TCP 上报 PC（M2-4；截图仍由调用方用 draw_boxes 完成）
// 协议：与 host/receiver.py 对齐 —— 每行一条 JSON
#include <string>

#include "business/roi_monitor.hpp"

class Reporter {
public:
    void init(bool enable_report, const std::string& server_ip, int port);
    bool connect();                 // 建立 TCP 连接
    void disconnect();
    // 组 JSON {type,cls,ts_start_ms,stay_ms,snapshot} 并发送；断线自动重连一次
    bool report(const Event& e, const char* snapshot = nullptr);

private:
    bool try_send(const std::string& s);

    int fd_ = -1;
    bool enabled_ = false;
    std::string ip_;
    int port_ = 9000;
};

