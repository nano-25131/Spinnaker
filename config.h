#pragma once
// 配置：默认值 < config.ini < 命令行 --key=value
#include <map>
#include <string>
#include <vector>
#include "text_utils.h"

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

// 读取 ini：返回 false = 文件不存在/打不开
bool LoadIni(const fs::path &path, std::map<std::string, std::string> &kv);

// 把默认配置模板写到 path；失败返回 false
bool WriteDefaultConfig(const fs::path &path);

// 命令行：--key=value 或 --key value；--flag 单独出现视为 true
std::map<std::string, std::string> ParseCommandLine(const std::vector<std::string> &args);

// 把 kv 应用到 cfg（后应用的覆盖先应用的）
void ApplySettings(Config &cfg, const std::map<std::string, std::string> &kv, const char *source);

// 把明显不合理的值拉回合法范围；返回 false = 致命错误
bool ValidateConfig(Config &cfg);

void PrintConfig(const Config &cfg, const fs::path &configPath);
