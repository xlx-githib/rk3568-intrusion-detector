// 板端 Qt 应用（迭代3）
//
// Step 1（完成）：确认必须当 **Wayland 客户端**（板子跑的是 Weston；抢 DRM 会被抢回去）
// Step 2（完成）：视频接入 —— tcpclientsrc ! tsdemux ! h264parse ! mppvideodec(硬解)
//                        ! videoscale ! videoconvert ! video/x-raw,RGB ! appsink
//                        → 最新一帧 → QImage → 画到视频区
// Step 3a（完成）：横屏布局 + 叠加显示
//   - 左：视频区（16:9，ROI 黄框 + 人/车检测框叠加）
//   - 右：信息栏（连接状态/帧率/统计/参数）+ 最近事件列表
//   数据来源：主程序的 127.0.0.1:9100 状态通道（StateLink，JSON 行）
// Step 3b（本文件当前版本）：**触摸拖动调 ROI**
//   - 视频区 ROI 框四角有拖拽柄：拖角=缩放，拖框内=整体移动
//   - 拖动中本地实时显示（跟手），**松手才发一条 `roi x y w h` 给主程序**
//   - 主程序交给运行时控制台执行 → 与应用命令复用同一套热更新链路
//
// 编译：bash build.sh
// 运行：export XDG_RUNTIME_DIR=/run && QT_QPA_PLATFORM=wayland ./rkqttest [视频端口=5000] [状态端口=9100] [解码宽度=640]
//
// ⚠️ 本工程刻意不使用 Q_OBJECT（不用自定义 signal/slot，一律 lambda 连接），
//    这样编译**不需要 moc** —— SDK 里没编 host qmake/moc。
#include <QApplication>
#include <QElapsedTimer>
#include <QFont>
#include <QFontInfo>
#include <QFontMetrics>
#include <QImage>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QTime>
#include <QTimer>
#include <QTouchEvent>
#include <QVector>
#include <QWidget>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "gstsource.hpp"
#include "statelink.hpp"

namespace {
// 信号处理器里只能调 async-signal-safe 的函数（printf/quit 都不算）。
// 之前只设标志位、靠 QTimer 去查，实测 ^C 退不出来 → 直接 _exit()，简单可靠。
void on_sigint(int) { _exit(0); }

// 环境变量开关（非空且不是 "0" 即为开）
bool envOn(const char* key) {
    const char* v = getenv(key);
    return v && v[0] && v[0] != '0';
}
}  // namespace

