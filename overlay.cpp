#include "overlay.h"
#include <algorithm>
#include <opencv2/opencv.hpp>

void PutTextOutlined(cv::Mat &img, const std::string &text, cv::Point org, double scale, const cv::Scalar &color)
{
	const int font = cv::FONT_HERSHEY_SIMPLEX;
	int baseline = 0;
	const int maxWidth = img.cols - org.x - 4;
	const cv::Size sz = cv::getTextSize(text, font, scale, 1, &baseline);
	if (maxWidth > 0 && sz.width > maxWidth)
		scale *= static_cast<double>(maxWidth) / sz.width;
	cv::putText(img, text, org, font, scale, cv::Scalar(0, 0, 0), 3, cv::LINE_AA);
	cv::putText(img, text, org, font, scale, color, 1, cv::LINE_AA);
}

void DrawOverlay(cv::Mat &img, const std::vector<std::string> &topLines, const StatusMessage *msg)
{
	const double s = std::max(0.4, std::min(1.0, img.cols / 800.0));
	const int lineH = static_cast<int>(34 * s);

	int y = lineH;
	for (const auto &line : topLines)
	{
		PutTextOutlined(img, line, cv::Point(8, y), s, kWhite);
		y += lineH;
	}

	const int bottomY = img.rows - 8;
	PutTextOutlined(img, "SPACE save | BKSP delete | ESC exit", cv::Point(8, bottomY), s * 0.9, kGray);
	if (msg != nullptr)
		PutTextOutlined(img, msg->text, cv::Point(8, bottomY - lineH), s * 1.1, msg->color);
}
