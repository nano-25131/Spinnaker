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
#include "guards.h"
#include "overlay.h"
#include "text_utils.h"

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;
using namespace std;

// ============================================================================
// 采集主循环：实时预览，空格保存，退格删除本次运行保存的上一张，ESC 退出
// ============================================================================
int AcquireImages(CameraPtr pCam, INodeMap &nodeMap, const Config &cfg)
{
	int result = 0;

	try
	{
		const fs::path prefixPath = Utf8ToPath(cfg.prefix);
		const fs::path saveFolder = Utf8ToPath(cfg.saveFolder);

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
		std::vector<long long> savedIds; // 本次运行保存过的编号（退格只删这些，不会误删旧文件）
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

		// 平滑后的实际帧率
		double fps = 0.0;
		bool haveLastFrame = false;
		std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();

		// 启动采集；守卫对象保证离开作用域时一定 EndAcquisition
		pCam->BeginAcquisition();
		AcquisitionGuard acqGuard(pCam);
		cout << "Start acquiring images (SPACE = save, BACKSPACE = delete last, ESC = exit)..." << endl;

		// 初始化图像处理器
		ImageProcessor processor;
		processor.SetColorProcessing(SPINNAKER_COLOR_PROCESSING_ALGORITHM_HQ_LINEAR);

		cv::namedWindow("Live View", cv::WINDOW_AUTOSIZE);

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
					cout << "Write OK: " << PathToUtf8(MakeImagePath(saveFolder, prefixPath, r.id)) << endl;
				}
				else if (r.isDelete)
				{
					setStatus("DELETE FAILED #" + idStr + " (" + r.reason + ")", kRed, 6.0);
					cout << "Delete failed for #" << idStr << " (file written, then removal failed)" << endl;
				}
				else
				{
					// 写盘失败：这张图并不存在，从"本次已保存"列表里去掉，避免退格去删一个不存在的文件
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
						cv::resize(cvImage, resizedImage, cv::Size(), cfg.previewScale, cfg.previewScale, cv::INTER_NEAREST);
						cv::cvtColor(resizedImage, previewBgr, cv::COLOR_GRAY2BGR);

						std::vector<std::string> lines;
						lines.push_back("Next #" + std::to_string(group_id) +
										"   Saved " + std::to_string(savedIds.size()) +
										"   Writing " + std::to_string(writer.PendingCount()) +
										"   Failed " + std::to_string(failedCount));
						{
							std::ostringstream oss;
							oss << "FPS " << std::fixed << std::setprecision(1) << fps
								<< "   " << cvImage.cols << "x" << cvImage.rows;
							lines.push_back(oss.str());
						}

						const bool showMsg = !status.text.empty() && std::chrono::steady_clock::now() < status.until;
						DrawOverlay(previewBgr, lines, showMsg ? &status : nullptr);

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
				const int key = cv::waitKey(1);

				if (key == 27) // ESC
				{
					cout << "ESC pressed, exiting..." << endl;
					break;
				}
				else if (key == 32) // 空格：保存
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
							++group_id;
						}
						else
						{
							cout << "Save queue full, disk too slow. Wait a moment and try again." << endl;
							setStatus("QUEUE FULL - wait a moment, then retry", kRed, 3.0);
						}
					}
				}
				else if (key == 8 || key == 127) // 退格/Delete：删除本次运行保存的上一张
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
