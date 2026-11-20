#pragma once
// RAII 守卫：保证任何退出路径都会释放相机资源
#include "Spinnaker.h"
#include "console_ctrl.h"

// 保证 EndAcquisition 一定被调用
struct AcquisitionGuard
{
	Spinnaker::CameraPtr cam;
	explicit AcquisitionGuard(Spinnaker::CameraPtr c) : cam(c) {}
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
	Spinnaker::CameraPtr cam;
	explicit CameraInitGuard(Spinnaker::CameraPtr c) : cam(c) {}
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
	Spinnaker::ImagePtr &img;
	explicit ImageReleaser(Spinnaker::ImagePtr &i) : img(i) {}
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
