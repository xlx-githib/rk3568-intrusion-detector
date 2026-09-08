#include "intruder_gui.h"

#include <QApplication>
#include <QDateTime>
#include <QVBoxLayout>

ServerWin::ServerWin(quint16 port, QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QString("入侵告警接收端  监听 0.0.0.0:%1").arg(port));
    resize(680, 480);

    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    setCentralWidget(log_);

    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection,
            this, &ServerWin::onNewConnection);
    if (!server_->listen(QHostAddress::Any, port)) {
        append("监听失败: " + server_->errorString());
    } else {
        append(QString("已监听 0.0.0.0:%1，等待板端上报 ...").arg(port));
    }
}

void ServerWin::onNewConnection() {
    while (QTcpSocket* s = server_->nextPendingConnection()) {
        connect(s, &QTcpSocket::readyRead, this,
                [this, s] { onReadyRead(s); });
        connect(s, &QTcpSocket::disconnected, s, &QTcpSocket::deleteLater);
        append(QString("客户端接入: %1").arg(s->peerAddress().toString()));
    }
}

void ServerWin::onReadyRead(QTcpSocket* s) {
    buf_ += QString::fromUtf8(s->readAll());
    int idx;
    while ((idx = buf_.indexOf('\n')) >= 0) {      // 按行解析
        QString line = buf_.left(idx).trimmed();
        buf_.remove(0, idx + 1);
        if (!line.isEmpty()) onLine(line);
    }
}

void ServerWin::onLine(const QString& line) {
    // 目前先原样显示 JSON；M3 扩展为事件列表/告警图/统计
    append(QDateTime::currentDateTime().toString("HH:mm:ss.zzz") + "  " + line);
}

void ServerWin::append(const QString& text) {
    log_->appendPlainText(text);
    log_->moveCursor(QTextCursor::End);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    quint16 port = 9000;
    if (argc > 1) port = QString(argv[1]).toUShort();
    ServerWin w(port);
    w.show();
    return app.exec();
}
