// StatLink 实现：本机状态通道（127.0.0.1:9100）
//   - 我们当服务端，等板端 Qt 连进来（Qt 在同板，走回环，不碰 WiFi）
//   - 推送：每 ~100ms 一行 JSON 状态（ROI/检测框/统计）
//   - 接收：一行一条控制命令，暂存队列等主流程取用（Step 3b 触摸调参用）
#include "output/statlink.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// 标准库名字统一引入(替代满屏 std:: 前缀)
using namespace std;

namespace {
uint64_t now_ms() {
    return uint64_t(chrono::duration_cast<chrono::milliseconds>(
                        chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace

StatLink::~StatLink() { stop(); }

bool StatLink::start(int port) {
    if (running_.load()) return true;
    if (port <= 0) return false;
    port_ = port;

    srv_ = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_ < 0) { perror("[stat] socket"); srv_ = -1; return false; }
    int on = 1;
    setsockopt(srv_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 只监听回环：Qt 界面就在同一块板上
    sa.sin_port = htons(port_);
    if (bind(srv_, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        perror("[stat] bind");
        ::close(srv_); srv_ = -1;
        return false;
    }
    if (listen(srv_, 2) < 0) {
        perror("[stat] listen");
        ::close(srv_); srv_ = -1;
        return false;
    }
    fcntl(srv_, F_SETFL, O_NONBLOCK);

    running_ = true;
    th_ = thread(&StatLink::loop, this);
    printf("[stat] 状态通道已监听 127.0.0.1:%d（板端 Qt 连这里拿 ROI/检测框）\n", port_);
    return true;
}

void StatLink::loop() {
    uint64_t last_send = 0;
    while (running_.load()) {
        // ---- 没客户端就等它连进来（非阻塞，便于退出）----
        if (fd_.load() < 0) {
            int c = accept(srv_, nullptr, nullptr);
            if (c >= 0) {
                fd_ = c;
                printf("[stat] 板端界面已连接\n");
            } else {
                usleep(50 * 1000);
                continue;
            }
        }
        int fd = fd_.load();
        if (fd < 0) continue;

        // ---- ① 收命令（非阻塞；一行一条，可能一次收到半行/多行）----
        char buf[512];
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (n == 0) {                                  // 对端正常关闭
            printf("[stat] 板端界面已断开\n");
            ::close(fd); fd_ = -1;
            continue;
        }
        if (n > 0) {
            buf[n] = '\0';
            recv_line_ += buf;                          // 拼上没处理完的尾巴
            size_t pos;
            while ((pos = recv_line_.find('\n')) != string::npos) {
                string line = recv_line_.substr(0, pos);
                recv_line_.erase(0, pos + 1);
                if (line.empty()) continue;
                printf("[stat] 收到命令: %s\n", line.c_str());
                lock_guard<mutex> lk(m_);
                if (cmds_.size() < 32) cmds_.push_back(line);   // 有界，防止对端刷爆
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            printf("[stat] 接收失败，断开\n");
            ::close(fd); fd_ = -1;
            continue;
        }

        // ---- ② 节流发状态（~100ms 一行）----
        uint64_t t = now_ms();
        if (t - last_send >= 100) {
            last_send = t;
            StateSnapshot snap;
            {
                lock_guard<mutex> lk(m_);
                snap = snap_;                           // 值拷贝：拷完就能放锁，发送不占锁
            }
            string s = buildJson(snap);
            size_t off = 0;
            bool ok = true;
            while (off < s.size()) {                    // 小数据一般一次发完，但别假设
                ssize_t w = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
                if (w <= 0) { ok = false; break; }
                off += size_t(w);
            }
            if (!ok) {
                printf("[stat] 发送失败，断开\n");
                ::close(fd); fd_ = -1;
            }
        }
        usleep(20 * 1000);
    }
}

void StatLink::publish(const StateSnapshot& s) {
    if (!running_.load()) return;
    lock_guard<mutex> lk(m_);
    snap_ = s;
}

bool StatLink::takeCommand(string& out) {
    lock_guard<mutex> lk(m_);
    if (cmds_.empty()) return false;
    out = cmds_.front();
    cmds_.pop_front();
    return true;
}

void StatLink::stop() {
    if (!running_.load()) return;
    running_ = false;
    if (th_.joinable()) th_.join();
    if (fd_.load() >= 0) { ::close(fd_.load()); fd_ = -1; }
    if (srv_ >= 0) { ::close(srv_); srv_ = -1; }
}

// 手写 JSON（不引第三方库）：一行一条，Qt 端用 QJsonDocument 解析
string StatLink::buildJson(const StateSnapshot& s) {
    char head[512];
    snprintf(head, sizeof(head),
             "{\"type\":\"STATE\",\"fw\":%d,\"fh\":%d,"
             "\"roi\":[%d,%d,%d,%d],\"stay\":%d,\"leave\":%d,"
             "\"conf\":%.2f,\"nms\":%.2f,\"log\":%d,\"report\":%d,"
             "\"fps\":%.1f,\"infer\":%d,\"events\":%d,\"alarms\":%d,"
             "\"pushed\":%llu,\"dropped\":%llu,\"ev\":\"%s\",\"es\":%llu,\"dets\":[",
             s.frame_w, s.frame_h,
             s.roi_x, s.roi_y, s.roi_w, s.roi_h, s.stay_sec, s.leave_confirm,
             s.conf_pct / 100.0, s.nms_pct / 100.0, s.log_level, s.report_on ? 1 : 0,
             s.fps, s.infer, s.events, s.alarms,
             (unsigned long long)s.pushed, (unsigned long long)s.dropped,
             s.last_event.c_str(), (unsigned long long)s.last_stay_ms);
    string out = head;

    const size_t kMaxDets = 12;    // 单行别太长（16 个框也够画了）
    for (size_t i = 0; i < s.dets.size() && i < kMaxDets; ++i) {
        const DetObject& d = s.dets[i];
        char b[160];
        snprintf(b, sizeof(b),
                 "%s{\"c\":%d,\"x1\":%.0f,\"y1\":%.0f,\"x2\":%.0f,\"y2\":%.0f,\"p\":%.2f}",
                 i ? "," : "", d.cls_id, d.x1, d.y1, d.x2, d.y2, d.conf);
        out += b;
    }
    out += "]}\n";
    return out;
}
