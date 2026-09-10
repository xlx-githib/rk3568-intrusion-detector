#include "common/config.hpp"

#include <fstream>
#include <string>

// 标准库名字统一引入(替代满屏 std:: 前缀)
using namespace std;

namespace {
string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
}  // namespace

bool Config::load(const string& path, Config& out) {
    ifstream in(path);
    if (!in.is_open()) return false;
    out.kv_.clear();

    string line;
    while (getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == string::npos) continue;
        string k = trim(line.substr(0, eq));
        string v = trim(line.substr(eq + 1));
        if (!k.empty()) out.kv_[k] = v;
    }
    return true;
}

bool Config::has(const string& k) const { return kv_.count(k) > 0; }

string Config::get(const string& k, const string& def) const {
    auto it = kv_.find(k);
    return (it == kv_.end()) ? def : it->second;
}

int Config::getInt(const string& k, int def) const {
    auto it = kv_.find(k);
    if (it == kv_.end()) return def;
    return stoi(it->second);
}

double Config::getDouble(const string& k, double def) const {
    auto it = kv_.find(k);
    if (it == kv_.end()) return def;
    return stod(it->second);
}

bool Config::getBool(const string& k, bool def) const {
    auto it = kv_.find(k);
    if (it == kv_.end()) return def;
    string v = it->second;
    return (v == "1" || v == "true" || v == "yes" || v == "on");
}
