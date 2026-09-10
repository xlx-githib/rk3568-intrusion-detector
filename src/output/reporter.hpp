#pragma once
// 输出层：事件 JSON over TCP 上报 PC（M2-4；截图仍由调用方用 draw_boxes 完成）
// 协议：与 host/receiver.py 对齐 —— 每行一条 JSON
#include <cstdint>
#include <string>
#include <vector>

#include "business/roi_monitor.hpp"

// 标准库名字逐个引入(头文件不用 using namespace std，避免污染包含者)
using std::string;
using std::vector;

class Reporter {
public:
    void init(bool enable_report, const string& server_ip, int port);
    bool connect();                 // 建立 TCP 连接
    void disconnect();
    // 组 JSON {type,cls,ts_start_ms,stay_ms,snapshot} 并发送；断线自动重连一次
    bool report(const Event& e, const char* snapshot = nullptr);
    // ALARM 附带缩略图(RGB888 tw×th)：JSON 增 thumb_w/thumb_h/img(base64)，PC Qt 端可显示告警画面
    bool reportImg(const Event& e, const char* snapshot,
                   int tw, int th, const vector<uint8_t>& rgb);
    // 周期性现场预览帧(准实时画面)：type=PREVIEW，Qt 端持续刷新显示
    bool reportPreview(int tw, int th, const vector<uint8_t>& rgb);

private:
    bool try_send(const string& s);
    bool build_json(const Event& e, const char* snapshot,
                    int tw, int th, const vector<uint8_t>* rgb, string& out);

    int fd_ = -1;
    bool enabled_ = false;
    string ip_;
    int port_ = 9000;
};

