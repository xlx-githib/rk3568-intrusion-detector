#ifndef INTRUDER_GUI_H
#define INTRUDER_GUI_H

#include <QMainWindow>
#include <QPlainTextEdit>
#include <QTcpServer>
#include <QTcpSocket>
#include <QString>

// PC 端 Qt 接收上位机（M3 目标；当前为最小可运行版：
// 监听端口 → 逐行显示收到的 JSON 事件，跨机联调用）
class ServerWin : public QMainWindow {
    Q_OBJECT
public:
    explicit ServerWin(quint16 port, QWidget* parent = nullptr);

private slots:
    void onNewConnection();
    void onReadyRead(QTcpSocket* s);
    void onLine(const QString& line);

private:
    void append(const QString& text);
    QPlainTextEdit* log_ = nullptr;
    QTcpServer*     server_ = nullptr;
    QString         buf_;
};

#endif // INTRUDER_GUI_H
