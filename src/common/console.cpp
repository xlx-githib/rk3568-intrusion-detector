// 运行时控制台实现（M3）
//   - 独立线程 poll(stdin) 读命令，不阻塞主流水线
//   - 只写 RtParams(原子)，工作线程靠 version 变化感知并应用
//   - save/reload 与 config/intruder.conf 互通(只接管自己管理的键，其余原样保留)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/stat.h>

#include "common/console.hpp"

using namespace std;

namespace {
void trim(string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i) s.erase(0, i);
}
bool isManaged(const string& k) {
    return k == "roi_x" || k == "roi_y" || k == "roi_w" || k == "roi_h" ||
           k == "stay_sec" || k == "leave_confirm" || k == "conf_thresh" ||
           k == "nms_thresh" || k == "report_enable";
}
}  // namespace

// ---------------- EventLog ----------------
void EventLog::add(const string& line) {
    lock_guard<mutex> lk(m_);
    q_.push_back(line);
    while (q_.size() > size_t(kMax)) q_.pop_front();
}

string EventLog::dump(int n) const {
    lock_guard<mutex> lk(m_);
    if (q_.empty()) return "(暂无事件)";
    if (n <= 0 || size_t(n) > q_.size()) n = int(q_.size());
    string out;
    size_t start = q_.size() - size_t(n);
    for (size_t i = start; i < q_.size(); ++i) {
        out += q_[i];
        out += '\n';
    }
    return out;
}

// ---------------- Console ----------------
void Console::start(RtParams* p, const string& conf_path,
                    StatusFn status_fn, EventsFn events_fn, atomic<bool>* quit) {
    p_ = p;
    conf_path_ = conf_path;
    status_fn_ = status_fn;
    events_fn_ = events_fn;
    quit_ = quit;
    running_ = true;
    th_ = thread(&Console::loop, this);
}

void Console::stop() {
    running_ = false;
    if (th_.joinable()) th_.join();
}

void Console::loop() {
    printf("[con] 运行时控制台就绪：输入 help 查看命令\n");
    char buf[256];
    while (running_.load()) {
        struct pollfd pfd;
        pfd.fd = 0;            // stdin
        pfd.events = POLLIN;
        int r = poll(&pfd, 1, 200);
        if (r <= 0) continue;  // 超时：回头检查 running_
        if (!fgets(buf, sizeof(buf), stdin)) break;   // EOF(如管道结束)
        string line(buf);
        trim(line);
        if (!line.empty()) onLine(line);
    }
}

void Console::onLine(const string& line) {
    istringstream iss(line);
    string cmd;
    iss >> cmd;

    if (cmd == "help" || cmd == "?") {
        printf(
            "[con] 可用命令:\n"
            "  status                    查看运行状态(帧率/参数/统计)\n"
            "  roi <x> <y> <w> <h>       在线设置警戒区(立即生效)\n"
            "  stay <秒>                 停留多久触发告警\n"
            "  leave <帧数>              离开去抖帧数\n"
            "  conf <0~1>                置信度阈值\n"
            "  nms <0~1>                 NMS IoU 阈值\n"
            "  log <0~3>                 日志级别(0=D 1=I 2=W 3=E)\n"
            "  report on|off             开关 socket 上报\n"
            "  events [n]                查看最近 n 条事件(默认 10)\n"
            "  save                      把当前参数写回配置文件\n"
            "  reload                    从配置文件重新加载参数\n"
            "  quit                      退出程序\n");
        return;
    }
    if (cmd == "status") {
        printf("%s", status_fn_ ? status_fn_().c_str() : "(无状态回调)\n");
        if (status_fn_) { }
        printf("\n");
        return;
    }
    if (cmd == "roi") {
        int x, y, w, h;
        if (!(iss >> x >> y >> w >> h)) { printf("[con] 用法: roi <x> <y> <w> <h>\n"); return; }
        int fw = p_->frame_w.load(), fh = p_->frame_h.load();
        if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
            (fw > 0 && x + w > fw) || (fh > 0 && y + h > fh)) {
            printf("[con] ROI 非法(画面 %dx%d，需 x+w<=宽, y+h<=高)\n", fw, fh);
            return;
        }
        p_->roi_x = x; p_->roi_y = y; p_->roi_w = w; p_->roi_h = h;
        p_->bump();
        printf("[con] ROI=(%d,%d,%d,%d) 已生效\n", x, y, w, h);
        return;
    }
    if (cmd == "stay") {
        int s;
        if (!(iss >> s) || s <= 0) { printf("[con] 用法: stay <秒(>0)>\n"); return; }
        p_->stay_sec = s; p_->bump();
        printf("[con] stay=%ds 已生效\n", s);
        return;
    }
    if (cmd == "leave") {
        int n;
        if (!(iss >> n) || n <= 0) { printf("[con] 用法: leave <帧数(>0)>\n"); return; }
        p_->leave_confirm = n; p_->bump();
        printf("[con] 离开去抖=%d 帧 已生效\n", n);
        return;
    }
    if (cmd == "conf") {
        double f;
        if (!(iss >> f) || f <= 0.0 || f >= 1.0) { printf("[con] 用法: conf <0~1>\n"); return; }
        p_->conf_pct = int(f * 100 + 0.5); p_->bump();
        printf("[con] conf_thresh=%.2f 已生效\n", p_->conf_pct.load() / 100.0);
        return;
    }
    if (cmd == "nms") {
        double f;
        if (!(iss >> f) || f <= 0.0 || f >= 1.0) { printf("[con] 用法: nms <0~1>\n"); return; }
        p_->nms_pct = int(f * 100 + 0.5); p_->bump();
        printf("[con] nms_thresh=%.2f 已生效\n", p_->nms_pct.load() / 100.0);
        return;
    }
    if (cmd == "log") {
        int l;
        if (!(iss >> l) || l < 0 || l > 3) { printf("[con] 用法: log <0~3>\n"); return; }
        p_->log_level = l;
        printf("[con] log_level=%d 已生效\n", l);
        return;
    }
    if (cmd == "report") {
        string v;
        if (!(iss >> v) || (v != "on" && v != "off")) { printf("[con] 用法: report on|off\n"); return; }
        p_->report_on = (v == "on");
        printf("[con] socket 上报已 %s\n", v.c_str());
        return;
    }
    if (cmd == "events") {
        int n = 10;
        iss >> n;
        printf("%s", events_fn_ ? events_fn_(n).c_str() : "(无事件回调)\n");
        return;
    }
    if (cmd == "save") { doSave(); return; }
    if (cmd == "reload") { doReload(); return; }
    if (cmd == "quit" || cmd == "exit") {
        printf("[con] 退出中...\n");
        if (quit_) *quit_ = true;
        return;
    }
    printf("[con] 未知命令: %s (输入 help)\n", cmd.c_str());
}

