#include "intruder_gui.h"

#include <QApplication>
#include <QDateTime>
#include <QHBoxLayout>
#include <QImage>
#include <QPixmap>
#include <QScrollBar>
#include <QStatusBar>
#include <QVBoxLayout>

// ---- 轻量 JSON 字段提取（协议字段均为基础类型，无需引入 JSON 库）----
static QString jsonStr(const QString& s, const char* key) {
    const QString pat = "\"" + QString::fromLatin1(key) + "\":\"";
    int p = s.indexOf(pat);
    if (p < 0) return QString();
    p += pat.size();
    int e = s.indexOf('"', p);
    return (e < 0) ? QString() : s.mid(p, e - p);
}
static int jsonInt(const QString& s, const char* key, int dflt = 0) {
    const QString pat = "\"" + QString::fromLatin1(key) + "\":";
    int p = s.indexOf(pat);
    if (p < 0) return dflt;
    bool ok = false;
    int v = s.mid(p + pat.size(), 16).toInt(&ok);
    return ok ? v : dflt;
}

QString ServerWin::colorFor(const QString& type) const {
    if (type == "INTRUDE") return "#ffcc00";   // 黄：进入
    if (type == "ALARM")   return "#ff5555";   // 红：告警
    if (type == "LEAVE")   return "#9aa0a6";   // 灰：离开
    if (type == "RESOLVE") return "#3ddc84";   // 绿：解除
    if (type == "TEST")    return "#66ccff";
    return "#e0e0e0";
}

ServerWin::ServerWin(quint16 port, QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QString("入侵告警接收端  监听 0.0.0.0:%1").arg(port));
    resize(1020, 620);

    log_ = new QTextBrowser(this);
    log_->setOpenExternalLinks(false);
    log_->setStyleSheet("QTextBrowser{background:#121212;color:#e0e0e0;"
                        "border:1px solid #333;font-family:Consolas;font-size:12px;}");

    snapLabel_ = new QLabel(this);
    snapLabel_->setAlignment(Qt::AlignCenter);
    snapLabel_->setFixedSize(560, 315);          // 16:9 固定显示区
    snapLabel_->setText("暂无告警画面\n(摄像头触发 ALARM 后显示)");
    snapLabel_->setStyleSheet("background:#111;color:#666;border:2px solid #444;");

    statLabel_ = new QLabel(this);
    statLabel_->setStyleSheet("font-weight:bold;font-size:13px;");
    updateStats();

    connLabel_ = new QLabel("未连接", this);
    connLabel_->setStyleSheet("color:#999;");

    auto* right = new QVBoxLayout;
    right->addWidget(new QLabel("<b>最近告警画面</b>"));
    right->addWidget(snapLabel_, 1);
    right->addWidget(statLabel_);
    right->addWidget(connLabel_);

    auto* lay = new QHBoxLayout;
    lay->addWidget(log_, 3);
    lay->addLayout(right, 2);
    auto* cw = new QWidget(this);
    cw->setLayout(lay);
    setCentralWidget(cw);

    appendLog("<span style='color:#8ab4f8'>"
              + QString("已监听 0.0.0.0:%1，等待板端上报 ...").arg(port)
              + "</span>");

    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection,
            this, &ServerWin::onNewConnection);
    if (!server_->listen(QHostAddress::Any, port))
        appendLog("<span style='color:#ff5555'>监听失败: "
                  + server_->errorString() + "</span>");
}

void ServerWin::onNewConnection() {
    while (QTcpSocket* s = server_->nextPendingConnection()) {
        connect(s, &QTcpSocket::readyRead, this,
                [this, s] { onReadyRead(s); });
        connect(s, &QTcpSocket::disconnected, this, [this] {
            connLabel_->setText("已断开");
            connLabel_->setStyleSheet("color:#ff7777;");
        });
        connect(s, &QTcpSocket::disconnected, s, &QTcpSocket::deleteLater);
        connLabel_->setText("已连接: " + s->peerAddress().toString());
        connLabel_->setStyleSheet("color:#3ddc84;font-weight:bold;");
        appendLog("<span style='color:#9aa0a6'>客户端接入: "
                  + s->peerAddress().toString() + "</span>");
    }
}

