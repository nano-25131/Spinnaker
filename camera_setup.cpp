#include "camera_setup.h"
#include <algorithm>
#include <iostream>

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;
using namespace std;

#include "text_utils.h"

// Use the following enum to select the stream mode
enum StreamMode
{
	STREAM_MODE_TELEDYNE_GIGE_VISION, // Teledyne Gige Vision is the default stream mode for spinview which is supported on Windows
	STREAM_MODE_PGRLWF,				  // Light Weight Filter driver is our legacy driver which is supported on Windows
	STREAM_MODE_SOCKET,				  // Socket is supported for MacOS and Linux, and uses native OS network sockets instead of a
									  // filter driver
};

#if defined(WIN32) || defined(WIN64) || defined(_WIN32)
static const StreamMode chosenStreamMode = STREAM_MODE_TELEDYNE_GIGE_VISION;
#else
static const StreamMode chosenStreamMode = STREAM_MODE_SOCKET;
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

bool SetIntClamped(INodeMap &nm, const char *name, int64_t v, int64_t *applied)
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

bool SetEnumByName(INodeMap &nm, const char *node, const char *entry)
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

bool ConfigureRoi(INodeMap &nm, const Config &cfg)
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

void ApplyAutoOrValue(INodeMap &nm, const char *autoNode, const char *valueNode,
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
		v = (std::max)(mn, (std::min)(mx, v));
	}
	p->SetValue(v);
	cout << label << ": " << p->GetValue() << endl;
}

void ConfigureLowLatency(CameraPtr pCam, INodeMap &nodeMap, int bufferCount)
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
