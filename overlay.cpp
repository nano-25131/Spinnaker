#include "overlay.h"
#include "text_render.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <opencv2/imgproc.hpp>

// 极简 + 层次：数值用半粗体白字，标签用灰色小字，分组之间用细竖线隔开；
// 所有面板都是带 1px 亮边的半透明胶囊（毛玻璃感），不遮挡画面中心。
//   左上  状态条：● 24.8 fps | #12 | 11 saved [| 2 writing] [| 1 failed] | 2048×2048
//   底部  按键提示
//   提示  操作结果（居中小条，淡出）

namespace
{
const cv::Scalar kPanelFill(18, 18, 18);
#if defined(_WIN32)
const char *kTimes = "\xC3\x97"; // ×
#else
const char *kTimes = "x";
#endif

int CapH(int px) { return std::max(6, static_cast<int>(std::lround(px * 0.70))); } // 大写字母/数字高度

cv::Rect Local(const cv::Rect &r, cv::Point off)
{
	return cv::Rect(r.x - off.x, r.y - off.y, r.width, r.height);
}

void FillRounded(cv::Mat &img, cv::Rect r, int rad, const cv::Scalar &c)
{
	rad = std::max(0, std::min(rad, std::min(r.width, r.height) / 2));
	cv::rectangle(img, cv::Rect(r.x + rad, r.y, r.width - 2 * rad, r.height), c, cv::FILLED);
	cv::rectangle(img, cv::Rect(r.x, r.y + rad, r.width, r.height - 2 * rad), c, cv::FILLED);
	const int x1 = r.x + r.width - 1 - rad, y1 = r.y + r.height - 1 - rad;
	cv::circle(img, cv::Point(r.x + rad, r.y + rad), rad, c, cv::FILLED, cv::LINE_AA);
	cv::circle(img, cv::Point(x1, r.y + rad), rad, c, cv::FILLED, cv::LINE_AA);
	cv::circle(img, cv::Point(r.x + rad, y1), rad, c, cv::FILLED, cv::LINE_AA);
	cv::circle(img, cv::Point(x1, y1), rad, c, cv::FILLED, cv::LINE_AA);
}

// 在 r 区域里先画到一份拷贝上，再按 alpha 混合回去（只拷贝小区域，开销很小）
template <class F>
void Blend(cv::Mat &img, cv::Rect r, double alpha, F draw)
{
	r &= cv::Rect(0, 0, img.cols, img.rows);
	alpha = std::min(1.0, alpha);
	if (r.empty() || alpha <= 0.0)
		return;
	cv::Mat roi = img(r);
	cv::Mat layer = roi.clone();
	draw(layer, r.tl());
	cv::addWeighted(layer, alpha, roi, 1.0 - alpha, 0.0, roi);
}

void Pill(cv::Mat &img, const cv::Rect &r, double alpha, const cv::Scalar &c)
{
	Blend(img, r, alpha, [&](cv::Mat &layer, cv::Point off)
		  { FillRounded(layer, Local(r, off), r.height / 2, c); });
}

// 半透明深色胶囊 + 1px 亮边
void Chip(cv::Mat &img, const cv::Rect &r, double k)
{
	Pill(img, r, 0.16 * k, kWhite);
	Pill(img, cv::Rect(r.x + 1, r.y + 1, r.width - 2, r.height - 2), 0.60 * k, kPanelFill);
}

// 状态点：实心圆 + 一圈淡淡的光晕
void Dot(cv::Mat &img, cv::Point c, int r, const cv::Scalar &col)
{
	const int gr = r * 2;
	const cv::Rect rc(c.x - gr - 1, c.y - gr - 1, 2 * gr + 3, 2 * gr + 3);
	Blend(img, rc, 0.28, [&](cv::Mat &l, cv::Point off)
		  { cv::circle(l, c - off, gr, col, cv::FILLED, cv::LINE_AA); });
	cv::circle(img, c, r, col, cv::FILLED, cv::LINE_AA);
}

// ---- 一行由多段文字拼成 ----
struct Seg
{
	std::string text;
	cv::Scalar color;
	bool bold;
	bool small; // 标签：字号略小，基线对齐
	int gapAfter;
	bool divAfter; // 在后面的空隙中间画一条细竖线
};
using Row = std::vector<Seg>;

int SegPx(const Seg &s, int px)
{
	return s.small ? std::max(8, static_cast<int>(std::lround(px * 0.88))) : px;
}

int RowWidth(const Row &row, int px)
{
	int w = 0;
	for (size_t i = 0; i < row.size(); ++i)
	{
		w += MeasureText(row[i].text, SegPx(row[i], px), row[i].bold).width;
		if (i + 1 < row.size())
			w += row[i].gapAfter;
	}
	return w;
}

void DrawRow(cv::Mat &img, const Row &row, int x, int baseline, int px)
{
	const int capH = CapH(px);
	for (const auto &s : row)
	{
		x += DrawTextAA(img, s.text, cv::Point(x, baseline), SegPx(s, px), s.bold, s.color);
		if (s.divAfter && s.gapAfter > 0)
		{
			const int lx = x + s.gapAfter / 2;
			cv::line(img, cv::Point(lx, baseline - capH), cv::Point(lx, baseline), cv::Scalar(110, 110, 110), 1, cv::LINE_AA);
		}
		x += s.gapAfter;
	}
}

// 放不下就逐像素缩小字号
int FitPx(const Row &row, int px, int extra, int avail)
{
	while (px > 10 && RowWidth(row, px) + extra > avail)
		--px;
	return px;
}
} // namespace

