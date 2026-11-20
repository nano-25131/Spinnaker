#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cctype>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW
#pragma comment(lib, "shell32.lib")
#endif

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;
using namespace std;

namespace fs = std::filesystem;

// 本文件含中文字面量，MSVC 必须用 /utf-8 编译，否则 config.ini 模板会变成 GBK 乱码。
// 下面的断言在源文件被当作 UTF-8 时成立（"中" = 3 字节 + 结尾 \0 = 4）。
static_assert(sizeof("中") == 4, "请使用 UTF-8 编译：MSVC 加 /utf-8 （CMake: add_compile_options(/utf-8)）");

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
// 字符串 / 路径工具（全程以 UTF-8 std::string 传递文本，落到文件系统时转成 fs::path）
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

static std::string ToLower(std::string s)
{
	for (auto &c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

#if defined(_WIN32)
static std::string WideToUtf8(const std::wstring &w)
{
	if (w.empty())
		return "";
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
	return s;
}
static std::wstring Utf8ToWide(const std::string &s)
{
	if (s.empty())
		return L"";
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
	return w;
}
#endif

// UTF-8 字符串 -> fs::path（Windows 下走宽字符，中文路径不会丢）
static fs::path Utf8ToPath(const std::string &s)
{
#if defined(_WIN32)
	return fs::path(Utf8ToWide(s));
#else
	return fs::path(s);
#endif
}

// fs::path -> UTF-8 字符串（仅用于打印）
static std::string PathToUtf8(const fs::path &p)
{
#if defined(_WIN32)
	return WideToUtf8(p.wstring());
#else
	return p.string();
#endif
}

// 命令行参数统一转成 UTF-8（Windows 下 argv 是 ANSI/GBK，不能直接用）
static std::vector<std::string> CollectArgs(int argc, char **argv)
{
	std::vector<std::string> args;
#if defined(_WIN32)
	(void)argc;
	(void)argv;
	int wargc = 0;
	LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
	if (wargv != nullptr)
	{
		for (int i = 1; i < wargc; ++i)
			args.push_back(WideToUtf8(wargv[i]));
		LocalFree(wargv);
	}
#else
	for (int i = 1; i < argc; ++i)
		args.emplace_back(argv[i]);
#endif
	return args;
}

// 可执行文件所在目录（config.ini 默认放这里，大家双击/从任意目录启动都读同一份）
static fs::path ExeDir()
{
#if defined(_WIN32)
	wchar_t buf[MAX_PATH * 4];
	const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
	if (n > 0 && n < sizeof(buf) / sizeof(buf[0]))
		return fs::path(buf).parent_path();
#endif
	std::error_code ec;
	return fs::current_path(ec);
}

// ============================================================================
// 配置：默认值 < config.ini < 命令行 --key=value
// ============================================================================
struct Config
{
	std::string prefix;				// 文件名前缀，空 = 只用编号
	bool autoIndex = true;			// start_index = auto
	long long startIndex = 0;		// 手动指定的起始编号
	std::string saveFolder = ".";	// 保存目录
	double previewScale = 0.2;		// 预览缩放
	int pngCompression = 1;			// PNG 压缩级别 0-9（1 最快）
	int queueSize = 32;				// 后台写盘队列最多缓存多少帧
	int roiWidth = 2048;
	int roiHeight = 2048;
	int offsetX = -1;				// -1 = 居中
	int offsetY = -1;
	std::string exposure = "keep";	// keep / auto / 数值(us)
	std::string gain = "keep";		// keep / auto / 数值(dB)
	int bufferCount = 3;			// 相机端缓冲区数量
	bool pauseOnExit = true;		// 退出时是否等待回车
};

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

// 读取 ini：返回 false = 文件不存在/打不开
static bool LoadIni(const fs::path &path, std::map<std::string, std::string> &kv)
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

// 命令行：--key=value 或 --key value；--flag 单独出现视为 true
static std::map<std::string, std::string> ParseCommandLine(const std::vector<std::string> &args)
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

static void ApplySettings(Config &cfg, const std::map<std::string, std::string> &kv, const char *source)
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

// 把明显不合理的值拉回合法范围
static bool ValidateConfig(Config &cfg)
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

static void PrintConfig(const Config &cfg, const fs::path &configPath)
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

// ============================================================================
// 文件命名 / 编号扫描
// ============================================================================

static fs::path MakeImagePath(const fs::path &folder, const fs::path &prefix, long long id)
{
	fs::path name = prefix;
	if (!name.empty())
		name += "_";
	name += std::to_string(id);
	name += ".png";
	return folder / name;
}

// 扫描目录，找出符合 "前缀_数字.png"（或无前缀时 "数字.png"）的最大编号；没有则返回 -1
static long long ScanMaxIndex(const fs::path &folder, const fs::path &prefix)
{
	fs::path::string_type pfx;
	if (!prefix.empty())
	{
		fs::path p = prefix;
		p += "_";
		pfx = p.native();
	}

	long long maxId = -1;
	std::error_code ec;
	fs::directory_iterator it(folder, ec), end;
	for (; !ec && it != end; it.increment(ec))
	{
		std::error_code ec2;
		if (!it->is_regular_file(ec2))
			continue;

		const fs::path &p = it->path();
		const fs::path::string_type ext = p.extension().native();
		if (!(ext.size() == 4 && ext[0] == '.' && (ext[1] | 0x20) == 'p' && (ext[2] | 0x20) == 'n' && (ext[3] | 0x20) == 'g'))
			continue;

		const fs::path::string_type stem = p.stem().native();
		if (stem.size() <= pfx.size())
			continue;
		if (!pfx.empty() && stem.compare(0, pfx.size(), pfx) != 0)
			continue;

		long long id = 0;
		bool ok = (stem.size() - pfx.size()) <= 9; // 最多 9 位，避免溢出
		for (size_t i = pfx.size(); ok && i < stem.size(); ++i)
		{
			if (stem[i] < '0' || stem[i] > '9')
				ok = false;
			else
				id = id * 10 + (stem[i] - '0');
		}
		if (ok && id > maxId)
			maxId = id;
	}
	return maxId;
}

// ============================================================================
// 后台写盘线程：主线程只做 clone + 入队，PNG 编码和写文件都在这里
// ============================================================================
struct SaveJob
{
	fs::path path;
	long long id = 0;
	cv::Mat image;
};

// 后台线程完成一次写盘后的结果，由主线程取走并显示在预览画面上
struct WriteResult
{
	long long id = 0;
	bool ok = true;
	bool isDelete = false; // true = 写盘期间被用户删除，但删除文件失败
	std::string reason;	   // 失败原因（纯 ASCII，会叠加到预览画面）
};

class AsyncImageWriter
{
public:
	enum class CancelResult
	{
		RemovedFromQueue, // 还没开始写，直接从队列撤销
		MarkedInFlight,	  // 正在写，写完后自动删除
		NotPending		  // 不在队列里，调用方自己删文件
	};

	AsyncImageWriter(size_t maxQueue, int pngCompression)
		: maxQueue_(maxQueue), pngCompression_(pngCompression), thread_(&AsyncImageWriter::Run, this) {}

	~AsyncImageWriter() { Shutdown(); }

	AsyncImageWriter(const AsyncImageWriter &) = delete;
	AsyncImageWriter &operator=(const AsyncImageWriter &) = delete;

	// 队列满返回 false
	bool Enqueue(SaveJob &&job)
	{
		{
			std::lock_guard<std::mutex> lk(m_);
			if (q_.size() >= maxQueue_)
				return false;
			q_.push_back(std::move(job));
		}
		cv_.notify_one();
		return true;
	}

	// 该路径是否还在队列里或正在写（此时磁盘上可能还看不到文件）
	bool IsPending(const fs::path &p)
	{
		std::lock_guard<std::mutex> lk(m_);
		if (hasInflight_ && inflight_ == p)
			return true;
		for (const auto &j : q_)
			if (j.path == p)
				return true;
		return false;
	}

	CancelResult Cancel(const fs::path &p)
	{
		std::lock_guard<std::mutex> lk(m_);
		for (auto it = q_.begin(); it != q_.end(); ++it)
		{
			if (it->path == p)
			{
				q_.erase(it);
				return CancelResult::RemovedFromQueue;
			}
		}
		if (hasInflight_ && inflight_ == p)
		{
			inflightCancelled_ = true;
			return CancelResult::MarkedInFlight;
		}
		return CancelResult::NotPending;
	}

	// 取走后台线程已完成的结果（主线程每轮调用一次）
	std::vector<WriteResult> TakeResults()
	{
		std::lock_guard<std::mutex> lk(m_);
		std::vector<WriteResult> out;
		out.swap(results_);
		return out;
	}

	size_t PendingCount()
	{
		std::lock_guard<std::mutex> lk(m_);
		return q_.size() + (hasInflight_ ? 1 : 0);
	}

	// 等队列写完再退出
	void Shutdown()
	{
		if (!thread_.joinable())
			return;
		const size_t n = PendingCount();
		if (n > 0)
			cout << "Flushing " << n << " pending image(s) to disk..." << endl;
		{
			std::lock_guard<std::mutex> lk(m_);
			stop_ = true;
		}
		cv_.notify_all();
		thread_.join();
		if (failed_ > 0)
			cout << "Warning: " << failed_ << " image(s) failed to write." << endl;
	}

private:
	void Run()
	{
		for (;;)
		{
			SaveJob job;
			{
				std::unique_lock<std::mutex> lk(m_);
				cv_.wait(lk, [&]
						 { return stop_ || !q_.empty(); });
				if (q_.empty())
					return; // stop_ 且队列已空
				job = std::move(q_.front());
				q_.pop_front();
				inflight_ = job.path;
				hasInflight_ = true;
				inflightCancelled_ = false;
			}

			std::string err;
			const bool ok = WriteImage(job, err);

			{
				std::lock_guard<std::mutex> lk(m_);
				if (inflightCancelled_)
				{
					// 写盘期间用户按了退格：写完后删除；只有删除失败才需要通知主线程
					if (ok)
					{
						std::error_code ec;
						const bool removed = fs::remove(job.path, ec);
						if (!removed || ec)
						{
							WriteResult r;
							r.id = job.id;
							r.ok = false;
							r.isDelete = true;
							r.reason = "remove failed";
							results_.push_back(r);
						}
					}
				}
				else
				{
					WriteResult r;
					r.id = job.id;
					r.ok = ok;
					r.reason = err;
					results_.push_back(r);
				}
				hasInflight_ = false;
			}
		}
	}

	// imencode 到内存 + ofstream(fs::path) 写文件：绕开 cv::imwrite 的非 ASCII 路径问题
	bool WriteImage(const SaveJob &job, std::string &err)
	{
		try
		{
			std::vector<uchar> buf;
			const std::vector<int> params = {cv::IMWRITE_PNG_COMPRESSION, pngCompression_};
			if (!cv::imencode(".png", job.image, buf, params))
			{
				cout << "Failed to encode: " << PathToUtf8(job.path) << endl;
				err = "encode failed";
				++failed_;
				return false;
			}

			// 先写 .part 再改名，避免程序被强杀时留下半个 PNG
			fs::path tmp = job.path;
			tmp += ".part";
			{
				std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
				if (!ofs)
				{
					cout << "Failed to open for writing: " << PathToUtf8(tmp) << endl;
					err = "cannot open file";
					++failed_;
					return false;
				}
				ofs.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
				ofs.close();
				if (!ofs)
				{
					cout << "Failed to write: " << PathToUtf8(tmp) << endl;
					err = "write failed (disk full?)";
					std::error_code ec;
					fs::remove(tmp, ec);
					++failed_;
					return false;
				}
			}

			std::error_code ec;
			if (fs::exists(job.path, ec)) // 最后一道保险：绝不覆盖
			{
				cout << "Refuse to overwrite existing file: " << PathToUtf8(job.path) << endl;
				err = "file exists";
				fs::remove(tmp, ec);
				++failed_;
				return false;
			}
			fs::rename(tmp, job.path, ec);
			if (ec)
			{
				cout << "Failed to rename " << PathToUtf8(tmp) << ": " << ec.message() << endl;
				err = "rename failed";
				std::error_code ec2;
				fs::remove(tmp, ec2);
				++failed_;
				return false;
			}
			return true;
		}
		catch (const std::exception &e)
		{
			cout << "Writer error: " << e.what() << endl;
			err = "exception";
			++failed_;
			return false;
		}
	}

	const size_t maxQueue_;
	const int pngCompression_;

	std::mutex m_;
	std::condition_variable cv_;
	std::deque<SaveJob> q_;
	std::vector<WriteResult> results_;
	bool stop_ = false;
	fs::path inflight_;
	bool hasInflight_ = false;
	bool inflightCancelled_ = false;
	std::atomic<int> failed_{0};

	std::thread thread_; // 放最后：其余成员初始化完才启动线程
};

// ============================================================================
// 相机设置
// ============================================================================

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

// 设置整型节点：夹到 [min,max] 并按步进对齐；返回是否写入成功
static bool SetIntClamped(INodeMap &nm, const char *name, int64_t v, int64_t *applied = nullptr)
{
	CIntegerPtr p = nm.GetNode(name);
	if (!IsReadable(p) || !IsWritable(p))
		return false;
	const int64_t mn = p->GetMin();
	const int64_t mx = p->GetMax();
	int64_t inc = p->GetInc();
	if (inc < 1)
		inc = 1;
	if (v > mx)
		v = mx;
	if (v < mn)
		v = mn;
	v = mn + ((v - mn) / inc) * inc;
	p->SetValue(v);
	if (applied)
		*applied = p->GetValue();
	return true;
}

static bool SetEnumByName(INodeMap &nm, const char *node, const char *entry)
{
	CEnumerationPtr p = nm.GetNode(node);
	if (!IsReadable(p) || !IsWritable(p))
		return false;
	CEnumEntryPtr e = p->GetEntryByName(entry);
	if (!IsReadable(e))
		return false;
	p->SetIntValue(e->GetValue());
	return true;
}

// ROI：宽高来自配置，偏移 center 时按传感器尺寸居中
static bool ConfigureRoi(INodeMap &nm, const Config &cfg)
{
	CIntegerPtr ptrWidth = nm.GetNode("Width");
	CIntegerPtr ptrHeight = nm.GetNode("Height");
	if (!IsWritable(ptrWidth) || !IsWritable(ptrHeight))
	{
		cout << "Error: Width or Height not writable, cannot set ROI." << endl;
		return false;
	}

	// 先把偏移归零，避免 偏移+宽度 超出传感器范围导致设置宽高失败
	SetIntClamped(nm, "OffsetX", 0);
	SetIntClamped(nm, "OffsetY", 0);

	const int64_t sensorW = ptrWidth->GetMax(); // 偏移为 0 时的最大值 = 传感器尺寸
	const int64_t sensorH = ptrHeight->GetMax();

	int64_t aw = 0, ah = 0;
	SetIntClamped(nm, "Width", cfg.roiWidth, &aw);
	SetIntClamped(nm, "Height", cfg.roiHeight, &ah);
	if (aw != cfg.roiWidth || ah != cfg.roiHeight)
		cout << "Warning: requested ROI " << cfg.roiWidth << "x" << cfg.roiHeight
			 << " adjusted to " << aw << "x" << ah << " (sensor " << sensorW << "x" << sensorH << ")" << endl;

	const int64_t ox = cfg.offsetX < 0 ? (sensorW - aw) / 2 : cfg.offsetX;
	const int64_t oy = cfg.offsetY < 0 ? (sensorH - ah) / 2 : cfg.offsetY;
	int64_t aox = 0, aoy = 0;
	if (SetIntClamped(nm, "OffsetX", ox, &aox) && SetIntClamped(nm, "OffsetY", oy, &aoy))
		cout << "ROI set to " << aw << "x" << ah << ", offset X: " << aox << ", Y: " << aoy << "..." << endl;
	else
		cout << "Warning: OffsetX or OffsetY not writable, ROI may not be centered." << endl;
	return true;
}

// keep = 不动；auto = 打开自动；数值 = 关闭自动并写入
static void ApplyAutoOrValue(INodeMap &nm, const char *autoNode, const char *valueNode,
							 const std::string &setting, const char *label)
{
	const std::string s = ToLower(setting);
	if (s.empty() || s == "keep")
		return;

	if (s == "auto")
	{
		if (SetEnumByName(nm, autoNode, "Continuous"))
			cout << label << ": auto (Continuous)" << endl;
		else
			cout << "Warning: " << autoNode << " not available, " << label << " unchanged." << endl;
		return;
	}

	double v = 0;
	try
	{
		v = std::stod(s);
	}
	catch (...)
	{
		cout << "Warning: invalid " << label << " value \"" << setting << "\", unchanged." << endl;
		return;
	}

	SetEnumByName(nm, autoNode, "Off"); // 先关自动，否则手动值会被覆盖/拒绝
	CFloatPtr p = nm.GetNode(valueNode);
	if (!IsReadable(p) || !IsWritable(p))
	{
		cout << "Warning: " << valueNode << " not writable, " << label << " unchanged." << endl;
		return;
	}
	const double mn = p->GetMin(), mx = p->GetMax();
	if (v < mn || v > mx)
	{
		cout << "Warning: " << label << " " << v << " out of range [" << mn << ", " << mx << "], clamped." << endl;
		v = std::max(mn, std::min(mx, v));
	}
	p->SetValue(v);
	cout << label << ": " << p->GetValue() << endl;
}

// 降低延迟：只保留最新帧 + 减少缓冲数量；并打印实际帧率和曝光时间便于排查
// 必须在 BeginAcquisition() 之前调用
static void ConfigureLowLatency(CameraPtr pCam, INodeMap &nodeMap, int bufferCount)
{
	try
	{
		INodeMap &sNodeMap = pCam->GetTLStreamNodeMap();

		// 1) 缓冲区处理模式：NewestOnly = 只给最新一帧，旧帧直接丢弃
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
					int64_t n = bufferCount;
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

// ============================================================================
// 预览叠加：状态信息 + 保存/删除结果提示
// OpenCV 自带字体只支持 ASCII，所以叠加到画面上的文字一律用英文（不放路径）。
// ============================================================================
struct StatusMessage
{
	std::string text;
	cv::Scalar color = cv::Scalar(255, 255, 255);
	std::chrono::steady_clock::time_point until{};
};

static const cv::Scalar kGreen(0, 200, 0);
static const cv::Scalar kRed(0, 0, 255);
static const cv::Scalar kYellow(0, 220, 255);
static const cv::Scalar kWhite(255, 255, 255);
static const cv::Scalar kGray(190, 190, 190);

// 带黑色描边的文字，任何背景下都看得清；文字太宽时自动缩小字号
static void PutTextOutlined(cv::Mat &img, const std::string &text, cv::Point org, double scale, const cv::Scalar &color)
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

// 左上角状态行；底部：操作提示 + 最近一次保存/删除结果
static void DrawOverlay(cv::Mat &img, const std::vector<std::string> &topLines, const StatusMessage *msg)
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

static void PrintUsage()
{
	cout << "Usage: Acquisition.exe [--config=path] [--key=value ...]\n"
			"  --config=PATH        配置文件（默认：exe 同目录下的 config.ini，不存在会自动生成）\n"
			"  --prefix=NAME        文件名前缀\n"
			"  --start_index=N|auto 起始编号\n"
			"  --save_folder=DIR    保存目录\n"
			"  --exposure_us=N|auto|keep\n"
			"  --gain_db=N|auto|keep\n"
			"  --width=N --height=N --offset_x=N|center --offset_y=N|center\n"
			"  --png_compression=0-9 --preview_scale=0.2 --writer_queue_size=N --buffer_count=N\n"
			"  --pause_on_exit=true|false\n"
		 << endl;
}

int main(int argc, char **argv)
{
#if defined(_WIN32)
	// 控制台按 UTF-8 输出，打印中文路径不乱码（字体需支持中文）
	SetConsoleOutputCP(CP_UTF8);
	// 注册控制台事件：点 X 关闭窗口也能正常释放相机
	SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif

	const std::vector<std::string> args = CollectArgs(argc, argv);
	for (const auto &a : args)
	{
		if (a == "--help" || a == "-h" || a == "/?")
		{
			PrintUsage();
			return 0;
		}
	}

	//=========================读取配置============================================
	Config cfg;
	const auto cmdKv = ParseCommandLine(args);

	fs::path configPath = ExeDir() / "config.ini";
	{
		const auto it = cmdKv.find("config");
		if (it != cmdKv.end() && !it->second.empty())
			configPath = Utf8ToPath(it->second);
	}

	{
		std::map<std::string, std::string> fileKv;
		if (LoadIni(configPath, fileKv))
		{
			ApplySettings(cfg, fileKv, "config file");
		}
		else
		{
			cout << "Config file not found, creating default: " << PathToUtf8(configPath) << endl;
			std::ofstream ofs(configPath, std::ios::binary);
			if (ofs)
				ofs << kDefaultConfigText;
			else
				cout << "Warning: cannot write default config (check permissions), using built-in defaults." << endl;
		}
	}
	ApplySettings(cfg, cmdKv, "command line");

	if (!ValidateConfig(cfg))
	{
		cout << "Press Enter to exit..." << endl;
		getchar();
		return -1;
	}
	PrintConfig(cfg, configPath);
	//============================================================================

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

		result = RunSingleCamera(pCam, cfg);

		cout << "Camera 0 example complete." << endl;

		pCam = nullptr;
	}

	// Clear camera list before releasing system
	camList.Clear();

	// Release system
	system->ReleaseInstance();

	// 如果是被关闭信号触发的退出，不再等待回车
	if (!g_stopRequested && cfg.pauseOnExit)
	{
		cout << endl
			 << "Done! Press Enter to exit..." << endl;
		getchar();
	}

	return result;
}