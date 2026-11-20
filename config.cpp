#include "config.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace std;

// 本文件含中文字面量，MSVC 必须用 /utf-8 编译，否则 config.ini 模板会变成 GBK 乱码。
// 下面的断言在源文件被当作 UTF-8 时成立（"中" = 3 字节 + 结尾 \0 = 4）。
static_assert(sizeof("中") == 4, "请使用 UTF-8 编译：MSVC 加 /utf-8 （CMake: add_compile_options(/utf-8)）");

static const char *kDefaultConfigText = R"(; Spinnaker 采集程序配置（UTF-8 编码，路径可含中文）
; 行首 ; 或 # 为注释。注意：不支持行尾注释（路径里可能有 ; 或 #）。
; 命令行参数优先级更高，例如：  Acquisition.exe --prefix=sampleA --exposure_us=8000
; 想让整组共用一份配置：  Acquisition.exe --config=\\server\share\config.ini

; ---- 保存 ----
; 文件名前缀，留空则文件名为 0.png 1.png ...；非空则为 前缀_0.png 前缀_1.png ...
prefix =
; 起始编号：auto = 扫描保存目录，从 "已有最大编号 + 1" 开始（按前缀分别计数）；也可填具体数字
start_index = auto
; 保存目录（不存在会自动创建）
save_folder = .
; PNG 压缩级别 0-9。1 = 最快（文件大一些），9 = 最小（编码很慢）
png_compression = 1
; 后台写盘队列最多缓存的帧数（每帧约 4 MB，满了会提示稍等再按空格）
writer_queue_size = 32

; ---- 预览 ----
; 预览缩放比例，0.2 = 缩小到 20%
preview_scale = 0.2

; ---- 相机 ----
; ROI 宽高（会按相机的步进自动对齐）
width = 2048
height = 2048
; ROI 偏移：center = 在传感器上居中，或填具体像素值
offset_x = center
offset_y = center
; 曝光时间(微秒)：keep = 不改相机当前值，auto = 自动曝光，或填数值
exposure_us = keep
; 增益(dB)：keep / auto / 数值
gain_db = keep
; 相机端缓冲区数量（越小延迟越低，但容易丢帧）
buffer_count = 3

; ---- 其他 ----
; 退出时是否等待按回车（true / false）
pause_on_exit = true
)";

static bool ParseBool(const std::string &v)
{
	const std::string s = ToLower(v);
	if (s == "1" || s == "true" || s == "yes" || s == "on")
		return true;
	if (s == "0" || s == "false" || s == "no" || s == "off")
		return false;
	throw std::invalid_argument("bool");
}

bool WriteDefaultConfig(const fs::path &path)
{
	std::ofstream ofs(path, std::ios::binary);
	if (!ofs)
		return false;
	ofs << kDefaultConfigText;
	return static_cast<bool>(ofs);
}

