#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <limits>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;
using namespace std;

namespace fs = std::filesystem;

// ============================================================================
// 全局退出标志：用于响应 Ctrl+C / 点击控制台 X 关闭 / 注销 / 关机
// ============================================================================
static std::atomic<bool> g_stopRequested(false); // 请求主循环退出
static std::atomic<bool> g_cleanupDone(false);	 // 相机清理完成

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

// ============================================================================
// RAII 守卫：保证任何退出路径都会释放相机资源
// ============================================================================

// 保证 EndAcquisition 一定被调用
struct AcquisitionGuard
{
	CameraPtr cam;
	explicit AcquisitionGuard(CameraPtr c) : cam(c) {}
	~AcquisitionGuard()
	{
		try
		{
			if (cam != nullptr && cam->IsStreaming())
			{
				cam->EndAcquisition();
			}
		}
		catch (...)
		{
		}
	}
	AcquisitionGuard(const AcquisitionGuard &) = delete;
	AcquisitionGuard &operator=(const AcquisitionGuard &) = delete;
};

// 保证 DeInit 一定被调用，并在完成后通知控制台关闭回调
struct CameraInitGuard
{
	CameraPtr cam;
	explicit CameraInitGuard(CameraPtr c) : cam(c) {}
	~CameraInitGuard()
	{
		try
		{
			if (cam != nullptr && cam->IsInitialized())
			{
				cam->DeInit();
			}
		}
		catch (...)
		{
		}
		g_cleanupDone = true;
	}
	CameraInitGuard(const CameraInitGuard &) = delete;
	CameraInitGuard &operator=(const CameraInitGuard &) = delete;
};

// 保证每一帧都会 Release（包括不完整帧、中途异常）
struct ImageReleaser
{
	ImagePtr &img;
	explicit ImageReleaser(ImagePtr &i) : img(i) {}
	~ImageReleaser()
	{
		try
		{
			if (img != nullptr)
			{
				img->Release();
			}
		}
		catch (...)
		{
		}
	}
	ImageReleaser(const ImageReleaser &) = delete;
	ImageReleaser &operator=(const ImageReleaser &) = delete;
};

// ============================================================================
// 工具函数
// ============================================================================

// 去掉字符串首尾空白和引号（Windows 下复制路径常带引号）
static std::string TrimPath(std::string s)
{
	const char *ws = " \t\r\n\"";
	const size_t b = s.find_first_not_of(ws);
	if (b == std::string::npos)
		return "";
	const size_t e = s.find_last_not_of(ws);
	return s.substr(b, e - b + 1);
}

// Use the following enum to select the stream mode
enum StreamMode
{
	STREAM_MODE_TELEDYNE_GIGE_VISION, // Teledyne Gige Vision is the default stream mode for spinview which is supported on Windows
	STREAM_MODE_PGRLWF,				  // Light Weight Filter driver is our legacy driver which is supported on Windows
	STREAM_MODE_SOCKET,				  // Socket is supported for MacOS and Linux, and uses native OS network sockets instead of a
									  // filter driver
};

#if defined(WIN32) || defined(WIN64) || defined(_WIN32)
const StreamMode chosenStreamMode = STREAM_MODE_TELEDYNE_GIGE_VISION;
#else
const StreamMode chosenStreamMode = STREAM_MODE_SOCKET;
#endif

// This function demonstrates how we can change stream modes.
int SetStreamMode(CameraPtr pCam)
{
	// Retrieve Stream nodemap
	const INodeMap &sNodeMap = pCam->GetTLStreamNodeMap();

	// The node "StreamMode" is only available for GEV cameras.
	// Skip setting stream mode if the node is inaccessible.
	const CEnumerationPtr ptrStreamMode = sNodeMap.GetNode("StreamMode");
	if (!IsReadable(ptrStreamMode) || !IsWritable(ptrStreamMode))
	{
		return 0;
	}

	gcstring streamMode;
	switch (chosenStreamMode)
	{
	case STREAM_MODE_PGRLWF:
		streamMode = "LWF";
		break;
	case STREAM_MODE_SOCKET:
		streamMode = "Socket";
		break;
	case STREAM_MODE_TELEDYNE_GIGE_VISION:
	default:
		streamMode = "TeledyneGigeVision";
	}

	// Retrieve the desired entry node from the enumeration node
	const CEnumEntryPtr ptrStreamModeCustom = ptrStreamMode->GetEntryByName(streamMode);
	if (!IsReadable(ptrStreamModeCustom))
	{
		// Failed to get custom node
		cout << "Stream mode " + streamMode + " not available.  Aborting..." << endl;
		return -1;
	}
	// Retrieve the integer value from the entry node
	const int64_t streamModeCustom = ptrStreamModeCustom->GetValue();

	// Set integer as new value for enumeration node
	ptrStreamMode->SetIntValue(streamModeCustom);

	// Print out the current stream mode
	cout << endl
		 << "Stream Mode set to " + ptrStreamMode->GetCurrentEntry()->GetSymbolic() << "..." << endl;

	return 0;
}

