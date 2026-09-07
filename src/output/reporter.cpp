// Reporter 实现（M2-4）：事件 → JSON 行 → TCP 发送，断线自动重连
#include <cstdio>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "output/reporter.hpp"

void Reporter::init(bool enable_report, const std::string& server_ip, int port) {
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

bool Reporter::try_send(const std::string& s) {
    if (fd_ < 0) connect();
    if (fd_ < 0) return false;
    ssize_t n = send(fd_, s.data(), s.size(), MSG_NOSIGNAL);
    if (n < 0) {                       // 断线：关闭，下次自动重连
        printf("[rep] 发送失败，连接断开\n");
        ::close(fd_); fd_ = -1;
        return false;
    }
    return true;
}

bool Reporter::report(const Event& e, const char* snapshot) {
    if (!enabled_) return false;
    const char* type = "";
    switch (e.type) {
        case EventType::INTRUDE: type = "INTRUDE"; break;
        case EventType::ALARM:   type = "ALARM";   break;
        case EventType::LEAVE:   type = "LEAVE";   break;
        case EventType::RESOLVE: type = "RESOLVE"; break;
    }
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
                       "{\"type\":\"%s\",\"cls\":%d,\"ts_start_ms\":%llu,\"stay_ms\":%llu,\"snapshot\":\"%s\"}\n",
                       type, e.cls_id,
                       (unsigned long long)(e.ts_start_us / 1000ULL),
                       (unsigned long long)e.stay_ms,
                       snapshot ? snapshot : "");
    if (len < 0 || size_t(len) >= sizeof(buf)) return false;
    return try_send(std::string(buf, size_t(len)));
}
