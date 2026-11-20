#pragma once
// 预览叠加：状态信息 + 保存/删除结果提示
// OpenCV 自带字体只支持 ASCII，所以叠加到画面上的文字一律用英文（不放路径）。
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

inline const cv::Scalar kGreen = cv::Scalar(0, 200, 0);
inline const cv::Scalar kRed = cv::Scalar(0, 0, 255);
inline const cv::Scalar kYellow = cv::Scalar(0, 220, 255);
inline const cv::Scalar kWhite = cv::Scalar(255, 255, 255);
inline const cv::Scalar kGray = cv::Scalar(190, 190, 190);

// 带黑色描边的文字，任何背景下都看得清；文字太宽时自动缩小字号
void PutTextOutlined(cv::Mat &img, const std::string &text, cv::Point org, double scale, const cv::Scalar &color);

// 左上角状态行；底部：操作提示 + 最近一次保存/删除结果
void DrawOverlay(cv::Mat &img, const std::vector<std::string> &topLines, const StatusMessage *msg);
