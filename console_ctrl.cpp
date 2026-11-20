#include "console_ctrl.h"
#include "platform.h"
#include <chrono>
#include <thread>

std::atomic<bool> g_stopRequested(false);
std::atomic<bool> g_cleanupDone(false);

#if defined(_WIN32)
static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
	switch (ctrlType)
	{
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		g_stopRequested = true;
		// 此回调运行在独立线程中。Windows 在回调返回后（或约 5 秒后）会终止进程，
		// 所以这里等待主线程把相机清理完（EndAcquisition / DeInit）再返回。
		for (int i = 0; i < 45 && !g_cleanupDone; ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		return TRUE;
	default:
		return FALSE;
	}
}
#endif

void InitConsole()
{
#if defined(_WIN32)
	// 控制台按 UTF-8 输出，打印中文路径不乱码（字体需支持中文）
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif
}
