#pragma once
// 相机设置：流模式、ROI、曝光/增益、低延迟、设备信息
#include <cstdint>
#include <string>
#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"
#include "config.h"

int SetStreamMode(Spinnaker::CameraPtr pCam);

// 设置整型节点：夹到 [min,max] 并按步进对齐；返回是否写入成功
bool SetIntClamped(Spinnaker::GenApi::INodeMap &nm, const char *name, int64_t v, int64_t *applied = nullptr);

bool SetEnumByName(Spinnaker::GenApi::INodeMap &nm, const char *node, const char *entry);

// ROI：宽高来自配置，偏移 center 时按传感器尺寸居中
bool ConfigureRoi(Spinnaker::GenApi::INodeMap &nm, const Config &cfg);

// keep = 不动；auto = 打开自动；数值 = 关闭自动并写入
void ApplyAutoOrValue(Spinnaker::GenApi::INodeMap &nm, const char *autoNode, const char *valueNode,
					  const std::string &setting, const char *label);

// 降低延迟：只保留最新帧 + 减少缓冲数量；必须在 BeginAcquisition() 之前调用
void ConfigureLowLatency(Spinnaker::CameraPtr pCam, Spinnaker::GenApi::INodeMap &nodeMap, int bufferCount);

int PrintDeviceInfo(Spinnaker::GenApi::INodeMap &nodeMap);