class MainWindow : public QWidget {
public:
    MainWindow(GstSource* src, StateLink* link, uint16_t port)
        : src_(src), link_(link), port_(port) {
        // 诊断开关（定位“UI 为什么慢”用，效果看 README 第 8 节）：
        //   SKIP_VIDEO=1    不画视频，只画背景+右栏  → 看 fps 跳不跳
        //   SKIP_SIDEBAR=1  不画右栏                → 看右栏成本多少
        skip_video_ = envOn("SKIP_VIDEO");
        skip_side_ = envOn("SKIP_SIDEBAR");
        printf("[qt] 诊断: SKIP_VIDEO=%d SKIP_SIDEBAR=%d\n", skip_video_ ? 1 : 0, skip_side_ ? 1 : 0);
        setAttribute(Qt::WA_AcceptTouchEvents);   // 收 QTouchEvent（否则触摸只被合成为鼠标事件）
        et_.start();
        // 10ms 轮询：视频取到新帧 或 状态有更新 → 重绘；两者都没变就不画（省 CPU）
        auto* t = new QTimer(this);
        QObject::connect(t, &QTimer::timeout, this, [this] {
            ++tick_;
            bool need = false;
            QImage img;
            if (src_->takeFrame(img)) { frame_ = img; ++got_; need = true; }
            if (link_->dirty()) { link_->clearDirty(); trackEvent(); need = true; side_dirty_ = true; }
            if (need || frame_.isNull()) update();   // 无画面时让占位动画继续动
        });
        t->start(10);
        // 每秒打一行统计（终端里看）
        auto* t2 = new QTimer(this);
        QObject::connect(t2, &QTimer::timeout, this, [this] {
            double sec = et_.elapsed() / 1000.0;
            int d = paints_ - last_paints_;
            last_paints_ = paints_;
            fps_now_ = sec > 0 ? paints_ / sec : 0.0;
            printf("[qt] UI 重绘 %.1f fps（最近1秒 %d 次）| 解码收帧 %llu 显示 %llu 顶掉 %llu "
                   "| 状态通道 %s | 窗口 %dx%d\n",
                   fps_now_, d, (unsigned long long)src_->received(),
                   (unsigned long long)got_, (unsigned long long)src_->dropped(),
                   link_->state().valid ? "已连接" : "等待主程序", width(), height());
            fflush(stdout);
        });
        t2->start(1000);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        ++paints_;
        QPainter p(this);
        const int W = width(), H = height();
        p.fillRect(rect(), QColor(12, 12, 16));

        // ---- 横屏布局：左视频、右信息栏 ----
        const int sbw = qBound(230, W / 4, 400);             // 右栏宽（自适应）
        const QRect side(W - sbw, 0, sbw, H);
        const QRect vr = videoRect();                        // 与事件处理共用同一套算法

        p.fillRect(vr, Qt::black);
        if (skip_video_) {
            p.fillRect(vr, QColor(45, 45, 60));      // 诊断：不画视频（但占位面积一样大）
        } else if (!frame_.isNull()) {
            // ⚠️ **别开 SmoothPixmapTransform**：在 1920x1080 屏上把 640x360 平滑放大到 ~1500x850，
            //    每帧要双线性插值 127 万像素，实测 UI 直接从 31fps 掉到 13fps（顶掉 56%）。
            //    最近邻放大粗糙一点，但快好几倍；画面本身分辨率不高，平滑反而看不出好处。
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(vr, frame_);
        } else {
            drawWaiting(p, vr);
        }
        p.setPen(QPen(QColor(0, 170, 220), 3));
        p.drawRect(vr);

        drawOverlay(p, vr);      // ROI 黄框 + 检测框

        // ---- 右栏：内容 100ms 才变一次，缓存成 pixmap，每帧只做一次 blit ----
        // （之前每帧重画几十行文字+事件列表，光字形渲染就很贵）
        if (!skip_side_) {
            if (side_dirty_ || side_cache_.size() != side.size()) {
                btns_.clear();                  // 按钮命中区跟右栏缓存一起重建
                side_cache_ = QPixmap(side.size());
                side_cache_.fill(QColor(20, 22, 28));
                QPainter sp(&side_cache_);
                drawSidebar(sp, QRect(0, 0, side.width(), side.height()));
                side_dirty_ = false;
            }
            p.drawPixmap(side.topLeft(), side_cache_);
        }
    }

    // ---------------- 输入：鼠标 + 触摸 → 同一套拖动逻辑 ----------------
    // Wayland 下 weston 一般会把单指触摸合成成鼠标事件，但不同版本行为不一，
    // 所以两条路都接；Touch 处理后 return true 吞掉，避免再被合成为鼠标事件而重复处理
    void mousePressEvent(QMouseEvent* e) override { onPress(e->localPos()); }
    void mouseMoveEvent(QMouseEvent* e) override { onMove(e->localPos()); }
    void mouseReleaseEvent(QMouseEvent*) override { onRelease(); }

    bool event(QEvent* e) override {
        if (e->type() == QEvent::TouchBegin || e->type() == QEvent::TouchUpdate ||
            e->type() == QEvent::TouchEnd) {
            QTouchEvent* te = static_cast<QTouchEvent*>(e);
            const QList<QTouchEvent::TouchPoint>& pts = te->touchPoints();
            if (pts.isEmpty()) return true;
            const QPointF pos = pts.first().pos();
            if (e->type() == QEvent::TouchBegin)       onPress(pos);
            else if (e->type() == QEvent::TouchUpdate) onMove(pos);
            else                                       onRelease();
            return true;
        }
        return QWidget::event(e);
    }

private:
    // 板上实测：不指定族名时 Qt 的 fallback 会失败（load glyph failed err=24），一个字都画不出
    static QFont fontFor(int pt, bool bold = true) {
        QFont f(QStringLiteral("Liberation Sans"));
        if (!QFontInfo(f).family().contains(QStringLiteral("Liberation"), Qt::CaseInsensitive))
            f = QFont(QStringLiteral("DejaVu Sans"));
        f.setPointSize(pt);
        f.setBold(bold);
        return f;
    }

    // 在 box 内按 asw:ash 比例居中放一个矩形（letterbox，避免画面被拉伸变形）
    static QRect fitAspect(const QRect& box, int asw, int ash) {
        int w = box.width(), h = w * ash / asw;
        if (h > box.height()) { h = box.height(); w = h * asw / ash; }
        return QRect(box.x() + (box.width() - w) / 2, box.y() + (box.height() - h) / 2, w, h);
    }