// 降低延迟：只保留最新帧 + 减少缓冲数量；并打印实际帧率和曝光时间便于排查
// 必须在 BeginAcquisition() 之前调用
static void ConfigureLowLatency(CameraPtr pCam, INodeMap &nodeMap)
{
	try
	{
		INodeMap &sNodeMap = pCam->GetTLStreamNodeMap();

		// 1) 缓冲区处理模式：NewestOnly = 只给最新一帧，旧帧直接丢弃
		//    默认 OldestFirst 会按顺序给旧帧，处理速度跟不上时队列堆积，画面就越来越滞后
		CEnumerationPtr ptrHandling = sNodeMap.GetNode("StreamBufferHandlingMode");
		if (IsReadable(ptrHandling) && IsWritable(ptrHandling))
		{
			CEnumEntryPtr ptrNewestOnly = ptrHandling->GetEntryByName("NewestOnly");
			if (IsReadable(ptrNewestOnly))
			{
				ptrHandling->SetIntValue(ptrNewestOnly->GetValue());
				cout << "StreamBufferHandlingMode set to NewestOnly..." << endl;
			}
		}

		// 2) 缓冲区数量改成手动并设小一点，进一步限制排队深度
		CEnumerationPtr ptrCountMode = sNodeMap.GetNode("StreamBufferCountMode");
		if (IsReadable(ptrCountMode) && IsWritable(ptrCountMode))
		{
			CEnumEntryPtr ptrManual = ptrCountMode->GetEntryByName("Manual");
			if (IsReadable(ptrManual))
			{
				ptrCountMode->SetIntValue(ptrManual->GetValue());

				CIntegerPtr ptrCount = sNodeMap.GetNode("StreamBufferCountManual");
				if (IsReadable(ptrCount) && IsWritable(ptrCount))
				{
					int64_t n = 3;
					if (n < ptrCount->GetMin())
						n = ptrCount->GetMin();
					if (n > ptrCount->GetMax())
						n = ptrCount->GetMax();
					ptrCount->SetValue(n);
					cout << "StreamBufferCountManual set to " << n << "..." << endl;
				}
			}
		}

		// 3) 打印实际帧率和曝光时间：曝光时间太长会直接拉低帧率，看起来就像"延迟"
		CFloatPtr ptrFps = nodeMap.GetNode("AcquisitionResultingFrameRate");
		if (IsReadable(ptrFps))
		{
			cout << "Resulting frame rate: " << ptrFps->GetValue() << " fps" << endl;
		}
		CFloatPtr ptrExp = nodeMap.GetNode("ExposureTime");
		if (IsReadable(ptrExp))
		{
			cout << "Exposure time: " << ptrExp->GetValue() / 1000.0 << " ms" << endl;
		}
	}
	catch (Spinnaker::Exception &e)
	{
		cout << "ConfigureLowLatency warning: " << e.what() << endl;
	}
}

