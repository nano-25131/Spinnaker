#pragma once
// 抗锯齿文字渲染：Windows 下用 GDI（Segoe UI）渲染，字形比 OpenCV 自带 Hershey 字体精致得多；
// 其它平台退回 Hershey。只能在主线程调用。
#include <string>
#include <opencv2/core.hpp>

struct TextMetrics
{
	int width = 0;
	int ascent = 0;	 // 基线以上
	int descent = 0; // 基线以下
};

// px = 字号（像素，约等于 em 高度）；bold = 半粗体
TextMetrics MeasureText(const std::string &utf8, int px, bool bold);

// 在 baseline 处画文字（baseline 为文字基线左端），img 必须是 8UC3。alpha 0~1。返回文字宽度。
int DrawTextAA(cv::Mat &img, const std::string &utf8, cv::Point baseline, int px, bool bold,
			   const cv::Scalar &color, double alpha = 1.0);