// 板端 Qt 应用（迭代3）
//
// Step 1（已完成）：linuxfb/wayland 能不能出画面、屏幕方向/分辨率、CPU 刷新帧率
//   → 结论：板子跑的是 Weston，**必须当 Wayland 客户端**（QT_QPA_PLATFORM=wayland）；
//     直接抢 DRM(fb0) 会被 weston 抢回屏幕（只亮一下）
// Step 2（本文件当前版本）：把视频接进来
//   tcpclientsrc ! tsdemux ! h264parse ! mppvideodec(硬解) ! videoscale ! videoconvert
//     ! video/x-raw,RGB ! appsink  →  最新一帧 → QImage → 画到窗口
//
// 编译：bash build.sh        运行：QT_QPA_PLATFORM=wayland ./rkqttest [端口=5000]
//
// ⚠️ 本工程刻意不使用 Q_OBJECT（不用自定义 signal/slot，一律 lambda 连接），
//    这样编译**不需要 moc** —— SDK 里没编 host qmake/moc。
#include <QApplication>
#include <QElapsedTimer>
#include <QFont>
#include <QFontInfo>
#include <QImage>
#include <QPainter>
#include <QTimer>
#include <QWidget>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "gstsource.hpp"

namespace {
// 信号处理器里只能调 async-signal-safe 的函数（printf/quit 都不算）。
// 之前只设标志位、靠 QTimer 去查，实测 ^C 退不出来 → 直接 _exit()，简单可靠。
void on_sigint(int) { _exit(0); }
}  // namespace

class TestWidget : public QWidget {
public:
    TestWidget(GstSource* src, uint16_t port) : src_(src), port_(port) {
        et_.start();
        // 10ms 轮询取帧（远快于 30fps 的产帧速度）→ 每次都能拿到最新帧；
        // 而且**只有取到新帧才重绘**：没帧时不空转重绘，省 CPU（实测能把“顶掉”降下来）
        auto* t = new QTimer(this);
        QObject::connect(t, &QTimer::timeout, this, [this] {
            ++tick_;
            QImage img;
            if (src_->takeFrame(img)) { frame_ = img; ++got_; update(); }
            else if (frame_.isNull()) update();     // 还没画面时让占位动画继续动
        });
        t->start(10);
        // 每秒报一次真实刷新率 + 收帧情况
        auto* t2 = new QTimer(this);
        QObject::connect(t2, &QTimer::timeout, this, [this] {
            double sec = et_.elapsed() / 1000.0;
            int d = paints_ - last_paints_;
            last_paints_ = paints_;
            fps_now_ = sec > 0 ? paints_ / sec : 0.0;
            printf("[qttest] UI 重绘 %.1f fps（最近1秒 %d 次）| 解码收帧 %llu 显示 %llu 顶掉 %llu | 窗口 %dx%d\n",
                   fps_now_, d, (unsigned long long)src_->received(),
                   (unsigned long long)got_, (unsigned long long)src_->dropped(),
                   width(), height());
            fflush(stdout);
        });
        t2->start(1000);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        ++paints_;
        QPainter p(this);
        const int W = width(), H = height();
        p.fillRect(rect(), QColor(10, 10, 14));

        // ---- 视频区：16:9，居中偏上（竖屏时按高度限制） ----
        int vw = W, vh = W * 9 / 16;
        if (vh > H * 3 / 5) { vh = H * 3 / 5; vw = vh * 16 / 9; }
        const QRect vr((W - vw) / 2, (H - vh) / 3, vw, vh);
        p.fillRect(vr, Qt::black);
        if (!frame_.isNull()) {
            // 解码输出(640x360) → 视频区缩放。SmoothPixmapTransform 好看但费 CPU，卡就改成 false
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.drawImage(vr, frame_);
        } else {
            // 无画面：提示 + 移动方块（方块在动 = UI 活着，问题在流那边）
            p.setPen(QColor(255, 200, 0));
            p.setFont(fontFor(28));
            p.drawText(vr.adjusted(20, 0, -20, 0), Qt::AlignCenter,
                       QStringLiteral("等待视频流 tcp://127.0.0.1:%1").arg(port_));
            int bw = qMax(40, vw / 10);
            int x = vr.x() + 20 + int((long(tick_) * 10) % qMax(1, vr.width() - bw - 40));
            p.fillRect(x, vr.bottom() - bw - 20, bw, bw, QColor(0, 255, 120));
        }
        p.setPen(QPen(QColor(0, 200, 255), 4));
        p.drawRect(vr);

        // ---- 状态行 ----
        p.setPen(Qt::white);
        p.setFont(fontFor(26));
        const int y = vr.bottom() + 60;
        p.drawText(30, y, QStringLiteral("屏幕 %1x%2    UI %3 fps")
                              .arg(W).arg(H).arg(fps_now_, 0, 'f', 1));
        p.drawText(30, y + 46, QStringLiteral("解码收帧 %1   显示 %2   顶掉 %3")
                                   .arg(static_cast<qulonglong>(src_->received()))
                                   .arg(static_cast<qulonglong>(got_))
                                   .arg(static_cast<qulonglong>(src_->dropped())));
        if (frame_.isNull()) {
            p.setPen(QColor(255, 120, 120));
            p.drawText(30, y + 92, QStringLiteral("还没收到画面：确认主程序带 STREAM_PORT=5000 在跑"));
        }
    }

private:
    // 板上实测：不指定族名时 Qt 的 fallback 会失败（load glyph failed err=24），一个字都画不出
    static QFont fontFor(int pt) {
        QFont f(QStringLiteral("Liberation Sans"));
        if (!QFontInfo(f).family().contains(QStringLiteral("Liberation"), Qt::CaseInsensitive))
            f = QFont(QStringLiteral("DejaVu Sans"));
        f.setPointSize(pt);
        f.setBold(true);
        return f;
    }

    GstSource* src_ = nullptr;
    uint16_t port_ = 0;
    QElapsedTimer et_;
    QImage frame_;
    int tick_ = 0;
    int paints_ = 0;
    int last_paints_ = 0;
    double fps_now_ = 0.0;
    uint64_t got_ = 0;        // UI 实际取到的帧数
};

int main(int argc, char** argv) {
    signal(SIGINT, on_sigint);       // Wayland/linuxfb 都没有窗口管理器，Ctrl+C 自己接管
    QApplication app(argc, argv);

    // 端口可用参数覆盖（默认 5000，与主程序 STREAM_PORT 对齐）
    uint16_t port = 5000;
    if (argc > 1) { int v = atoi(argv[1]); if (v > 0) port = uint16_t(v); }

    printf("[qttest] Qt=%s  platform=%s  推流端口=%u\n", qVersion(),
           QApplication::platformName().toUtf8().constData(), unsigned(port));
    fflush(stdout);

    GstSource src;
    if (!src.start(port, 640, 360))
        printf("[qttest] 视频源启动失败: %s（界面会显示等待提示，不影响 UI 自检）\n",
               src.lastError().c_str());

    TestWidget w(&src, port);
    w.showFullScreen();
    int rc = app.exec();
    src.stop();
    return rc;
}