// 实时预览，空格保存，退格删除上一张，ESC 退出
int AcquireImages(CameraPtr pCam, INodeMap &nodeMap, INodeMap & /*nodeMapTLDevice*/)
{
	int result = 0;

	try
	{
		int group_id = 0;
		std::string group_name;

		std::cout << "Image filename prefix (default: empty):";
		std::getline(std::cin, group_name);
		group_name = TrimPath(group_name);
		if (group_name.empty())
		{
			std::cout << "Use default empty prefix" << std::endl;
		}

		std::cout << "Starting image index ID:";
		std::cin >> group_id;
		if (!std::cin)
		{
			std::cin.clear();
			group_id = 0;
			std::cout << "Invalid index, use 0" << std::endl;
		}
		std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

		// 用 getline 读取，路径中的空格不会被截断
		std::string save_folder;
		std::cout << "Image save folder path:";
		std::getline(std::cin, save_folder);
		save_folder = TrimPath(save_folder);
		if (save_folder.empty())
		{
			save_folder = ".";
			std::cout << "Use current folder" << std::endl;
		}

		// 目录不存在则创建
		{
			std::error_code ec;
			fs::create_directories(fs::path(save_folder), ec);
			if (ec)
			{
				cout << "Cannot create folder: " << save_folder << " (" << ec.message() << ")" << endl;
				return -1;
			}
		}

		// 设置采集模式为连续
		CEnumerationPtr ptrAcquisitionMode = nodeMap.GetNode("AcquisitionMode");
		CEnumEntryPtr ptrAcquisitionModeContinuous = ptrAcquisitionMode->GetEntryByName("Continuous");
		const int64_t acquisitionModeContinuous = ptrAcquisitionModeContinuous->GetValue();
		ptrAcquisitionMode->SetIntValue(acquisitionModeContinuous);
		cout << "Acquisition mode set to continuous..." << endl;

		// 降低延迟（必须在 BeginAcquisition 之前）
		ConfigureLowLatency(pCam, nodeMap);

		// 启动采集；守卫对象保证离开作用域时一定 EndAcquisition
		pCam->BeginAcquisition();
		AcquisitionGuard acqGuard(pCam);
		cout << "Start acquiring images (press ESC to exit)..." << endl;

		// 初始化图像处理器
		ImageProcessor processor;
		processor.SetColorProcessing(SPINNAKER_COLOR_PROCESSING_ALGORITHM_HQ_LINEAR);

		cv::namedWindow("Live View", cv::WINDOW_AUTOSIZE);

		auto make_filename = [&](int id)
		{
			std::ostringstream filename;
			if (group_name.empty())
				filename << save_folder << "/" << id << ".png";
			else
				filename << save_folder << "/" << group_name << "_" << id << ".png";
			return filename.str();
		};

		int consecutiveErrors = 0;
		const int maxConsecutiveErrors = 50;

		while (!g_stopRequested)
		{
			ImagePtr pResultImage = nullptr;
			ImagePtr convertedImage = nullptr;
			cv::Mat cvImage;
			bool hasFrame = false;

			try
			{
				pResultImage = pCam->GetNextImage(50);
				ImageReleaser releaser(pResultImage); // 本轮结束必定 Release

				if (pResultImage->IsIncomplete())
				{
					cout << "Image incomplete: "
						 << Image::GetImageStatusDescription(pResultImage->GetImageStatus()) << endl;
				}
				else
				{
					// 已经是 Mono8 就直接用，省掉一次 2048x2048 的格式转换和拷贝
					if (pResultImage->GetPixelFormat() == PixelFormat_Mono8)
					{
						convertedImage = pResultImage;
					}
					else
					{
						convertedImage = processor.Convert(pResultImage, PixelFormat_Mono8);
					}

					// 包装为 OpenCV Mat（不拷贝，数据由 convertedImage 持有）
					cvImage = cv::Mat(
						static_cast<int>(convertedImage->GetHeight()),
						static_cast<int>(convertedImage->GetWidth()),
						CV_8UC1,
						convertedImage->GetData());

					if (!cvImage.empty())
					{
						cv::Mat resizedImage;
						// 预览用最近邻插值，比默认双线性更快
						cv::resize(cvImage, resizedImage, cv::Size(), 0.2, 0.2, cv::INTER_NEAREST);
						cv::imshow("Live View", resizedImage);
						hasFrame = true;
					}
					else
					{
						std::cerr << "cvImage is empty!" << std::endl;
					}
				}

				consecutiveErrors = 0;

				// 键盘处理放在 releaser 作用域内：保存时 cvImage 仍然有效
				const int key = cv::waitKey(1);

				if (key == 27) // ESC
				{
					cout << "ESC pressed, exiting..." << endl;
					break;
				}
				else if (key == 32) // 空格：保存
				{
					if (hasFrame)
					{
						if (cv::imwrite(make_filename(group_id), cvImage))
						{
							cout << "Saved: " << group_id << endl;
							group_id++;
						}
						else
						{
							cout << "Failed to save: " << make_filename(group_id) << endl;
						}
					}
					else
					{
						cout << "No valid frame to save" << endl;
					}
				}
				else if (key == 8 || key == 127) // 退格/Delete：删除上一张
				{
					if (group_id > 0)
					{
						const int delete_id = group_id - 1;
						const std::string path = make_filename(delete_id);
						std::error_code ec;
						if (fs::exists(path, ec))
						{
							fs::remove(path, ec);
							cout << "Deleted: " << path << endl;
							group_id--;
						}
						else
						{
							cout << "No such file to delete: " << path << endl;
						}
					}
					else
					{
						cout << "No images to delete" << endl;
					}
				}
			}
			catch (Spinnaker::Exception &e)
			{
				// 超时属于正常情况（暂时没有新帧），不打印，但仍要响应 ESC
				if (e.GetError() == SPINNAKER_ERR_TIMEOUT)
				{
					if (cv::waitKey(1) == 27)
					{
						cout << "ESC pressed, exiting..." << endl;
						break;
					}
					continue;
				}

				cout << "Image error: " << e.what() << endl;
				if (++consecutiveErrors >= maxConsecutiveErrors)
				{
					cout << "Too many consecutive errors, stop acquisition." << endl;
					result = -1;
					break;
				}
			}
			catch (cv::Exception &e)
			{
				std::cerr << "OpenCV error: " << e.what() << std::endl;
				result = -1;
				break;
			}
		}

		if (g_stopRequested)
		{
			cout << "Stop requested (console closed / Ctrl+C), cleaning up..." << endl;
		}

		cv::destroyAllWindows();
		// acqGuard 析构时自动 EndAcquisition
	}
	catch (Spinnaker::Exception &e)
	{
		cout << "Error: " << e.what() << endl;
		result = -1;
	}
	catch (std::exception &e)
	{
		cout << "Error: " << e.what() << endl;
		result = -1;
	}

	return result;
}

