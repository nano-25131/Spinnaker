#include "acquisition.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

#include "async_writer.h"
#include "camera_setup.h"
#include "console_ctrl.h"
#include "file_naming.h"
#include "folder_dialog.h"
#include "guards.h"
#include "overlay.h"
#include "text_utils.h"

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;
using namespace std;

// 预览窗口里的鼠标操作：左键 = 保存（同空格），右键 = 删除上一张（同退格）。
// 回调只记录请求，由主循环在 cv::waitKey 之后统一处理。
// 回调只会在鼠标位于画面内时触发；双击会产生 DBLCLK 事件而不是第二次 DOWN，所以一并处理，快速连点不丢。
struct MouseRequests
{
	bool save = false;
	bool del = false;
};

static void OnPreviewMouse(int event, int, int, int, void *userdata)
{
	MouseRequests *m = static_cast<MouseRequests *>(userdata);
	if (m == nullptr)
		return;
	if (event == cv::EVENT_LBUTTONDOWN || event == cv::EVENT_LBUTTONDBLCLK)
		m->save = true;
	else if (event == cv::EVENT_RBUTTONDOWN || event == cv::EVENT_RBUTTONDBLCLK)
		m->del = true;
}

// ============================================================================
// 采集主循环：实时预览，空格/左键保存，退格/右键删除本次运行保存的上一张，ESC 退出
// ============================================================================
int AcquireImages(CameraPtr pCam, INodeMap &nodeMap, const Config &cfg)
{
	int result = 0;

	try
	{
		const fs::path prefixPath = Utf8ToPath(cfg.prefix);
		fs::path saveFolder = Utf8ToPath(cfg.saveFolder); // 运行中可用 F 键切换

		// 目录不存在则创建
		{
			std::error_code ec;
			fs::create_directories(saveFolder, ec);
			if (ec)
			{
				cout << "Cannot create folder: " << cfg.saveFolder << " (" << ec.message() << ")" << endl;
				return -1;
			}
		}

		// 起始编号：auto = 已有最大编号 + 1
		long long group_id = 0;
		if (cfg.autoIndex)
		{
			const long long maxId = ScanMaxIndex(saveFolder, prefixPath);
			group_id = maxId + 1;
			cout << "Auto index: existing max = " << (maxId < 0 ? std::string("none") : std::to_string(maxId))
				 << ", start from " << group_id << endl;
		}
		else
		{
			group_id = cfg.startIndex;
			cout << "Start index (manual): " << group_id << endl;
		}
		cout << "Saving to: " << PathToUtf8(fs::absolute(saveFolder)) << endl;

		// 设置采集模式为连续
		CEnumerationPtr ptrAcquisitionMode = nodeMap.GetNode("AcquisitionMode");
		CEnumEntryPtr ptrAcquisitionModeContinuous = ptrAcquisitionMode->GetEntryByName("Continuous");
		const int64_t acquisitionModeContinuous = ptrAcquisitionModeContinuous->GetValue();
		ptrAcquisitionMode->SetIntValue(acquisitionModeContinuous);
		cout << "Acquisition mode set to continuous..." << endl;

		// 降低延迟（必须在 BeginAcquisition 之前）
		ConfigureLowLatency(pCam, nodeMap, cfg.bufferCount);

		// 后台写盘线程。声明在 acqGuard 之前 => 析构顺序是：先 EndAcquisition，再把队列刷完
		AsyncImageWriter writer(static_cast<size_t>(cfg.queueSize), cfg.pngCompression);
		std::vector<long long> savedIds; // 当前保存目录下、本次运行保存过的编号（退格只删这些；切换目录时清空）
		int failedCount = 0;			 // 本次运行写盘失败的次数

		// 预览画面上的提示信息
		StatusMessage status;
		auto setStatus = [&](const std::string &text, const cv::Scalar &color, double seconds)
		{
			status.text = text;
			status.color = color;
			status.until = std::chrono::steady_clock::now() +
						   std::chrono::milliseconds(static_cast<int>(seconds * 1000));
		};

		// 预览缩放比例：运行中可用 + / - 切换（只影响预览，不影响保存的原图）
		double previewScale = cfg.previewScale;
		auto stepPreviewScale = [&](int dir)
		{
			static const double kSteps[] = {0.10, 0.15, 0.20, 0.25, 0.30, 0.40, 0.50, 0.60, 0.75, 1.00};
			const int n = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));
			double target = previewScale; // 找不到更大/更小的档位 = 已到头，保持不变
			if (dir > 0)
			{
				for (int i = 0; i < n; ++i)
					if (kSteps[i] > previewScale + 1e-6)
					{
						target = kSteps[i];
						break;
					}
			}
			else
			{
				for (int i = n - 1; i >= 0; --i)
					if (kSteps[i] < previewScale - 1e-6)
					{
						target = kSteps[i];
						break;
					}
			}
			return target;
		};

		// 平滑后的实际帧率
		double fps = 0.0;
		bool haveLastFrame = false;
		bool hasSaveFlash = false; // 保存闪光用：记录最近一次成功入队的时间
		std::chrono::steady_clock::time_point lastSaveTime;
		std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();

		// 启动采集；守卫对象保证离开作用域时一定 EndAcquisition
		pCam->BeginAcquisition();
		AcquisitionGuard acqGuard(pCam);
		cout << "Start acquiring images (SPACE / left click = save, BACKSPACE / right click = delete last, F = change folder, +/- = preview size, ESC = exit)..." << endl;

		// 初始化图像处理器
		ImageProcessor processor;
		processor.SetColorProcessing(SPINNAKER_COLOR_PROCESSING_ALGORITHM_HQ_LINEAR);

		cv::namedWindow("Live View", cv::WINDOW_AUTOSIZE);
		MouseRequests mouse; // 必须比窗口活得久：声明在 destroyAllWindows 之前的同一作用域
		cv::setMouseCallback("Live View", OnPreviewMouse, &mouse);

		// 记录预览窗口是否已显示过；关闭后 WND_PROP_VISIBLE < 1，避免下帧 imshow 把窗口重新弹出
		bool windowOpened = false;
		auto isWindowClosed = [&]() -> bool
		{
			try
			{
				const double vis = cv::getWindowProperty("Live View", cv::WND_PROP_VISIBLE);
				if (vis >= 1.0)
				{
					windowOpened = true;
					return false;
				}
				return windowOpened;
			}
			catch (const cv::Exception &)
			{
				return windowOpened;
			}
		};
		isWindowClosed();

		// 该编号对应的文件是否已被占用（磁盘上已有，或还在写盘队列里）
		auto isTaken = [&](long long id)
		{
			const fs::path p = MakeImagePath(saveFolder, prefixPath, id);
			std::error_code ec;
			return writer.IsPending(p) || fs::exists(p, ec);
		};

		int consecutiveErrors = 0;
		const int maxConsecutiveErrors = 50;

		while (!g_stopRequested)
		{
			// 取走后台线程已完成的写盘结果：成功 -> 绿色提示；失败 -> 红色提示
			for (const auto &r : writer.TakeResults())
			{
				const std::string idStr = std::to_string(r.id);
				if (r.ok)
				{
					setStatus("Saved OK  #" + idStr, kGreen, 2.5);
					cout << "Write OK: " << PathToUtf8(r.path) << endl;
				}
				else if (r.isDelete)
				{
					setStatus("DELETE FAILED #" + idStr + " (" + r.reason + ")", kRed, 6.0);
					cout << "Delete failed for #" << idStr << " (file written, then removal failed)" << endl;
				}
				else
				{
					// 写盘失败：这张图并不存在，从"本次已保存"列表里去掉，避免退格去删一个不存在的文件。
					// 只有它属于当前保存目录时才处理：切换目录前提交的任务失败，不能误删新目录里同编号的记录
					if (r.path == MakeImagePath(saveFolder, prefixPath, r.id))
						savedIds.erase(std::remove(savedIds.begin(), savedIds.end(), r.id), savedIds.end());
					++failedCount;
					setStatus("SAVE FAILED #" + idStr + " (" + r.reason + ")", kRed, 6.0);
					cout << "Write FAILED: #" << idStr << " (" << r.reason << ")" << endl;
				}
			}

			ImagePtr pResultImage = nullptr;
			ImagePtr convertedImage = nullptr;
			cv::Mat cvImage;
			bool hasFrame = false;
			bool pickFolderRequested = false; // F 键：帧释放之后再弹对话框，避免对话框期间占着相机缓冲区

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
					// 已经是 Mono8 就直接用，省掉一次格式转换和拷贝
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
						// 帧率（指数平滑）
						const auto now = std::chrono::steady_clock::now();
						if (haveLastFrame)
						{
							const double dt = std::chrono::duration<double>(now - lastFrameTime).count();
							if (dt > 0.0)
							{
								const double inst = 1.0 / dt;
								fps = (fps <= 0.0) ? inst : 0.9 * fps + 0.1 * inst;
							}
						}
						lastFrameTime = now;
						haveLastFrame = true;

						// 预览用最近邻插值，比默认双线性更快；叠加文字只画在缩小后的预览图上，保存的原图不受影响
						cv::Mat resizedImage, previewBgr;
						cv::resize(cvImage, resizedImage, cv::Size(), previewScale, previewScale, cv::INTER_NEAREST);
						cv::cvtColor(resizedImage, previewBgr, cv::COLOR_GRAY2BGR);

						HudInfo hud;
						hud.nextId = group_id;
						hud.saved = savedIds.size();
						hud.writing = writer.PendingCount();
						hud.failed = failedCount;
						hud.fps = fps;
						hud.width = cvImage.cols;
						hud.height = cvImage.rows;

						// 保存闪光：入队那一刻起 0.25 秒内线性衰减
						double flash = 0.0;
						if (hasSaveFlash)
						{
							const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - lastSaveTime).count();
							flash = std::max(0.0, 1.0 - el / 0.25);
						}

						const bool showMsg = !status.text.empty() && std::chrono::steady_clock::now() < status.until;
						DrawOverlay(previewBgr, hud, showMsg ? &status : nullptr, flash);

						cv::imshow("Live View", previewBgr);
						hasFrame = true;
					}
					else
					{
						std::cerr << "cvImage is empty!" << std::endl;
					}
				}

				consecutiveErrors = 0;

				// 键盘处理放在 releaser 作用域内：保存时 cvImage 仍然有效
				const int key = cv::waitKey(1); // 同时会触发鼠标回调
				const bool wantSave = (key == 32) || mouse.save;
				const bool wantDelete = (key == 8 || key == 127) || mouse.del;
				mouse.save = mouse.del = false;
				if (isWindowClosed())
				{
					cout << "Preview window closed, exiting..." << endl;
					break;
				}

				if (key == 27) // ESC
				{
					cout << "ESC pressed, exiting..." << endl;
					break;
				}
				else if (wantSave) // 空格 / 鼠标左键：保存
				{
					if (!hasFrame)
					{
						cout << "No valid frame to save" << endl;
						setStatus("NO VALID FRAME - not saved", kRed, 2.5);
					}
					else if (isTaken(group_id))
					{
						// 拒绝覆盖：跳到下一个空闲编号，让用户再按一次空格
						const long long taken = group_id;
						while (isTaken(group_id))
							++group_id;
						cout << "Refuse to overwrite existing file (index " << taken
							 << "), next index is now " << group_id << ". Press SPACE again to save." << endl;
						setStatus("REFUSED: #" + std::to_string(taken) + " exists. Next #" +
									  std::to_string(group_id) + ", press SPACE again",
								  kRed, 4.0);
					}
					else
					{
						// 主线程只做一次内存拷贝（~4 MB，约 1ms）；编码和写盘交给后台线程
						SaveJob job;
						job.path = MakeImagePath(saveFolder, prefixPath, group_id);
						job.id = group_id;
						job.image = cvImage.clone();
						if (writer.Enqueue(std::move(job)))
						{
							cout << "Queued: " << group_id << " (writing " << writer.PendingCount() << ")" << endl;
							setStatus("Saving #" + std::to_string(group_id) + " ...", kYellow, 1.0);
							savedIds.push_back(group_id);
							hasSaveFlash = true;
							lastSaveTime = std::chrono::steady_clock::now();
							++group_id;
						}
						else
						{
							cout << "Save queue full, disk too slow. Wait a moment and try again." << endl;
							setStatus("QUEUE FULL - wait a moment, then retry", kRed, 3.0);
						}
					}
				}
				else if (key == '+' || key == '=' || key == '-' || key == '_') // 预览放大 / 缩小
				{
					const bool bigger = (key == '+' || key == '=');
					const double target = stepPreviewScale(bigger ? 1 : -1);
					const long pct = std::lround((target) * 100.0);
					if (target == previewScale)
					{
						setStatus(std::string("Preview already at ") + (bigger ? "max " : "min ") + std::to_string(pct) + "%",
								  kYellow, 1.5);
					}
					else
					{
						previewScale = target;
						cout << "Preview scale: " << pct << "%" << endl;
						setStatus("Preview " + std::to_string(pct) + "%", kGreen, 1.5);
					}
				}
				else if (key == 'f' || key == 'F') // 切换保存目录（在循环末尾处理）
				{
					pickFolderRequested = true;
				}
				else if (wantDelete) // 退格 / Delete / 鼠标右键：删除本次运行保存的上一张
				{
					if (savedIds.empty())
					{
						cout << "No image saved in this session to delete" << endl;
						setStatus("NOTHING TO DELETE (this session)", kYellow, 2.5);
					}
					else
					{
						const long long delete_id = savedIds.back();
						const std::string idStr = std::to_string(delete_id);
						const fs::path path = MakeImagePath(saveFolder, prefixPath, delete_id);

						const auto r = writer.Cancel(path);
						if (r != AsyncImageWriter::CancelResult::NotPending)
						{
							// 还在队列里（直接撤销）或正在写（写完后自动删除，失败会再提示）
							cout << "Deleted (pending save cancelled): " << PathToUtf8(path) << endl;
							savedIds.pop_back();
							group_id = delete_id; // 编号回退，下一张重新用这个编号
							setStatus("Deleted #" + idStr, kGreen, 2.5);
						}
						else
						{
							std::error_code ec;
							if (!fs::exists(path, ec))
							{
								cout << "No such file to delete: " << PathToUtf8(path) << endl;
								savedIds.pop_back();
								setStatus("DELETE FAILED #" + idStr + " (file not found)", kRed, 4.0);
							}
							else if (fs::remove(path, ec) && !ec)
							{
								cout << "Deleted: " << PathToUtf8(path) << endl;
								savedIds.pop_back();
								group_id = delete_id;
								setStatus("Deleted #" + idStr, kGreen, 2.5);
							}
							else
							{
								cout << "Failed to delete: " << PathToUtf8(path) << " (" << ec.message() << ")" << endl;
								setStatus("DELETE FAILED #" + idStr + " (cannot remove file)", kRed, 4.0);
							}
						}
					}
				}
			}
			catch (Spinnaker::Exception &e)
			{
				// 超时属于正常情况（暂时没有新帧），不打印，但仍要响应 ESC 和关闭预览窗口
				if (e.GetError() == SPINNAKER_ERR_TIMEOUT)
				{
					if (cv::waitKey(1) == 27)
					{
						cout << "ESC pressed, exiting..." << endl;
						break;
					}
					if (isWindowClosed())
					{
						cout << "Preview window closed, exiting..." << endl;
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

			// 切换保存目录：此时本轮的帧已经 Release。对话框期间主循环暂停，相机端只保留最新帧，关闭后画面立即恢复
			if (pickFolderRequested)
			{
				const fs::path chosen = PickFolder(saveFolder);
				haveLastFrame = false; // 对话框期间没有取帧，帧率统计重新开始
				fps = 0.0;

				if (chosen.empty())
				{
					cout << "Folder selection cancelled, still saving to: "
						 << PathToUtf8(fs::absolute(saveFolder)) << endl;
					setStatus("Folder unchanged", kYellow, 2.0);
				}
				else
				{
					std::error_code ec;
					if (fs::equivalent(chosen, saveFolder, ec) && !ec)
					{
						cout << "Same folder selected, nothing changed." << endl;
						setStatus("Folder unchanged", kYellow, 2.0);
					}
					else
					{
						saveFolder = chosen;
						savedIds.clear(); // 不跨目录撤销：退格只能删当前目录里本次保存的图
						if (cfg.autoIndex)
						{
							const long long maxId = ScanMaxIndex(saveFolder, prefixPath);
							group_id = maxId + 1;
							cout << "Folder changed. Existing max index = "
								 << (maxId < 0 ? std::string("none") : std::to_string(maxId))
								 << ", next index = " << group_id << endl;
						}
						else
						{
							cout << "Folder changed. Keep current index " << group_id << " (manual start_index)." << endl;
						}
						cout << "Saving to: " << PathToUtf8(fs::absolute(saveFolder)) << endl;
						setStatus("Folder changed - next #" + std::to_string(group_id), kGreen, 3.0);
					}
				}
			}
		}

		if (g_stopRequested)
		{
			cout << "Stop requested (console closed / Ctrl+C), cleaning up..." << endl;
		}

		cv::destroyAllWindows();
		// 离开作用域：acqGuard 先 EndAcquisition，writer 再把队列里剩余的图片写完
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

int RunSingleCamera(CameraPtr pCam, const Config &cfg)
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

		// ROI 必须先于曝光设置：曝光上限取决于帧率，帧率又取决于 ROI
		if (!ConfigureRoi(nodeMap, cfg))
			result = -1;
		ApplyAutoOrValue(nodeMap, "ExposureAuto", "ExposureTime", cfg.exposure, "Exposure (us)");
		ApplyAutoOrValue(nodeMap, "GainAuto", "Gain", cfg.gain, "Gain (dB)");

		// Set stream mode
		result = result | SetStreamMode(pCam);

		// Acquire images
		result = result | AcquireImages(pCam, nodeMap, cfg);

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