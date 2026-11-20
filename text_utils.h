#pragma once
// 字符串 / 路径工具（全程以 UTF-8 std::string 传递文本，落到文件系统时转成 fs::path）
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// 去掉字符串首尾空白和引号（Windows 下复制路径常带引号）
std::string TrimPath(std::string s);
std::string ToLower(std::string s);

// UTF-8 字符串 -> fs::path（Windows 下走宽字符，中文路径不会丢）
fs::path Utf8ToPath(const std::string &s);
// fs::path -> UTF-8 字符串（仅用于打印）
std::string PathToUtf8(const fs::path &p);

// 命令行参数统一转成 UTF-8（Windows 下 argv 是 ANSI/GBK，不能直接用）
std::vector<std::string> CollectArgs(int argc, char **argv);

// 可执行文件所在目录（config.ini 默认放这里，大家双击/从任意目录启动都读同一份）
fs::path ExeDir();