    static QColor evColor(const QString& line) {
        if (line.contains(QStringLiteral("ALARM")))   return QColor(255, 110, 110);
        if (line.contains(QStringLiteral("INTRUDE"))) return QColor(255, 210, 90);
        if (line.contains(QStringLiteral("RESOLVE"))) return QColor(120, 240, 140);
        return QColor(170, 175, 190);                 // LEAVE / 其它
    }

    void drawWaiting(QPainter& p, const QRect& vr) {
        p.setPen(QColor(255, 200, 0));
        p.setFont(fontFor(24));
        p.drawText(vr.adjusted(20, 0, -20, 0), Qt::AlignCenter,
                   QStringLiteral("等待视频流 tcp://127.0.0.1:%1").arg(port_));
        const int bw = qMax(40, vr.width() / 10);
        const int x = vr.x() + 20 + int((long(tick_) * 10) % qMax(1, vr.width() - bw - 40));
        p.fillRect(x, vr.bottom() - bw - 20, bw, bw, QColor(0, 255, 120));
    }

    // 叠加层：坐标系是主程序的 1280x720，要映射到视频区 vr
    void drawOverlay(QPainter& p, const QRect& vr) {
        const BoardState& st = link_->state();
        if (!st.valid || st.fw <= 0 || st.fh <= 0) return;
        const double sx = double(vr.width()) / st.fw;
        const double sy = double(vr.height()) / st.fh;

        // ---- 取当前该显示哪个 ROI ----
        //   拖动中            → 本地那份（跟手）
        //   刚下发、主程序还没回报到位 → 继续显示本地值（否则会“闪回旧框”再跳到新框）
        //   其余              → 主程序回报的真实值
        if (pending_) {
            const bool acked = qAbs(st.roi_x - pending_roi_.x()) < 4 &&
                               qAbs(st.roi_y - pending_roi_.y()) < 4 &&
                               qAbs(st.roi_w - pending_roi_.width()) < 4 &&
                               qAbs(st.roi_h - pending_roi_.height()) < 4;
            if (acked || pending_t_.elapsed() > 1500) pending_ = false;   // 确认到位或超时
        }
        QRectF roi;
        bool editing = false;
        if (drag_ != DRAG_NONE && roi_edit_valid_) {
            roi = roi_edit_;
            editing = true;
        } else if (pending_) {
            roi = pending_roi_;
        } else {
            roi = QRectF(st.roi_x, st.roi_y, st.roi_w, st.roi_h);
        }
        if (roi.width() > 0 && roi.height() > 0) {
            const QColor rc = editing ? QColor(255, 160, 0) : QColor(255, 220, 0);
            const QRectF r(vr.x() + roi.x() * sx, vr.y() + roi.y() * sy,
                           roi.width() * sx, roi.height() * sy);
            p.fillRect(r, QColor(rc.red(), rc.green(), rc.blue(), 26));
            p.setPen(QPen(rc, editing ? 4 : 3));
            p.drawRect(r);

            // 四角拖拽柄：既是“可以拖”的提示，也是触摸目标（拾取热区更大，见 onPress）
            p.setPen(Qt::NoPen);
            p.setBrush(rc);
            const QPointF cs[4] = {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()};
            for (const QPointF& c : cs)
                p.drawRect(QRectF(c.x() - 7, c.y() - 7, 14, 14));
            p.setBrush(Qt::NoBrush);

            // 拖动中实时显示数值（触摸屏上没有鼠标指针，得让用户看到数字在变）
            if (editing) {
                p.setFont(fontFor(16));
                const QString tip = QStringLiteral("ROI %1,%2  %3x%4   松手生效")
                    .arg(int(roi.x())).arg(int(roi.y()))
                    .arg(int(roi.width())).arg(int(roi.height()));
                const QFontMetrics fm(p.font());
                const int tw = fm.horizontalAdvance(tip) + 18;
                QRect t(int(r.x()), int(r.y()) - fm.height() - 10, tw, fm.height() + 10);
                if (t.top() < vr.top()) t.moveTop(int(r.bottom()) + 10);   // 上方放不下就放框下面
                if (t.right() > vr.right()) t.moveRight(vr.right());
                if (t.left() < vr.left()) t.moveLeft(vr.left());
                p.fillRect(t, QColor(255, 160, 0, 235));
                p.setPen(Qt::black);
                p.drawText(t, Qt::AlignCenter, tip);
            }
        }

        // ---- 检测框：ROI 内=红/青（会报警）；ROI 外=灰色（看到了但不报警）----
        // 判定点与 RoiMonitor 保持一致：**框底边中点**。这样屏幕上看到的颜色
        // 就直接对应“这一帧到底会不会触发入侵”，不用去猜业务逻辑。
        p.setFont(fontFor(17));
        const QFontMetrics fm(p.font());
        for (const DetBox& b : st.dets) {
            const double bx = (b.x1 + b.x2) / 2;
            const double by = b.y2;
            const bool inside = roi.width() > 0 &&
                                bx >= roi.left() && bx <= roi.right() &&
                                by >= roi.top()  && by <= roi.bottom();
            const QColor c = inside ? ((b.cls_id == 0) ? QColor(255, 70, 70) : QColor(70, 200, 255))
                                    : QColor(125, 128, 140);
            const QRectF r(vr.x() + b.x1 * sx, vr.y() + b.y1 * sy,
                           (b.x2 - b.x1) * sx, (b.y2 - b.y1) * sy);
            p.setPen(QPen(c, inside ? 3 : 2));
            p.drawRect(r);

            const QString tag = QStringLiteral("%1 %2%")
                .arg(b.cls_id == 0 ? QStringLiteral("person") : QStringLiteral("car"))
                .arg(int(b.conf * 100));
            const int tw = fm.horizontalAdvance(tag) + 14;
            QRect t(int(r.x()), int(r.y()) - fm.height() - 6, tw, fm.height() + 6);
            if (t.y() < vr.y()) t.moveTop(int(r.y()) + 2);   // 顶部放不下就移到框内
            p.fillRect(t, c);
            p.setPen(inside ? Qt::black : QColor(230, 230, 235));
            p.drawText(t, Qt::AlignCenter, tag);
        }
    }

