#pragma once
// 采集主循环：实时预览，空格保存，退格删除本次运行保存的上一张，ESC 退出
#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"
#include "config.h"

int AcquireImages(Spinnaker::CameraPtr pCam, Spinnaker::GenApi::INodeMap &nodeMap, const Config &cfg);

// 打印设备信息 -> Init -> 设 ROI/曝光/增益 -> 流模式 -> 采集；任何路径都会 DeInit
int RunSingleCamera(Spinnaker::CameraPtr pCam, const Config &cfg);
