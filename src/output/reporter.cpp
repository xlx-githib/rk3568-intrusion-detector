// Reporter 实现（M2-4）：事件 → JSON 行 → TCP 发送，断线自动重连
#include <cstdio>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "output/reporter.hpp"

// 标准库名字统一引入(替代满屏 std:: 前缀)
using namespace std;

namespace {
// 标准 Base64 编码（缩略图字节 → JSON 字段用）
string b64encode(const uint8_t* d, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string out;
    out.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(d[i]) << 16;
        if (i + 1 < n) v |= uint32_t(d[i + 1]) << 8;
        if (i + 2 < n) v |= uint32_t(d[i + 2]);
        out.push_back(T[(v >> 18) & 0x3f]);
        out.push_back(T[(v >> 12) & 0x3f]);
        out.push_back((i + 1 < n) ? T[(v >> 6) & 0x3f] : '=');
        out.push_back((i + 2 < n) ? T[v & 0x3f] : '=');
    }
    return out;
}
}  // namespace

void Reporter::init(bool enable_report, const string& server_ip, int port) {
    enabled_ = enable_report;
    ip_ = server_ip;
    port_ = port;
}

bool Reporter::connect() {
    if (!enabled_) return false;
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) { printf("[rep] socket 失败\n"); return false; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port_);
    if (inet_pton(AF_INET, ip_.c_str(), &sa.sin_addr) != 1) {
        printf("[rep] 地址解析失败: %s\n", ip_.c_str());
        ::close(fd_); fd_ = -1; return false;
    }
    if (::connect(fd_, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        printf("[rep] 连接 %s:%d 失败\n", ip_.c_str(), port_);
        ::close(fd_); fd_ = -1; return false;
    }
    printf("[rep] 已连接 %s:%d\n", ip_.c_str(), port_);
    return true;
}

void Reporter::disconnect() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool Reporter::try_send(const string& s) {
    if (fd_ < 0) connect();
    if (fd_ < 0) return false;
    size_t off = 0;
    while (off < s.size()) {                 // 大消息(缩略图)可能需多次 send
        ssize_t n = send(fd_, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n < 0) {                         // 断线：关闭，下次自动重连
            printf("[rep] 发送失败，连接断开\n");
            ::close(fd_); fd_ = -1;
            return false;
        }
        off += size_t(n);
    }
    return true;
}

bool Reporter::report(const Event& e, const char* snapshot) {
    if (!enabled_) return false;
    string s;
    if (!build_json(e, snapshot, 0, 0, nullptr, s)) return false;
    return try_send(s);
}

bool Reporter::reportImg(const Event& e, const char* snapshot,
                         int tw, int th, const vector<uint8_t>& rgb) {
    if (!enabled_) return false;
    string s;
    if (!build_json(e, snapshot, tw, th, &rgb, s)) return false;
    return try_send(s);
}

bool Reporter::reportPreview(int tw, int th, const vector<uint8_t>& rgb) {
    if (!enabled_ || rgb.empty() || tw <= 0 || th <= 0) return false;
    string s;
    char head[96];
    int l = snprintf(head, sizeof(head), "{\"type\":\"PREVIEW\",\"thumb_w\":%d,\"thumb_h\":%d,\"img\":\"",
                     tw, th);
    if (l < 0 || size_t(l) >= sizeof(head)) return false;
    s.assign(head, size_t(l));
    s += b64encode(rgb.data(), rgb.size());
    s += "\"}\n";
    return try_send(s);
}

bool Reporter::build_json(const Event& e, const char* snapshot,
                          int tw, int th, const vector<uint8_t>* rgb,
                          string& out) {
    const char* type = "";
    switch (e.type) {
        case EventType::INTRUDE: type = "INTRUDE"; break;
        case EventType::ALARM:   type = "ALARM";   break;
        case EventType::LEAVE:   type = "LEAVE";   break;
        case EventType::RESOLVE: type = "RESOLVE"; break;
    }
    char head[320];
    int len = snprintf(head, sizeof(head),
                       "{\"type\":\"%s\",\"cls\":%d,\"ts_start_ms\":%llu,\"stay_ms\":%llu,\"snapshot\":\"%s\"",
                       type, e.cls_id,
                       (unsigned long long)(e.ts_start_us / 1000ULL),
                       (unsigned long long)e.stay_ms,
                       snapshot ? snapshot : "");
    if (len < 0 || size_t(len) >= sizeof(head)) return false;
    out.assign(head, size_t(len));
    if (rgb && !rgb->empty() && tw > 0 && th > 0) {
        char imghead[96];
        int l2 = snprintf(imghead, sizeof(imghead), ",\"thumb_w\":%d,\"thumb_h\":%d,\"img\":\"",
                          tw, th);
        out.append(imghead, size_t(l2));
        out += b64encode(rgb->data(), rgb->size());
        out += "\"";
    }
    out += "}\n";
    return true;
}