    void drawSidebar(QPainter& p, const QRect& s) {
        const BoardState& st = link_->state();
        p.fillRect(s, QColor(20, 22, 28));

        const int x = s.x() + 14;
        const int right = s.right() - 14;
        const int lh = 26;                    // 行高
        const int kPt = 14;                   // 正文键值字号
        const int avail = s.width() - 28;     // 单行可用宽度
        int y = s.y() + 38;

        p.setPen(QColor(90, 200, 255));
        p.setFont(fontFor(19));
        p.drawText(x, y, QStringLiteral("RK3568 入侵检测"));
        y += 16;

        // 画一个圆角按钮并登记命中区；cmd 以 '@' 开头 = 本地动作（如切页），否则发给主程序
        auto button = [&](const QRect& r, const QString& label, const QString& cmd,
                          const QColor& bg) {
            const bool held = (pressed_cmd_ == cmd);
            p.setPen(Qt::NoPen);
            p.setBrush(held ? bg.lighter(160) : bg);
            p.drawRoundedRect(r, 6, 6);
            p.setBrush(Qt::NoBrush);
            p.setPen(QColor(235, 240, 250));
            p.setFont(fontFor(14));
            p.drawText(r, Qt::AlignCenter, label);
            Btn b; b.r = r; b.cmd = cmd;
            btns_.append(b);
        };

        // ---- 页签：状态 / 参数 ----
        {
            const int bw = (avail - 6) / 2;
            button(QRect(x, y, bw, 32), QStringLiteral("状态"), QStringLiteral("@page 0"),
                   page_ == 0 ? QColor(52, 92, 140) : QColor(40, 46, 58));
            button(QRect(x + bw + 6, y, bw, 32), QStringLiteral("参数"), QStringLiteral("@page 1"),
                   page_ == 1 ? QColor(52, 92, 140) : QColor(40, 46, 58));
            y += 44;
        }

        p.setFont(fontFor(kPt, false));
        // 键左对齐、值右对齐；**值太长就自动缩字号**，否则会撞上键名甚至画出栏外
        auto line = [&](const QString& k, const QString& v, const QColor& vc = QColor(235, 235, 245)) {
            const QFont base = fontFor(kPt, false);
            p.setFont(base);
            p.setPen(QColor(150, 158, 175));
            p.drawText(x, y, k);

            const int room = avail - QFontMetrics(base).horizontalAdvance(k) - 8;
            QFont vf = base;
            const int need = QFontMetrics(vf).horizontalAdvance(v);
            if (need > room && need > 0 && room > 20)
                vf.setPointSizeF(qMax(9.0, vf.pointSizeF() * double(room) / need));
            p.setFont(vf);
            p.setPen(vc);
            p.drawText(QRect(x, y - 20, avail, 24), Qt::AlignRight, v);
            y += lh;
        };

        auto sep = [&] {
            y += 8;
            p.setPen(QPen(QColor(60, 70, 90), 2));
            p.drawLine(x, y, right, y);
            y += 26;
        };

        if (page_ == 0) {
            // ================= 状态页 =================
            line(QStringLiteral("状态通道"),
                 st.valid ? QStringLiteral("已连接") : QStringLiteral("等待中"),
                 st.valid ? QColor(120, 240, 140) : QColor(250, 190, 80));
            line(QStringLiteral("检测帧率"), QStringLiteral("%1 fps").arg(st.fps, 0, 'f', 1));
            line(QStringLiteral("推理帧数"), QString::number(st.infer));
            line(QStringLiteral("事件/告警"), QStringLiteral("%1 / %2").arg(st.events).arg(st.alarms));
            line(QStringLiteral("推流"), QStringLiteral("%1 / 丢 %2").arg(st.pushed).arg(st.dropped));
            sep();
            line(QStringLiteral("ROI"), QStringLiteral("%1,%2 %3x%4")
                     .arg(st.roi_x).arg(st.roi_y).arg(st.roi_w).arg(st.roi_h));
            if (roi_edit_valid_ && drag_ != DRAG_NONE)     // 手指正压在屏上拖 → 给个明确反馈
                line(QStringLiteral("ROI 编辑中"), QStringLiteral("松手生效"), QColor(255, 180, 60));
            line(QStringLiteral("停留阈值"), QStringLiteral("%1 s").arg(st.stay_sec));
            line(QStringLiteral("离开去抖"), QStringLiteral("%1 帧").arg(st.leave_confirm));
            line(QStringLiteral("conf/nms"),
                 QStringLiteral("%1/%2").arg(st.conf, 0, 'f', 2).arg(st.nms, 0, 'f', 2));
            line(QStringLiteral("上报 PC"),
                 st.report_on ? QStringLiteral("开") : QStringLiteral("关"),
                 st.report_on ? QColor(120, 240, 140) : QColor(170, 175, 190));

            // ---- 最近事件（新的在上）----
            y += 12;
            p.setPen(QColor(90, 200, 255));
            p.setFont(fontFor(16));
            p.drawText(x, y, QStringLiteral("最近事件"));
            y += 24;
            p.setFont(fontFor(13, false));
            if (ev_hist_.isEmpty()) {
                p.setPen(QColor(130, 135, 150));
                p.drawText(x, y, QStringLiteral("（暂无）"));
            }
            for (const QString& e : ev_hist_) {
                if (y > s.bottom() - 14) break;
                p.setPen(evColor(e));
                p.drawText(x, y, e);
                y += 21;
            }
        } else {
            // ================= 参数页（把运行时控制台搬到屏上）=================
            // 每个可调项：显示当前值 + [−] [+] 两个大按钮（触摸友好）
            auto row = [&](const QString& label, const QString& value,
                           const QString& cmd_dec, const QString& cmd_inc) {
                p.setPen(QColor(150, 158, 175));
                p.setFont(fontFor(kPt, false));
                p.drawText(x, y, label);
                p.setPen(QColor(235, 240, 250));
                p.drawText(QRect(x, y - 20, avail, 24), Qt::AlignRight, value);
                y += 22;
                const int bw = (avail - 8) / 2;
                button(QRect(x, y, bw, 34), QStringLiteral("−"), cmd_dec, QColor(72, 50, 52));
                button(QRect(x + bw + 8, y, bw, 34), QStringLiteral("+"), cmd_inc, QColor(45, 74, 56));
                y += 42;
            };

            row(QStringLiteral("停留阈值 (秒)"), QString::number(st.stay_sec),
                QStringLiteral("stay %1").arg(qMax(1, st.stay_sec - 1)),
                QStringLiteral("stay %1").arg(st.stay_sec + 1));
            row(QStringLiteral("离开去抖 (帧)"), QString::number(st.leave_confirm),
                QStringLiteral("leave %1").arg(qMax(1, st.leave_confirm - 1)),
                QStringLiteral("leave %1").arg(st.leave_confirm + 1));
            row(QStringLiteral("置信度 conf"), QString::number(st.conf, 'f', 2),
                QStringLiteral("conf %1").arg(qMax(0.05, st.conf - 0.05), 0, 'f', 2),
                QStringLiteral("conf %1").arg(qMin(0.95, st.conf + 0.05), 0, 'f', 2));
            row(QStringLiteral("NMS 阈值"), QString::number(st.nms, 'f', 2),
                QStringLiteral("nms %1").arg(qMax(0.05, st.nms - 0.05), 0, 'f', 2),
                QStringLiteral("nms %1").arg(qMin(0.95, st.nms + 0.05), 0, 'f', 2));

            // 开关类
            button(QRect(x, y, avail, 34),
                   st.report_on ? QStringLiteral("上报 PC：开（点击关）")
                                : QStringLiteral("上报 PC：关（点击开）"),
                   QStringLiteral("report %1").arg(st.report_on ? QStringLiteral("off")
                                                                 : QStringLiteral("on")),
                   st.report_on ? QColor(45, 74, 56) : QColor(62, 56, 44));
            y += 44;

            // 动作类
            const int bw = (avail - 8) / 2;
            button(QRect(x, y, bw, 34), QStringLiteral("ROI 全屏"),
                   QStringLiteral("roi 0 0 1280 720"), QColor(52, 60, 78));
            button(QRect(x + bw + 8, y, bw, 34), QStringLiteral("保存配置"),
                   QStringLiteral("save"), QColor(52, 60, 78));
            y += 42;
            button(QRect(x, y, avail, 34), QStringLiteral("重新加载配置"),
                   QStringLiteral("reload"), QColor(52, 60, 78));
            y += 46;

            p.setPen(QColor(140, 148, 165));
            p.setFont(fontFor(12, false));
            p.drawText(QRect(x, y, avail, 110), Qt::TextWordWrap,
                       QStringLiteral("提示：ROI 也可直接在左边画面里拖动四角调整。\n"
                                      "所有改动都是运行时热更新，不需重启程序。"));
        }
    }

