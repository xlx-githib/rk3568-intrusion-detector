// 板端 Qt 最小验证程序（迭代3 第 1 步）
//
// 目的：确认三件事
//   1) Qt 能在板子的 linuxfb 上真的出画面（fb0 是 DRM 模拟帧缓冲，不一定写得进）
//   2) 屏幕分辨率/方向对不对（1080x1920 还是 1920x1080、有没有被裁）
//   3) 纯 CPU(raster) 刷新能跑多少帧 —— 决定后面视频区能做到多大/多少帧
//
// 编译：bash build.sh            运行：QT_QPA_PLATFORM=linuxfb ./rkqttest
//
// ⚠️ 本文件刻意不使用 Q_OBJECT：自定义信号槽全用 lambda 连接，
//    因此编译**不需要 moc 工具**（SDK 里没编 host qmake/moc）。后面 UI 若要用自定义信号，
//    再单独找 moc 或改回 lambda 连接。
#include <QApplication>
#include <QElapsedTimer>
#include <QFont>
#include <QPainter>
#include <QTimer>
#include <QWidget>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace {
// 信号处理器里只能调 async-signal-safe 的函数（printf/quit 都不算）。
// 之前只设标志位、靠 QTimer 去查，实测 ^C 退不出来（DRM/linuxfb 下事件循环没能及时处理）
// → 直接 _exit()，简单可靠。
void on_sigint(int) { _exit(0); }
}  // namespace

class TestWidget : public QWidget {
public:
    TestWidget() {
        et_.start();
        // ~30fps 请求重绘：能不能真的画出 30 帧，看终端每秒打印的实测值
        auto* t = new QTimer(this);
        QObject::connect(t, &QTimer::timeout, this, [this] { ++tick_; update(); });
        t->start(33);
        // 每秒报一次真实刷新率
        auto* t2 = new QTimer(this);
        QObject::connect(t2, &QTimer::timeout, this, [this] {
            double sec = et_.elapsed() / 1000.0;
            int d = paints_ - last_paints_;
            last_paints_ = paints_;
            fps_now_ = sec > 0 ? paints_ / sec : 0.0;
            printf("[qttest] 重绘累计 %d 次 / %.0fs → 平均 %.1f fps（最近1秒 %d 次）| 窗口 %dx%d\n",
                   paints_, sec, fps_now_, d, width(), height());
            fflush(stdout);
        });
        t2->start(1000);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        ++paints_;
        QPainter p(this);
        const int W = width(), H = height();
        p.fillRect(rect(), QColor(14, 14, 20));

        // ① 外框 + 十字线：确认整块屏幕都被画到（边缘没被裁说明分辨率/尺寸对）
        p.setPen(QPen(QColor(0, 200, 255), 6));
        p.drawRect(3, 3, W - 7, H - 7);
        p.drawLine(W / 2, 0, W / 2, H);
        p.drawLine(0, H / 2, W, H / 2);

        // ② 色条：验证 RGB 顺序没搞反（应显示 白 红 绿 蓝 黄）
        const QColor bars[5] = {Qt::white, Qt::red, Qt::green, Qt::blue, Qt::yellow};
        for (int i = 0; i < 5; ++i)
            p.fillRect(30 + i * 130, 30, 120, 90, bars[i]);

        // ③ 移动方块：肉眼看流畅度
        int bw = W / 6;
        int x = int((long(tick_) * 12) % (W - bw));
        p.fillRect(x, H * 2 / 3, bw, bw, QColor(0, 255, 120));

        // ④ 文字（若板端没装字体则不会显示 —— 属正常，看色条/方块即可）
        p.setPen(Qt::white);
        QFont f = p.font();
        f.setPointSize(34);
        f.setBold(true);
        p.setFont(f);
        p.drawText(30, H / 4, QStringLiteral("RK3568  Qt5 + linuxfb"));
        p.drawText(30, H / 4 + 60, QStringLiteral("size %1x%2").arg(W).arg(H));
        p.drawText(30, H / 4 + 120, QStringLiteral("avg fps %1").arg(fps_now_, 0, 'f', 1));
        p.drawText(30, H / 4 + 180, QStringLiteral("色条应为 白 红 绿 蓝 黄"));
    }

private:
    QElapsedTimer et_;
    int tick_ = 0;
    int paints_ = 0;
    int last_paints_ = 0;
    double fps_now_ = 0.0;
};

int main(int argc, char** argv) {
    signal(SIGINT, on_sigint);       // linuxfb 没有窗口管理器，Ctrl+C 自己接管
    QApplication app(argc, argv);
    printf("[qttest] Qt=%s  platform=%s\n", qVersion(),
           QApplication::platformName().toUtf8().constData());
    printf("[qttest] 提示：若无文字但有图形，那是板端字体问题；看色条/方块即可\n");
    fflush(stdout);

    TestWidget w;
    w.showFullScreen();
    return app.exec();
}
