#pragma once
// 全局退出标志：用于响应 Ctrl+C / 点击控制台 X 关闭 / 注销 / 关机
#include <atomic>

extern std::atomic<bool> g_stopRequested; // 请求主循环退出
extern std::atomic<bool> g_cleanupDone;	  // 相机清理完成

// 设置控制台 UTF-8 输出，并注册控制台事件回调（点 X 关闭窗口也能正常释放相机）。非 Windows 下为空操作。
void InitConsole();