    // 状态里的“最近事件”变了就记一条历史（主程序只在事件发生时刷新它）
    void trackEvent() {
        const BoardState& st = link_->state();
        if (!st.valid || st.last_event.isEmpty()) return;
        if (st.last_event == last_ev_ && st.last_stay_ms == last_ev_stay_) return;
        last_ev_ = st.last_event;
        last_ev_stay_ = st.last_stay_ms;
        ev_hist_.prepend(QStringLiteral("%1  %2  %3s")
                             .arg(QTime::currentTime().toString(QStringLiteral("hh:mm:ss")))
                             .arg(st.last_event)
                             .arg(st.last_stay_ms / 1000.0, 0, 'f', 1));
        while (ev_hist_.size() > 10) ev_hist_.removeLast();
    }

    // ---------------- 拖动交互（触摸调 ROI）----------------
    enum DragMode { DRAG_NONE, DRAG_MOVE, DRAG_TL, DRAG_TR, DRAG_BL, DRAG_BR };

    // 视频区矩形：paintEvent 与事件处理必须用**同一套算法**，否则“看到的框”和“拖到的位置”对不上
    QRect videoRect() const {
        const int sbw = qBound(230, width() / 4, 400);
        return fitAspect(QRect(8, 8, width() - sbw - 16, height() - 16), 16, 9);
    }

