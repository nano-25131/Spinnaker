#include "Spinnaker.h"
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "acquisition.h"
#include "config.h"
#include "console_ctrl.h"
#include "text_utils.h"

using namespace Spinnaker;
using namespace std;

static void PrintUsage()
{
	cout << "Usage: Acquisition.exe [--config=path] [--key=value ...]\n"
			"  --config=PATH        配置文件（默认：exe 同目录下的 config.ini，不存在会自动生成）\n"
			"  --prefix=NAME        文件名前缀\n"
			"  --start_index=N|auto 起始编号\n"
			"  --save_folder=DIR    保存目录\n"
			"  --exposure_us=N|auto|keep\n"
			"  --gain_db=N|auto|keep\n"
			"  --width=N --height=N --offset_x=N|center --offset_y=N|center\n"
			"  --png_compression=0-9 --preview_scale=0.2 --writer_queue_size=N --buffer_count=N\n"
			"  --pause_on_exit=true|false\n"
		 << endl;
}

int main(int argc, char **argv)
{
	// 控制台 UTF-8 输出 + 注册关闭事件回调（点 X 关闭窗口也能正常释放相机）
	InitConsole();

	const std::vector<std::string> args = CollectArgs(argc, argv);
	for (const auto &a : args)
	{
		if (a == "--help" || a == "-h" || a == "/?")
		{
			PrintUsage();
			return 0;
		}
	}

	//=========================读取配置============================================
	Config cfg;
	const auto cmdKv = ParseCommandLine(args);

	fs::path configPath = ExeDir() / "config.ini";
	{
		const auto it = cmdKv.find("config");
		if (it != cmdKv.end() && !it->second.empty())
			configPath = Utf8ToPath(it->second);
	}

	{
		std::map<std::string, std::string> fileKv;
		if (LoadIni(configPath, fileKv))
		{
			ApplySettings(cfg, fileKv, "config file");
		}
		else
		{
			cout << "Config file not found, creating default: " << PathToUtf8(configPath) << endl;
			if (!WriteDefaultConfig(configPath))
				cout << "Warning: cannot write default config (check permissions), using built-in defaults." << endl;
		}
	}
	ApplySettings(cfg, cmdKv, "command line");

	if (!ValidateConfig(cfg))
	{
		cout << "Press Enter to exit..." << endl;
		getchar();
		return -1;
	}
	PrintConfig(cfg, configPath);
	//============================================================================

	//=========================测试权限============================================
	FILE *tempFile = fopen("test.txt", "w+");
	if (tempFile == nullptr)
	{
		cout << "Failed to create file in current folder.  Please check "
				"permissions."
			 << endl;
		cout << "Press Enter to exit..." << endl;
		getchar();
		return -1;
	}
	fclose(tempFile);
	remove("test.txt");
	//============================================================================

	// Print application build information
	cout << "Application build date: " << __DATE__ << " " << __TIME__ << endl
		 << endl;

	// Retrieve singleton reference to system object
	SystemPtr system = System::GetInstance();

	// Print out current library version
	const LibraryVersion spinnakerLibraryVersion = system->GetLibraryVersion();
	cout << "Spinnaker library version: " << spinnakerLibraryVersion.major << "." << spinnakerLibraryVersion.minor
		 << "." << spinnakerLibraryVersion.type << "." << spinnakerLibraryVersion.build << endl
		 << endl;

	// Retrieve list of cameras from the system
	CameraList camList = system->GetCameras();

	const unsigned int numCameras = camList.GetSize();

	cout << "Number of cameras detected: " << numCameras << endl
		 << endl;

	// Finish if there are no cameras
	if (numCameras == 0)
	{
		// Clear camera list before releasing system
		camList.Clear();

		// Release system
		system->ReleaseInstance();

		cout << "Not enough cameras!" << endl;
		cout << "Done! Press Enter to exit..." << endl;
		getchar();

		return -1;
	}

	int result = 0;

	// 用独立作用域包住 CameraPtr，确保在 ReleaseInstance 之前已经释放
	{
		CameraPtr pCam = camList.GetByIndex(0);

		cout << "Running example for camera 0..." << endl;

		result = RunSingleCamera(pCam, cfg);

		cout << "Camera 0 example complete." << endl;

		pCam = nullptr;
	}

	// Clear camera list before releasing system
	camList.Clear();

	// Release system
	system->ReleaseInstance();

	// 如果是被关闭信号触发的退出，不再等待回车
	if (!g_stopRequested && cfg.pauseOnExit)
	{
		cout << endl
			 << "Done! Press Enter to exit..." << endl;
		getchar();
	}

	return result;
}