// 把当前参数格式化(用于 save)
string Console::kvOf(const string& k) const {
    char b[64];
    if (k == "roi_x") snprintf(b, sizeof(b), "%d", p_->roi_x.load());
    else if (k == "roi_y") snprintf(b, sizeof(b), "%d", p_->roi_y.load());
    else if (k == "roi_w") snprintf(b, sizeof(b), "%d", p_->roi_w.load());
    else if (k == "roi_h") snprintf(b, sizeof(b), "%d", p_->roi_h.load());
    else if (k == "stay_sec") snprintf(b, sizeof(b), "%d", p_->stay_sec.load());
    else if (k == "leave_confirm") snprintf(b, sizeof(b), "%d", p_->leave_confirm.load());
    else if (k == "conf_thresh") snprintf(b, sizeof(b), "%.2f", p_->conf_pct.load() / 100.0);
    else if (k == "nms_thresh") snprintf(b, sizeof(b), "%.2f", p_->nms_pct.load() / 100.0);
    else if (k == "report_enable") snprintf(b, sizeof(b), "%d", p_->report_on.load() ? 1 : 0);
    else b[0] = '\0';
    return string(b);
}

bool Console::applyKv(const string& k, const string& v, bool report) {
    if (!isManaged(k)) return false;
    long iv = atol(v.c_str());
    double fv = atof(v.c_str());
    if (k == "roi_x") p_->roi_x = int(iv);
    else if (k == "roi_y") p_->roi_y = int(iv);
    else if (k == "roi_w") p_->roi_w = int(iv);
    else if (k == "roi_h") p_->roi_h = int(iv);
    else if (k == "stay_sec") p_->stay_sec = (iv > 0) ? int(iv) : 1;
    else if (k == "leave_confirm") p_->leave_confirm = (iv > 0) ? int(iv) : 1;
    else if (k == "conf_thresh") p_->conf_pct = int(fv * 100 + 0.5);
    else if (k == "nms_thresh") p_->nms_pct = int(fv * 100 + 0.5);
    else if (k == "report_enable") p_->report_on = (iv != 0);
    if (report) p_->bump();
    return true;
}

void Console::doSave() {
    string path = conf_path_;
    size_t slash = path.find_last_of('/');
    if (slash != string::npos) mkdir(path.substr(0, slash).c_str(), 0755);  // 确保目录存在

    vector<string> lines;
    {
        ifstream in(path);
        string l;
        while (getline(in, l)) lines.push_back(l);
    }
    vector<string> out;
    for (auto& l : lines) {
        if (l.empty() || l[0] == '#') { out.push_back(l); continue; }
        size_t eq = l.find('=');
        if (eq == string::npos) { out.push_back(l); continue; }
        string k = l.substr(0, eq);
        trim(k);
        if (isManaged(k)) out.push_back(k + "=" + kvOf(k));
        else out.push_back(l);
    }
    const char* keys[] = {"roi_x", "roi_y", "roi_w", "roi_h", "stay_sec",
                          "leave_confirm", "conf_thresh", "nms_thresh", "report_enable"};
    for (const char* k : keys) {
        bool found = false;
        for (auto& l : out) if (l.rfind(string(k) + "=", 0) == 0) { found = true; break; }
        if (!found) out.push_back(string(k) + "=" + kvOf(k));
    }
    ofstream o(path, ios::trunc);
    if (!o && slash != string::npos) {          // 目录不可写：退化到当前目录同名文件
        path = path.substr(slash + 1);
        o.open(path, ios::trunc);
    }
    if (!o) { printf("[con] 写入失败: %s\n", conf_path_.c_str()); return; }
    for (auto& l : out) o << l << "\n";
    printf("[con] 参数已保存到 %s\n", path.c_str());
}

void Console::doReload() {
    ifstream in(conf_path_);
    if (!in) { printf("[con] 打不开 %s\n", conf_path_.c_str()); return; }
    string l;
    int n = 0;
    while (getline(in, l)) {
        if (l.empty() || l[0] == '#') continue;
        size_t eq = l.find('=');
        if (eq == string::npos) continue;
        string k = l.substr(0, eq), v = l.substr(eq + 1);
        trim(k);
        trim(v);
        if (applyKv(k, v, false)) ++n;
    }
    p_->bump();
    printf("[con] 已从 %s 重载 %d 项参数\n", conf_path_.c_str(), n);
}
