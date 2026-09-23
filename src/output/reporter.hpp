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
    // 建立 TCP 连接；timeout_ms 是**我们自己的**上限（0=只探测一次立即返回）
    // ⚠️ 不能直接用阻塞 connect：PC 端没开监听时内核会 SYN 重试几十秒，
    //    把启动流程/输出线程卡死（推流启动都被拖在后面）
    bool connect(int timeout_ms = 2000);
    void disconnect();
    // 组 JSON {type,cls,ts_start_ms,stay_ms,snapshot} 并发送；断线自动重连一次
    bool report(const Event& e, const char* snapshot = nullptr);
    bool connected() const { return fd_ >= 0; }   // 是否已建立 TCP 连接
    // ALARM 附带缩略图(RGB888 tw×th)：JSON 增 thumb_w/thumb_h/img(base64)，PC Qt 端可显示告警画面
    bool reportImg(const Event& e, const char* snapshot,
                   int tw, int th, const vector<uint8_t>& rgb);
    // 周期性现场预览帧(准实时画面)：type=PREVIEW，Qt 端持续刷新显示
    bool reportPreview(int tw, int th, const vector<uint8_t>& rgb);

private:
    bool try_send(const string& s);
    // 未连接时尝试重连（带冷却，避免每个预览帧都花时间在 connect 上）
    bool ensure_connected();
    bool build_json(const Event& e, const char* snapshot,
                    int tw, int th, const vector<uint8_t>* rgb, string& out);

    int fd_ = -1;
    bool enabled_ = false;
    string ip_;
    int port_ = 9000;
    uint64_t last_conn_try_ms_ = 0;   // 上次尝试连接的时刻（steady 毫秒）
};

