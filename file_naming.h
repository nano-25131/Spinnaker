#pragma once
// 文件命名 / 编号扫描
#include "text_utils.h"

// 目录/前缀_编号.png（前缀为空时为 目录/编号.png）
fs::path MakeImagePath(const fs::path &folder, const fs::path &prefix, long long id);

// 扫描目录，找出符合 "前缀_数字.png"（或无前缀时 "数字.png"）的最大编号；没有则返回 -1
long long ScanMaxIndex(const fs::path &folder, const fs::path &prefix);
