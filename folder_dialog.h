#pragma once
// 选择文件夹对话框（运行中切换保存目录用）
#include "text_utils.h"

// 弹出系统"选择文件夹"对话框，initial 作为起始目录。
// 用户取消、出错或非 Windows 平台时返回空 path。会阻塞调用线程，请在主线程调用。
fs::path PickFolder(const fs::path &initial);