    // 屏幕坐标 → 视频坐标系（1280x720）
    QPointF toVideo(const QPointF& screen) const {
        const QRect vr = videoRect();
        const BoardState& st = link_->state();
        if (st.fw <= 0 || st.fh <= 0 || vr.width() <= 0 || vr.height() <= 0) return QPointF();
        return QPointF((screen.x() - vr.x()) * double(st.fw) / vr.width(),
                       (screen.y() - vr.y()) * double(st.fh) / vr.height());
    }

    // 视频坐标 → 屏幕坐标（画手柄、算拾取距离用）
    QPointF toScreen(const QPointF& v) const {
        const QRect vr = videoRect();
        const BoardState& st = link_->state();
        if (st.fw <= 0 || st.fh <= 0) return QPointF();
        return QPointF(vr.x() + v.x() * double(vr.width()) / st.fw,
                       vr.y() + v.y() * double(vr.height()) / st.fh);
    }

    void onPress(const QPointF& p) {
        const BoardState& st = link_->state();

        // 0) 按钮优先：命中就按下高亮，不进入 ROI 拖动
        if (!skip_side_) {
            const int sbw = qBound(230, width() / 4, 400);
            const QPoint off(width() - sbw, 0);          // 按钮 rect 是“相对右栏缓存”的坐标
            for (const Btn& b : btns_) {
                if (b.r.translated(off).contains(p.toPoint())) {
                    pressed_cmd_ = b.cmd;
                    side_dirty_ = true;
                    update();
                    return;
                }
            }
        }

        if (!st.valid || st.fw <= 0 || st.roi_w <= 0) return;
        if (!roi_edit_valid_) {                 // 每次按下一份新的编辑副本
            roi_edit_ = QRectF(st.roi_x, st.roi_y, st.roi_w, st.roi_h);
            roi_edit_valid_ = true;
        }
        const QPointF v = toVideo(p);

        // 1) 先判四个角：拾取半径按**屏幕像素**算（手指大约 10mm，热区太小点不中）
        struct Hit { QPointF pt; DragMode m; };
        const double pick = 34.0;
        const Hit cs[4] = {
            {QPointF(roi_edit_.left(),  roi_edit_.top()),    DRAG_TL},
            {QPointF(roi_edit_.right(), roi_edit_.top()),    DRAG_TR},
            {QPointF(roi_edit_.left(),  roi_edit_.bottom()), DRAG_BL},
            {QPointF(roi_edit_.right(), roi_edit_.bottom()), DRAG_BR},
        };
        for (const Hit& c : cs) {
            if (QLineF(toScreen(c.pt), p).length() <= pick) {
                drag_ = c.m;
                drag_v_ = v;
                side_dirty_ = true;
                update();
                return;
            }
        }
        // 2) 框内 → 整体挪动
        if (roi_edit_.contains(v)) {
            drag_ = DRAG_MOVE;
            drag_v_ = v;
            side_dirty_ = true;
            update();
        }
    }
    void onMove(const QPointF& p) {
        if (drag_ == DRAG_NONE || !roi_edit_valid_) return;
        const BoardState& st = link_->state();
        const QPointF v = toVideo(p);
        const double dx = v.x() - drag_v_.x();
        const double dy = v.y() - drag_v_.y();
        drag_v_ = v;

        QRectF r = roi_edit_;
        switch (drag_) {
            case DRAG_MOVE: r.translate(dx, dy); break;
            case DRAG_TL:   r.setTopLeft(r.topLeft() + QPointF(dx, dy)); break;
            case DRAG_TR:   r.setTopRight(r.topRight() + QPointF(dx, dy)); break;
            case DRAG_BL:   r.setBottomLeft(r.bottomLeft() + QPointF(dx, dy)); break;
            case DRAG_BR:   r.setBottomRight(r.bottomRight() + QPointF(dx, dy)); break;
            default: break;
        }
        // normalized()：拖过头会把矩形拖翻，先规范化；再限到画面内 + 保底最小尺寸
        r = r.normalized().intersected(QRectF(0, 0, st.fw, st.fh));
        const double kMin = 60;
        if (r.width() < kMin)  r.setWidth(kMin);
        if (r.height() < kMin) r.setHeight(kMin);
        if (r.right() > st.fw)  r.moveRight(st.fw);
        if (r.bottom() > st.fh) r.moveBottom(st.fh);
        if (r.left() < 0)       r.moveLeft(0);
        if (r.top() < 0)        r.moveTop(0);
        roi_edit_ = r;
        side_dirty_ = true;
        update(videoRect());      // 拖动中只刷视频区：绘制量小、跟手更紧
    }

