#pragma once
// Windows 头文件的统一入口：保证 NOMINMAX / WIN32_LEAN_AND_MEAN 一致，避免 min/max 宏污染。
// 注意：如果同一个 .cpp 里还要包含 Spinnaker/OpenCV，请把它们放在本文件之前。
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW
#pragma comment(lib, "shell32.lib")
#endif
