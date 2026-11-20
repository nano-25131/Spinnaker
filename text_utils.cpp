#include "text_utils.h"
#include "platform.h"
#include <cctype>

std::string TrimPath(std::string s)
{
	const char *ws = " \t\r\n\"";
	const size_t b = s.find_first_not_of(ws);
	if (b == std::string::npos)
		return "";
	const size_t e = s.find_last_not_of(ws);
	return s.substr(b, e - b + 1);
}

std::string ToLower(std::string s)
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

fs::path Utf8ToPath(const std::string &s)
{
#if defined(_WIN32)
	return fs::path(Utf8ToWide(s));
#else
	return fs::path(s);
#endif
}

std::string PathToUtf8(const fs::path &p)
{
#if defined(_WIN32)
	return WideToUtf8(p.wstring());
#else
	return p.string();
#endif
}

std::vector<std::string> CollectArgs(int argc, char **argv)
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

fs::path ExeDir()
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
