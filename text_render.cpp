#include "text_render.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <opencv2/imgproc.hpp>

#if defined(_WIN32)
#include <map>
#include "platform.h" // 放在 OpenCV 头文件之后

namespace
{
struct GdiCtx
{
	HDC dc = nullptr;
	HBITMAP bmp = nullptr;
	HGDIOBJ oldBmp = nullptr;
	void *bits = nullptr;
	int bw = 0, bh = 0;
	std::map<int, HFONT> fonts; // key = px * 2 + bold

	~GdiCtx()
	{
		if (dc && oldBmp)
			SelectObject(dc, oldBmp);
		if (bmp)
			DeleteObject(bmp);
		for (auto &kv : fonts)
			DeleteObject(kv.second);
		if (dc)
			DeleteDC(dc);
	}
};

GdiCtx &Ctx()
{
	static GdiCtx c;
	if (!c.dc)
		c.dc = CreateCompatibleDC(nullptr);
	return c;
}

std::wstring ToWide(const std::string &s)
{
	if (s.empty())
		return L"";
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
	return w;
}

HFONT GetFont(GdiCtx &c, int px, bool bold)
{
	const int key = px * 2 + (bold ? 1 : 0);
	auto it = c.fonts.find(key);
	if (it != c.fonts.end())
		return it->second;

	LOGFONTW lf = {};
	lf.lfHeight = -px;
	lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfOutPrecision = OUT_TT_PRECIS;
	lf.lfQuality = ANTIALIASED_QUALITY; // 灰度抗锯齿（ClearType 会产生彩边）
	wcscpy_s(lf.lfFaceName, L"Segoe UI");
	HFONT f = CreateFontIndirectW(&lf);
	c.fonts[key] = f;
	return f;
}

void EnsureBitmap(GdiCtx &c, int w, int h)
{
	if (c.bmp && w <= c.bw && h <= c.bh)
		return;
	const int nw = std::max(w, c.bw), nh = std::max(h, c.bh);
	if (c.bmp)
	{
		SelectObject(c.dc, c.oldBmp);
		DeleteObject(c.bmp);
		c.bmp = nullptr;
	}
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = nw;
	bi.bmiHeader.biHeight = -nh; // 负数 = 自上而下
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	c.bmp = CreateDIBSection(c.dc, &bi, DIB_RGB_COLORS, &c.bits, nullptr, 0);
	c.oldBmp = SelectObject(c.dc, c.bmp);
	c.bw = nw;
	c.bh = nh;
}
} // namespace

TextMetrics MeasureText(const std::string &utf8, int px, bool bold)
{
	TextMetrics m;
	if (utf8.empty())
		return m;
	GdiCtx &c = Ctx();
	SelectObject(c.dc, GetFont(c, px, bold));
	const std::wstring w = ToWide(utf8);
	SIZE sz = {};
	GetTextExtentPoint32W(c.dc, w.c_str(), static_cast<int>(w.size()), &sz);
	TEXTMETRICW tm = {};
	GetTextMetricsW(c.dc, &tm);
	m.width = sz.cx;
	m.ascent = tm.tmAscent;
	m.descent = tm.tmDescent;
	return m;
}

int DrawTextAA(cv::Mat &img, const std::string &utf8, cv::Point baseline, int px, bool bold,
			   const cv::Scalar &color, double alpha)
{
	if (utf8.empty() || img.empty() || img.type() != CV_8UC3)
		return 0;
	GdiCtx &c = Ctx();
	SelectObject(c.dc, GetFont(c, px, bold));
	const std::wstring w = ToWide(utf8);

	SIZE sz = {};
	GetTextExtentPoint32W(c.dc, w.c_str(), static_cast<int>(w.size()), &sz);
	TEXTMETRICW tm = {};
	GetTextMetricsW(c.dc, &tm);
	const int tw = sz.cx + 2, th = tm.tmHeight;

	EnsureBitmap(c, tw, th);
	if (!c.bits)
		return sz.cx;
	const int stride = c.bw * 4;
	std::memset(c.bits, 0, static_cast<size_t>(stride) * th);

	SetBkMode(c.dc, TRANSPARENT);
	SetTextColor(c.dc, RGB(255, 255, 255));
	TextOutW(c.dc, 1, 0, w.c_str(), static_cast<int>(w.size()));
	GdiFlush();

	// 白字黑底 => 通道值就是覆盖率，按它把 color 混合进 img
	const int x0 = baseline.x - 1, y0 = baseline.y - tm.tmAscent;
	const int cb = cv::saturate_cast<uchar>(color[0]);
	const int cg = cv::saturate_cast<uchar>(color[1]);
	const int cr = cv::saturate_cast<uchar>(color[2]);
	const int aMul = static_cast<int>(std::lround(std::max(0.0, std::min(1.0, alpha)) * 256.0));
	const uint8_t *src = static_cast<const uint8_t *>(c.bits);

	for (int yy = 0; yy < th; ++yy)
	{
		const int iy = y0 + yy;
		if (iy < 0 || iy >= img.rows)
			continue;
		uchar *d = img.ptr<uchar>(iy);
		const uint8_t *s = src + static_cast<size_t>(yy) * stride;
		for (int xx = 0; xx < tw; ++xx)
		{
			const int ix = x0 + xx;
			if (ix < 0 || ix >= img.cols)
				continue;
			const int cov = s[xx * 4 + 1];
			if (cov == 0)
				continue;
			const int a = cov * aMul / 255; // 0..256
			uchar *p = d + ix * 3;
			p[0] = static_cast<uchar>(p[0] + (cb - p[0]) * a / 256);
			p[1] = static_cast<uchar>(p[1] + (cg - p[1]) * a / 256);
			p[2] = static_cast<uchar>(p[2] + (cr - p[2]) * a / 256);
		}
	}
	return sz.cx;
}

#else // 非 Windows：退回 Hershey

namespace
{
double HScale(int px) { return px / 30.0; }
int HThick(bool bold) { return bold ? 2 : 1; }
} // namespace

TextMetrics MeasureText(const std::string &t, int px, bool bold)
{
	int base = 0;
	const cv::Size s = cv::getTextSize(t, cv::FONT_HERSHEY_SIMPLEX, HScale(px), HThick(bold), &base);
	TextMetrics m;
	m.width = s.width;
	m.ascent = s.height;
	m.descent = base;
	return m;
}

int DrawTextAA(cv::Mat &img, const std::string &t, cv::Point baseline, int px, bool bold,
			   const cv::Scalar &color, double)
{
	cv::putText(img, t, baseline, cv::FONT_HERSHEY_SIMPLEX, HScale(px), color, HThick(bold), cv::LINE_AA);
	return MeasureText(t, px, bold).width;
}
#endif