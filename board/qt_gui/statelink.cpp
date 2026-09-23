// StateLink 实现：QTcpSocket 读行 → QJsonDocument 解析 → BoardState
#include "statelink.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTimer>

#include <cstdio>

StateLink::StateLink(QObject* parent) : QObject(parent) {
    QObject::connect(&sock_, &QTcpSocket::readyRead, this, [this] { onReadyRead(); });

    // 自动重连：主程序可能后启动或中途重启 → 每秒检查一次，断了就重连
    auto* t = new QTimer(this);
    QObject::connect(t, &QTimer::timeout, this, [this] {
        if (sock_.state() == QAbstractSocket::UnconnectedState) {
            sock_.abort();                       // 清掉半连接状态
            sock_.connectToHost(QStringLiteral("127.0.0.1"), port_);
        }
    });
    t->start(1000);
}

void StateLink::start(uint16_t port) {
    port_ = port;
    printf("[state] 连接状态通道 127.0.0.1:%u（未连上会每秒重试）\n", unsigned(port));
    sock_.connectToHost(QStringLiteral("127.0.0.1"), port_);
}

void StateLink::onReadyRead() {
    buf_ += sock_.readAll();
    int pos;
    while ((pos = buf_.indexOf('\n')) >= 0) {    // 一行一条 JSON
        const QByteArray line = buf_.left(pos);
        buf_.remove(0, pos + 1);
        if (!line.isEmpty()) parseLine(line);
    }
}

void StateLink::parseLine(const QByteArray& line) {
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) return;
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("type")).toString() != QStringLiteral("STATE")) return;

    BoardState s;
    s.valid = true;
    s.fw = o.value(QStringLiteral("fw")).toInt();
    s.fh = o.value(QStringLiteral("fh")).toInt();

    const QJsonArray roi = o.value(QStringLiteral("roi")).toArray();
    if (roi.size() == 4) {
        s.roi_x = roi.at(0).toInt();
        s.roi_y = roi.at(1).toInt();
        s.roi_w = roi.at(2).toInt();
        s.roi_h = roi.at(3).toInt();
    }
    s.stay_sec      = o.value(QStringLiteral("stay")).toInt();
    s.leave_confirm = o.value(QStringLiteral("leave")).toInt();
    s.conf          = o.value(QStringLiteral("conf")).toDouble();
    s.nms           = o.value(QStringLiteral("nms")).toDouble();
    s.log_level     = o.value(QStringLiteral("log")).toInt();
    s.report_on     = o.value(QStringLiteral("report")).toInt() != 0;
    s.fps           = o.value(QStringLiteral("fps")).toDouble();
    s.infer         = o.value(QStringLiteral("infer")).toInt();
    s.events        = o.value(QStringLiteral("events")).toInt();
    s.alarms        = o.value(QStringLiteral("alarms")).toInt();
    s.pushed        = o.value(QStringLiteral("pushed")).toVariant().toULongLong();
    s.dropped       = o.value(QStringLiteral("dropped")).toVariant().toULongLong();
    s.last_event    = o.value(QStringLiteral("ev")).toString();
    s.last_stay_ms  = o.value(QStringLiteral("es")).toVariant().toULongLong();

    const QJsonArray dets = o.value(QStringLiteral("dets")).toArray();
    for (const QJsonValue& v : dets) {
        const QJsonObject d = v.toObject();
        DetBox b;
        b.cls_id = d.value(QStringLiteral("c")).toInt();
        b.x1 = d.value(QStringLiteral("x1")).toDouble();
        b.y1 = d.value(QStringLiteral("y1")).toDouble();
        b.x2 = d.value(QStringLiteral("x2")).toDouble();
        b.y2 = d.value(QStringLiteral("y2")).toDouble();
        b.conf = d.value(QStringLiteral("p")).toDouble();
        s.dets.append(b);
    }

    st_ = s;            // 整体替换（只在 UI 线程，无需加锁）
    dirty_ = true;
}
