#pragma once
// 板端状态接收：连主程序的 127.0.0.1:9100，逐行读 JSON 状态（ROI/检测框/统计）
//
// - 用 QTcpSocket（Qt 事件驱动）：不需要额外线程，readyRead 回调就在 UI 线程，
//   所以取状态时不用加锁（只有 UI 线程碰 st_）
// - 解析用 QJsonDocument：比手写字符串解析可靠得多
// - 连不上会自动重连（主程序可能后启动、或中途重启），每秒试一次
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QVector>

struct DetBox {
    int cls_id = 0;                     // 0=person, 2=car（主程序的类别白名单）
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    double conf = 0;
};

// 一次状态快照（坐标系 = fw x fh，通常是 1280x720）
struct BoardState {
    bool valid = false;                 // 是否收到过至少一条状态
    int fw = 0, fh = 0;
    int roi_x = 0, roi_y = 0, roi_w = 0, roi_h = 0;
    int stay_sec = 3, leave_confirm = 5;
    double conf = 0.25, nms = 0.45;
    int log_level = 1;
    bool report_on = true;
    double fps = 0;
    int infer = 0, events = 0, alarms = 0;
    unsigned long long pushed = 0, dropped = 0;
    QString last_event;                 // 最近一条事件（空=还没有）
    unsigned long long last_stay_ms = 0;
    QVector<DetBox> dets;
};

class StateLink : public QObject {
public:
    explicit StateLink(QObject* parent = nullptr);
    void start(uint16_t port = 9100);

    const BoardState& state() const { return st_; }
    bool connected() const { return sock_.state() == QAbstractSocket::ConnectedState; }
    // 向上发一条控制命令（一行文本，如 "roi 300 200 600 400"）——
    // 主程序侧由 StatLink::takeCommand 取出，交给运行时控制台执行
    bool sendCommand(const QString& cmd);
    // 有新状态（UI 用它决定要不要重绘）
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

private:
    void onReadyRead();
    void parseLine(const QByteArray& line);

    QTcpSocket sock_;
    QByteArray buf_;                    // 半行缓冲
    BoardState st_;
    bool dirty_ = false;
    uint16_t port_ = 9100;
};