bool LoadIni(const fs::path &path, std::map<std::string, std::string> &kv)
{
	std::ifstream ifs(path);
	if (!ifs)
		return false;

	std::string line;
	bool first = true;
	while (std::getline(ifs, line))
	{
		if (first)
		{
			first = false;
			// 去掉 UTF-8 BOM（记事本另存为 "UTF-8 带 BOM" 时会有）
			if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
				static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
				line.erase(0, 3);
		}
		const std::string t = TrimPath(line);
		if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[')
			continue;
		const size_t eq = t.find('=');
		if (eq == std::string::npos)
			continue;
		kv[ToLower(TrimPath(t.substr(0, eq)))] = TrimPath(t.substr(eq + 1));
	}
	return true;
}

std::map<std::string, std::string> ParseCommandLine(const std::vector<std::string> &args)
{
	std::map<std::string, std::string> kv;
	for (size_t i = 0; i < args.size(); ++i)
	{
		const std::string &a = args[i];
		if (a.size() < 3 || a[0] != '-' || a[1] != '-')
		{
			cout << "Ignore argument: " << a << endl;
			continue;
		}
		std::string body = a.substr(2);
		const size_t eq = body.find('=');
		if (eq != std::string::npos)
		{
			kv[ToLower(body.substr(0, eq))] = TrimPath(body.substr(eq + 1));
		}
		else if (i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0)
		{
			kv[ToLower(body)] = TrimPath(args[++i]);
		}
		else
		{
			kv[ToLower(body)] = "true";
		}
	}
	return kv;
}

void ApplySettings(Config &cfg, const std::map<std::string, std::string> &kv, const char *source)
{
	for (const auto &item : kv)
	{
		const std::string &k = item.first;
		const std::string &v = item.second;
		try
		{
			if (k == "config" || k == "help")
				continue;
			else if (k == "prefix")
				cfg.prefix = v;
			else if (k == "start_index")
			{
				if (v.empty() || ToLower(v) == "auto")
					cfg.autoIndex = true;
				else
				{
					const long long n = std::stoll(v);
					if (n < 0)
						throw std::invalid_argument("negative");
					cfg.autoIndex = false;
					cfg.startIndex = n;
				}
			}
			else if (k == "save_folder")
				cfg.saveFolder = v.empty() ? "." : v;
			else if (k == "preview_scale")
				cfg.previewScale = std::stod(v);
			else if (k == "png_compression")
				cfg.pngCompression = std::stoi(v);
			else if (k == "writer_queue_size")
				cfg.queueSize = std::stoi(v);
			else if (k == "width")
				cfg.roiWidth = std::stoi(v);
			else if (k == "height")
				cfg.roiHeight = std::stoi(v);
			else if (k == "offset_x")
				cfg.offsetX = (v.empty() || ToLower(v) == "center") ? -1 : std::stoi(v);
			else if (k == "offset_y")
				cfg.offsetY = (v.empty() || ToLower(v) == "center") ? -1 : std::stoi(v);
			else if (k == "exposure_us")
				cfg.exposure = v.empty() ? "keep" : v;
			else if (k == "gain_db")
				cfg.gain = v.empty() ? "keep" : v;
			else if (k == "buffer_count")
				cfg.bufferCount = std::stoi(v);
			else if (k == "pause_on_exit")
				cfg.pauseOnExit = ParseBool(v);
			else
				cout << "Unknown setting ignored (" << source << "): " << k << endl;
		}
		catch (...)
		{
			cout << "Invalid value for '" << k << "' (" << source << "): \"" << v << "\", ignored" << endl;
		}
	}
}

bool ValidateConfig(Config &cfg)
{
	if (cfg.prefix.find_first_of("\\/:*?\"<>|") != std::string::npos)
	{
		cout << "Error: prefix contains characters not allowed in file names: \\ / : * ? \" < > |" << endl;
		return false;
	}
	if (!(cfg.previewScale > 0.0 && cfg.previewScale <= 1.0))
	{
		cout << "preview_scale out of range (0,1], use 0.2" << endl;
		cfg.previewScale = 0.2;
	}
	cfg.pngCompression = std::max(0, std::min(9, cfg.pngCompression));
	cfg.queueSize = std::max(1, cfg.queueSize);
	cfg.bufferCount = std::max(1, cfg.bufferCount);
	if (cfg.roiWidth <= 0 || cfg.roiHeight <= 0)
	{
		cout << "Error: width/height must be positive" << endl;
		return false;
	}
	return true;
}

void PrintConfig(const Config &cfg, const fs::path &configPath)
{
	cout << endl
		 << "*** CONFIG (" << PathToUtf8(configPath) << ") ***" << endl
		 << "  prefix          : " << (cfg.prefix.empty() ? "(empty)" : cfg.prefix) << endl
		 << "  start_index     : " << (cfg.autoIndex ? std::string("auto") : std::to_string(cfg.startIndex)) << endl
		 << "  save_folder     : " << cfg.saveFolder << endl
		 << "  png_compression : " << cfg.pngCompression << endl
		 << "  preview_scale   : " << cfg.previewScale << endl
		 << "  ROI             : " << cfg.roiWidth << "x" << cfg.roiHeight
		 << "  offset " << (cfg.offsetX < 0 ? std::string("center") : std::to_string(cfg.offsetX)) << ","
		 << (cfg.offsetY < 0 ? std::string("center") : std::to_string(cfg.offsetY)) << endl
		 << "  exposure_us     : " << cfg.exposure << endl
		 << "  gain_db         : " << cfg.gain << endl
		 << "  buffer_count    : " << cfg.bufferCount << endl
		 << endl;
}
