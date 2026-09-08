#ifndef INTRUDER_GUI_H
#define INTRUDER_GUI_H

#include <QMainWindow>
#include <QTextBrowser>
#include <QLabel>
#include <QTcpServer>
#include <QTcpSocket>
#include <QImage>
#include <QString>

// PC 端 Qt 接收上位机：监听板端事件 JSON → 分色事件日志 + 最近告警画面 + 统计
// 协议对齐 src/output/reporter.cpp：每行一条 JSON，
// ALARM 消息带 thumb_w/thumb_h/img(base64 RGB) 用于显示告警截图
class ServerWin : public QMainWindow {
    Q_OBJECT
public:
    explicit ServerWin(quint16 port, QWidget* parent = nullptr);

private slots:
    void onNewConnection();
    void onReadyRead(QTcpSocket* s);

private:
    void onLine(const QString& raw);       // 解析一行 JSON
    void appendLog(const QString& html);   // 分色追加日志
    void showSnap(const QString& raw);     // ALARM 缩略图显示到右侧
    void updateStats();
    QString colorFor(const QString& type) const;

    QTextBrowser* log_ = nullptr;          // 事件日志(分色)
    QLabel*       snapLabel_ = nullptr;    // 最近告警画面
    QLabel*       statLabel_ = nullptr;    // 事件统计
    QLabel*       connLabel_ = nullptr;    // 连接状态

    QTcpServer* server_ = nullptr;
    QString     buf_;                      // 半包缓冲

    int cIntrude_ = 0, cAlarm_ = 0, cLeave_ = 0, cResolve_ = 0;
    QImage lastSnap_;
};

#endif // INTRUDER_GUI_H