    void onRelease() {
        // 按钮释放：本地动作（@开头）或转发给主程序
        if (!pressed_cmd_.isEmpty()) {
            const QString c = pressed_cmd_;
            pressed_cmd_.clear();
            side_dirty_ = true;
            if (c.startsWith(QLatin1Char('@'))) {
                doLocal(c.mid(1));
            } else if (link_->sendCommand(c)) {
                printf("[qt] 已下发: %s\n", qPrintable(c));
            } else {
                printf("[qt] 状态通道未连接，命令未发出（会自动重连）\n");
            }
            update();
            return;
        }
        if (drag_ == DRAG_NONE || !roi_edit_valid_) return;
        const BoardState& st = link_->state();
        const int x = int(roi_edit_.x() + 0.5);
        const int y = int(roi_edit_.y() + 0.5);
        const int w = int(roi_edit_.width() + 0.5);
        const int h = int(roi_edit_.height() + 0.5);
        const bool moved = (x != st.roi_x || y != st.roi_y || w != st.roi_w || h != st.roi_h);
        const QRectF local = roi_edit_;        // 先存下来：待会儿要“乐观更新”
        drag_ = DRAG_NONE;
        roi_edit_valid_ = false;               // 交回给主程序回报的状态显示
        side_dirty_ = true;
        if (moved) {
            // **松手才发命令**：拖动中每帧都发会把状态通道和主程序刷爆
            const QString cmd = QStringLiteral("roi %1 %2 %3 %4").arg(x).arg(y).arg(w).arg(h);
            if (link_->sendCommand(cmd)) {
                // 乐观更新：主程序要下一帧（~60ms）才应用，这期间状态通道里还是旧值，
                // 若不继续显示本地值，屏幕就会“闪回旧框”再跳到新框
                pending_roi_ = local;
                pending_ = true;
                pending_t_.restart();
                printf("[qt] 已下发 ROI: %s\n", qPrintable(cmd));
            } else {
                printf("[qt] 状态通道未连接，ROI 命令没发出去（会自动重连）\n");
            }
        }
        update();
    }

