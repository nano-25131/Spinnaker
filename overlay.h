#pragma once
// 预览叠加（极简风格）：左上状态条、底部按键提示、结果提示条、保存闪光
// 叠加文字一律用英文、不放路径（非 Windows 平台退回的 OpenCV 字体只支持 ASCII）。
#include <chrono>
#include <string>
#include <vector>
#include <opencv2/core.hpp>

struct StatusMessage
{
	std::string text;
	cv::Scalar color = cv::Scalar(255, 255, 255);
	std::chrono::steady_clock::time_point until{};
};

// 颜色均为 BGR
inline const cv::Scalar kGreen = cv::Scalar(120, 200, 80);
inline const cv::Scalar kRed = cv::Scalar(90, 90, 240);
inline const cv::Scalar kYellow = cv::Scalar(70, 190, 250);
inline const cv::Scalar kWhite = cv::Scalar(245, 245, 245);
inline const cv::Scalar kGray = cv::Scalar(175, 175, 175);
// 强调色（BGR，现为琥珀色；极简版界面已不使用，保留以便以后扩展）
inline const cv::Scalar kAccent = cv::Scalar(0, 176, 255);

// HUD 面板要显示的数据
struct HudInfo
{
	long long nextId = 0; // 下一张的编号
	size_t saved = 0;	  // 当前目录本次已保存张数
	size_t writing = 0;	  // 正在写盘 / 排队的张数
	int failed = 0;		  // 写盘失败次数
	double fps = 0.0;
	int width = 0, height = 0; // 原图分辨率
};

// 在缩小后的预览图上叠加全部界面元素。
// flash: 保存闪光强度 0~1（0 = 无），由调用方随时间衰减
void DrawOverlay(cv::Mat &img, const HudInfo &info, const StatusMessage *msg, double flash);