void DrawOverlay(cv::Mat &img, const HudInfo &info, const StatusMessage *msg, double flash)
{
	if (img.empty())
		return;

	const double u = std::max(1.0, std::min(2.4, img.cols / 520.0));
	auto P = [u](double v)
	{ return std::max(1, static_cast<int>(std::lround(v * u))); };
	const int px = std::max(11, static_cast<int>(std::lround(14 * u)));
	const int margin = P(12);
	const int padX = static_cast<int>(std::lround(px * 0.85));
	const int padY = static_cast<int>(std::lround(px * 0.60));
	const int availW = img.cols - 2 * margin;
	const auto now = std::chrono::steady_clock::now();

	// 0) 保存闪光：画面边缘一圈白光向内渐隐（像快门），不再整幅提亮，方便保存瞬间继续看画面
	if (flash > 0.0)
	{
		const int t = P(7);
		const double a = std::min(0.85, flash * 0.85);
		for (int i = 0; i < t; ++i)
		{
			const double ai = a * (1.0 - static_cast<double>(i) / t);
			const int w = img.cols - 2 * i, h = img.rows - 2 * i;
			if (w <= 2 || h <= 2)
				break;
			const cv::Rect edges[4] = {cv::Rect(i, i, w, 1), cv::Rect(i, img.rows - 1 - i, w, 1),
									   cv::Rect(i, i + 1, 1, h - 2), cv::Rect(img.cols - 1 - i, i + 1, 1, h - 2)};
			for (const auto &e : edges)
				Blend(img, e, ai, [&](cv::Mat &layer, cv::Point)
					  { layer.setTo(kWhite); });
		}
	}

	// 1) 左上角状态条
	{
		const int gS = P(5), gB = P(22);
		const int dotR = std::max(2, static_cast<int>(std::lround(px * 0.22)));
		const int dotGap = P(10);
		const int extra = 2 * padX + 2 * dotR + dotGap;

		std::ostringstream f;
		f << std::fixed << std::setprecision(1) << info.fps;

		Row row;
		auto add = [&](const std::string &t, const cv::Scalar &c, bool bold, bool small, int gap, bool div)
		{ row.push_back({t, c, bold, small, gap, div}); };
		add(f.str(), kWhite, true, false, gS, false);
		add("fps", kGray, false, true, gB, true);
		add("#" + std::to_string(info.nextId), kWhite, true, false, gB, true);
		add(std::to_string(info.saved), kWhite, true, false, gS, false);
		add("saved", kGray, false, true, gB, true);
		if (info.writing > 0)
		{
			add(std::to_string(info.writing), kYellow, true, false, gS, false);
			add("writing", kGray, false, true, gB, true);
		}
		if (info.failed > 0)
		{
			add(std::to_string(info.failed), kRed, true, false, gS, false);
			add("failed", kGray, false, true, gB, true);
		}
		add(std::to_string(info.width) + kTimes + std::to_string(info.height), kGray, false, true, 0, false);

		// 放不下先去掉分辨率，再不行才缩字号
		if (RowWidth(row, px) + extra > availW)
		{
			row.pop_back();
			row.back().gapAfter = 0;
			row.back().divAfter = false;
		}
		const int fpx = FitPx(row, px, extra, availW);

		const int capH = CapH(fpx);
		const int h = capH + 2 * padY;
		const int w = std::min(RowWidth(row, fpx) + extra, availW);
		const cv::Rect pill(margin, margin, w, h);
		Chip(img, pill, 1.0);

		// 状态点：正常绿，写盘中黄，有失败红（静态，不闪烁）
		const cv::Scalar state = info.failed > 0 ? kRed : (info.writing > 0 ? kYellow : kGreen);
		const int baseline = pill.y + padY + capH;
		Dot(img, cv::Point(pill.x + padX + dotR, pill.y + h / 2), dotR, state);
		DrawRow(img, row, pill.x + padX + 2 * dotR + dotGap, baseline, fpx);
	}

	// 2) 底部按键提示：键名半粗体白，说明灰
	int barTop = img.rows - margin;
	{
		const int gk = P(6), gi = P(20);
		Row row;
		auto key = [&](const char *k, const char *label, bool last)
		{
			row.push_back({k, kWhite, true, false, gk, false});
			row.push_back({label, kGray, false, false, last ? 0 : gi, false});
		};
		key("SPACE/LMB", "save", false);
		key("BKSP/RMB", "undo", false);
		key("F", "folder", false);
		key("+/-", "zoom", false);
		key("ESC", "quit", true);

		// 太窄放不下时，先丢掉 zoom、再丢 folder（保存/撤销/退出始终保留）
		while (row.size() > 6 && RowWidth(row, 10) + 2 * padX > availW)
			row.erase(row.end() - 4, row.end() - 2);

		const int hpx = FitPx(row, std::max(10, static_cast<int>(std::lround(px * 0.86))), 2 * padX, availW);
		const int capH = CapH(hpx);
		const int h = capH + 2 * padY;
		const int w = RowWidth(row, hpx) + 2 * padX;
		const cv::Rect pill((img.cols - w) / 2, img.rows - margin - h, w, h);
		barTop = pill.y;

		// 刚启动时完整显示 8 秒，之后用 1 秒淡到 40%：用熟了就不再抢画面
		static const auto kStart = std::chrono::steady_clock::now();
		const double since = std::chrono::duration<double>(now - kStart).count();
		const double dim = since < 8.0 ? 1.0 : std::max(0.4, 1.0 - (since - 8.0) * 0.6);
		Blend(img, pill, dim, [&](cv::Mat &layer, cv::Point off)
			  {
				  const cv::Rect b = Local(pill, off);
				  Chip(layer, b, 0.9);
				  DrawRow(layer, row, b.x + padX, b.y + padY + capH, hpx); });
	}

	// 3) 操作结果提示：居中小条，最后 0.5 秒淡出
	if (msg != nullptr && !msg->text.empty() && now < msg->until)
	{
		const double remain = std::chrono::duration<double>(msg->until - now).count();
		const double fade = std::min(1.0, remain / 0.5);

		const int dotR = std::max(2, static_cast<int>(std::lround(px * 0.22)));
		const int dotGap = P(10);
		const int extra = 2 * padX + 2 * dotR + dotGap;
		int tpx = px;
		while (tpx > 10 && MeasureText(msg->text, tpx, true).width + extra > availW)
			--tpx;

		const int capH = CapH(tpx);
		const int h = capH + 2 * padY;
		const int w = MeasureText(msg->text, tpx, true).width + extra;
		const cv::Rect pill((img.cols - w) / 2, barTop - P(8) - h, w, h);

		Chip(img, pill, fade);
		Blend(img, pill, fade, [&](cv::Mat &layer, cv::Point off)
			  {
				  const cv::Rect b = Local(pill, off);
				  Dot(layer, cv::Point(b.x + padX + dotR, b.y + b.height / 2), dotR, msg->color);
				  DrawTextAA(layer, msg->text, cv::Point(b.x + padX + 2 * dotR + dotGap, b.y + padY + capH), tpx, true, kWhite); });
	}
}