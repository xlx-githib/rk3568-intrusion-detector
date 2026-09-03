#pragma once
// 配置文件解析（key=value，# 注释）
#include <string>
#include <map>

class Config {
public:
    // 从文件加载，成功返回 true
    static bool load(const std::string& path, Config& out);

    bool has(const std::string& k) const;
    std::string get(const std::string& k, const std::string& def = "") const;
    int         getInt(const std::string& k, int def = 0) const;
    double      getDouble(const std::string& k, double def = 0.0) const;
    bool        getBool(const std::string& k, bool def = false) const;

private:
    std::map<std::string, std::string> kv_;
};