int PrintDeviceInfo(INodeMap &nodeMap)
{
	int result = 0;
	cout << endl
		 << "*** DEVICE INFORMATION ***" << endl
		 << endl;

	try
	{
		FeatureList_t features;
		const CCategoryPtr category = nodeMap.GetNode("DeviceInformation");
		if (IsReadable(category))
		{
			category->GetFeatures(features);

			for (auto it = features.begin(); it != features.end(); ++it)
			{
				const CNodePtr pfeatureNode = *it;
				cout << pfeatureNode->GetName() << " : ";
				CValuePtr pValue = static_cast<CValuePtr>(pfeatureNode);
				cout << (IsReadable(pValue) ? pValue->ToString() : "Node not readable");
				cout << endl;
			}
		}
		else
		{
			cout << "Device control information not available." << endl;
		}
	}
	catch (Spinnaker::Exception &e)
	{
		cout << "Error: " << e.what() << endl;
		result = -1;
	}

	return result;
}

int RunSingleCamera(CameraPtr pCam)
{
	int result = 0;

	try
	{
		// Retrieve TL device nodemap and print device information
		INodeMap &nodeMapTLDevice = pCam->GetTLDeviceNodeMap();
		result = PrintDeviceInfo(nodeMapTLDevice);

		// Initialize camera；守卫对象保证任何路径都会 DeInit
		pCam->Init();
		CameraInitGuard initGuard(pCam);

		// Retrieve GenICam nodemap
		INodeMap &nodeMap = pCam->GetNodeMap();

		//===========================================================================
		// 设置分辨率为 2048x2048（居中）
		CIntegerPtr ptrWidth = nodeMap.GetNode("Width");
		CIntegerPtr ptrHeight = nodeMap.GetNode("Height");
		CIntegerPtr ptrOffsetX = nodeMap.GetNode("OffsetX");
		CIntegerPtr ptrOffsetY = nodeMap.GetNode("OffsetY");

		if (IsWritable(ptrWidth) && IsWritable(ptrHeight))
		{
			// 先把偏移归零，避免 偏移+宽度 超出传感器范围导致设置宽高失败
			if (IsWritable(ptrOffsetX) && IsWritable(ptrOffsetY))
			{
				ptrOffsetX->SetValue(0);
				ptrOffsetY->SetValue(0);
			}

			ptrWidth->SetValue(2048);
			ptrHeight->SetValue(2048);
			cout << "Resolution set to 2048x2048..." << endl;

			// 居中偏移：(2448 - 2048) / 2 = 200, (2048 - 2048) / 2 = 0
			if (IsWritable(ptrOffsetX) && IsWritable(ptrOffsetY))
			{
				ptrOffsetX->SetValue(200);
				ptrOffsetY->SetValue(0);
				cout << "Offset set to X: " << ptrOffsetX->GetValue() << ", Y: " << ptrOffsetY->GetValue() << "..." << endl;
			}
			else
			{
				cout << "Warning: OffsetX or OffsetY not writable, ROI may not be centered." << endl;
			}
		}
		else
		{
			cout << "Error: Width or Height not writable, cannot set resolution to 2048x2048." << endl;
			result = -1;
		}
		//===========================================================================

		// Set stream mode
		result = result | SetStreamMode(pCam);

		// Acquire images
		result = result | AcquireImages(pCam, nodeMap, nodeMapTLDevice);

		// 离开作用域时 initGuard 自动 DeInit
	}
	catch (Spinnaker::Exception &e)
	{
		cout << "Error: " << e.what() << endl;
		result = -1;
		g_cleanupDone = true; // Init 之前就失败时，也要让关闭回调不再等待
	}

	return result;
}

int main(int /*argc*/, char ** /*argv*/)
{
#if defined(_WIN32)
	// 注册控制台事件：点 X 关闭窗口也能正常释放相机
	SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif

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

		result = RunSingleCamera(pCam);

		cout << "Camera 0 example complete." << endl;

		pCam = nullptr;
	}

	// Clear camera list before releasing system
	camList.Clear();

	// Release system
	system->ReleaseInstance();

	// 如果是被关闭信号触发的退出，不再等待回车
	if (!g_stopRequested)
	{
		cout << endl
			 << "Done! Press Enter to exit..." << endl;
		getchar();
	}

	return result;
}