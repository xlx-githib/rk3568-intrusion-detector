#include "common/config.hpp"

#include <fstream>
#include <string>

namespace {
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
}  // namespace

bool Config::load(const std::string& path, Config& out) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    out.kv_.clear();

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (!k.empty()) out.kv_[k] = v;
    }
    return true;
}

bool Config::has(const std::string& k) const { return kv_.count(k) > 0; }

std::string Config::get(const std::string& k, const std::string& def) const {
    auto it = kv_.find(k);
    return (it == kv_.end()) ? def : it->second;
}

int Config::getInt(const std::string& k, int def) const {
    auto it = kv_.find(k);
    if (it == kv_.end()) return def;
    return std::stoi(it->second);
}

double Config::getDouble(const std::string& k, double def) const {
    auto it = kv_.find(k);
    if (it == kv_.end()) return def;
    return std::stod(it->second);
}

bool Config::getBool(const std::string& k, bool def) const {
    auto it = kv_.find(k);
    if (it == kv_.end()) return def;
    std::string v = it->second;
    return (v == "1" || v == "true" || v == "yes" || v == "on");
}
