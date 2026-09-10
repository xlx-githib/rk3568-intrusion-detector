#pragma once
// 配置文件解析（key=value，# 注释）
#include <string>
#include <map>

// 标准库名字逐个引入(头文件不用 using namespace std，避免污染包含者)
using std::map;
using std::string;

class Config {
public:
    // 从文件加载，成功返回 true
    static bool load(const string& path, Config& out);

    bool has(const string& k) const;
    string get(const string& k, const string& def = "") const;
    int         getInt(const string& k, int def = 0) const;
    double      getDouble(const string& k, double def = 0.0) const;
    bool        getBool(const string& k, bool def = false) const;

private:
    map<string, string> kv_;
};