void ServerWin::onReadyRead(QTcpSocket* s) {
    buf_ += QString::fromUtf8(s->readAll());
    int idx;
    while ((idx = buf_.indexOf('\n')) >= 0) {      // 按行解析(半包缓冲)
        QString line = buf_.left(idx).trimmed();
        buf_.remove(0, idx + 1);
        if (!line.isEmpty()) onLine(line);
    }
}

void ServerWin::onLine(const QString& raw) {
    QString type = jsonStr(raw, "type");
    if (type.isEmpty()) type = "INFO";

    if (type == "PREVIEW") {           // 现场预览帧：只刷新画面，不进日志(防 0.5s/条刷屏)
        showPreview(raw);
        return;
    }

    // 日志显示裁剪 base64 缩略图，避免整行 58KB 刷屏(画面见右侧)
    QString disp = raw;
    const QString imgKey = "\"img\":\"";
    int ip = disp.indexOf(imgKey);
    if (ip >= 0) {
        int ie = disp.indexOf('"', ip + imgKey.size());
        if (ie > 0) disp = disp.left(ip + imgKey.size()) + "...\"}";
    }

    appendLog("<span style='color:" + colorFor(type) + "'>"
              + QDateTime::currentDateTime().toString("HH:mm:ss.zzz") + "  "
              + disp.toHtmlEscaped() + "</span>");

    if (type == "INTRUDE")       { ++cIntrude_; }
    else if (type == "ALARM")    { ++cAlarm_;  showSnap(raw); }
    else if (type == "LEAVE")    { ++cLeave_; }
    else if (type == "RESOLVE")  { ++cResolve_; }
    updateStats();
}

// 解码事件消息里的 base64 RGB 缩略图
bool ServerWin::decodeThumb(const QString& raw, QImage& im) {
    int tw = jsonInt(raw, "thumb_w", 0), th = jsonInt(raw, "thumb_h", 0);
    QByteArray b = QByteArray::fromBase64(jsonStr(raw, "img").toLatin1());
    if (tw <= 0 || th <= 0) {
        appendLog("<span style='color:#ff5555'>[img 解码失败] 尺寸异常 tw="
                  + QString::number(tw) + " th=" + QString::number(th) + "</span>");
        return false;
    }
    if (b.size() < tw * th * 3) {
        appendLog("<span style='color:#ff5555'>[img 解码失败] 数据不足 bsize="
                  + QString::number(b.size()) + " 期望=" + QString::number(tw * th * 3)
                  + "</span>");
        return false;
    }
    im = QImage((const uchar*)b.constData(), tw, th, QImage::Format_RGB888).copy();
    return true;
}

void ServerWin::paintThumb(const QImage& im, const QString& border) {
    QPixmap pm = QPixmap::fromImage(im);
    snapLabel_->setPixmap(pm.scaled(snapLabel_->size(),
                                    Qt::KeepAspectRatio, Qt::SmoothTransformation));
    snapLabel_->setStyleSheet("background:#000;border:3px solid " + border + ";");
}

// ALARM 消息：告警画面(红框高亮 + 状态栏提示)
void ServerWin::showSnap(const QString& raw) {
    QImage im;
    if (!decodeThumb(raw, im)) return;
    lastSnap_ = im;
    paintThumb(im, "#ff3333");
    statusBar()->showMessage(QString("⚠ 告警! 类别 %1 · 停留 %2 ms")
                             .arg(jsonInt(raw, "cls", -1))
                             .arg(jsonInt(raw, "stay_ms")), 6000);
}

// 周期性现场预览帧：持续刷新右侧画面(准实时视频观感，绿色边框=正常监视)
void ServerWin::showPreview(const QString& raw) {
    QImage im;
    if (!decodeThumb(raw, im)) return;
    lastSnap_ = im;
    paintThumb(im, "#1f7a3d");
}

void ServerWin::appendLog(const QString& html) {
    log_->append(html);
    log_->verticalScrollBar()->setValue(log_->verticalScrollBar()->maximum());
}

void ServerWin::updateStats() {
    if (!statLabel_) return;
    statLabel_->setText(QString("进入 %1   告警 %2   离开 %3   解除 %4")
                        .arg(cIntrude_).arg(cAlarm_).arg(cLeave_).arg(cResolve_));
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    quint16 port = 9000;
    if (argc > 1) port = QString(argv[1]).toUShort();
    ServerWin w(port);
    w.show();
    return app.exec();
}