    // 本地动作（不发给主程序）：目前只有页签切换
    void doLocal(const QString& a) {
        if (a.startsWith(QStringLiteral("page "))) {
            page_ = a.mid(5).toInt();
            side_dirty_ = true;
        }
    }

    GstSource* src_ = nullptr;
    StateLink* link_ = nullptr;
    uint16_t port_ = 0;
    // ---- 按钮/页签（Step 3c）----
    struct Btn { QRect r; QString cmd; };
    QVector<Btn> btns_;               // 右栏重绘时重建
    QString pressed_cmd_;             // 当前按下的按钮命令（高亮用）
    int page_ = 0;                    // 0=状态页 1=参数页
    DragMode drag_ = DRAG_NONE;
    QRectF roi_edit_;                 // 拖动中的 ROI（视频坐标）
    bool roi_edit_valid_ = false;
    QPointF drag_v_;                  // 上一次触摸位置（视频坐标）
    QRectF pending_roi_;              // 已下发、等主程序确认的 ROI（乐观更新用）
    bool pending_ = false;
    QElapsedTimer pending_t_;         // 等确认的超时计时
    QElapsedTimer et_;
    QImage frame_;
    QPixmap side_cache_;            // 右栏缓存（内容变化时才重画）
    bool side_dirty_ = true;
    bool skip_video_ = false;       // 诊断开关
    bool skip_side_ = false;
    QVector<QString> ev_hist_;      // 最近事件文本
    QString last_ev_;               // 去重用：上次记录的“最近事件”
    unsigned long long last_ev_stay_ = 0;
    int tick_ = 0;
    int paints_ = 0;
    int last_paints_ = 0;
    double fps_now_ = 0.0;
    uint64_t got_ = 0;              // UI 实际取到的帧数
};

int main(int argc, char** argv) {
    signal(SIGINT, on_sigint);       // Wayland/linuxfb 都没有窗口管理器，Ctrl+C 自己接管
    QApplication app(argc, argv);

    // 参数：argv[1]=视频端口，argv[2]=状态端口，argv[3]=解码输出宽度（默认 640，调大更清晰但更费 CPU）
    uint16_t port = 5000, stat_port = 9100;
    int out_w = 640;
    if (argc > 1) { int v = atoi(argv[1]); if (v > 0) port = uint16_t(v); }
    if (argc > 2) { int v = atoi(argv[2]); if (v > 0) stat_port = uint16_t(v); }
    if (argc > 3) { int v = atoi(argv[3]); if (v >= 320) out_w = v; }

    printf("[qt] Qt=%s  platform=%s  视频端口=%u  状态端口=%u  解码输出=%dx%d\n", qVersion(),
           QApplication::platformName().toUtf8().constData(),
           unsigned(port), unsigned(stat_port), out_w, out_w * 9 / 16);
    fflush(stdout);

    GstSource src;
    src.start(port, out_w, out_w * 9 / 16);   // 不要求主程序此刻在推流（会自动重连）

    StateLink link;
    link.start(stat_port);           // 不要求主程序此刻在跑（会自动重连）

    MainWindow w(&src, &link, port);
    w.showFullScreen();
    int rc = app.exec();
    src.stop();
    return rc;
}
