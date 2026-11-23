#include "folder_dialog.h"
#include "platform.h"

#if defined(_WIN32)
#include <objbase.h>
#include <shobjidl.h>

#pragma comment(lib, "ole32.lib")

fs::path PickFolder(const fs::path &initial)
{
	fs::path result;

	// 对话框是 COM 组件。若线程已用别的模式初始化过 COM（RPC_E_CHANGED_MODE），直接沿用即可
	const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	const bool needUninit = SUCCEEDED(hrInit);
	if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE)
		return result;

	IFileOpenDialog *pDlg = nullptr;
	HRESULT hr = CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
	if (SUCCEEDED(hr))
	{
		DWORD opts = 0;
		pDlg->GetOptions(&opts);
		pDlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
		pDlg->SetTitle(L"选择保存文件夹");

		// 起始目录：当前保存目录（存在才设置）
		std::error_code ec;
		const fs::path absInitial = fs::absolute(initial, ec);
		if (!ec && fs::is_directory(absInitial, ec))
		{
			IShellItem *pStart = nullptr;
			if (SUCCEEDED(SHCreateItemFromParsingName(absInitial.c_str(), nullptr, IID_PPV_ARGS(&pStart))))
			{
				pDlg->SetFolder(pStart);
				pStart->Release();
			}
		}

		// 以当前前台窗口（按键所在的预览窗口）为所有者，对话框会盖在它上面，关闭后焦点自动回到预览窗口
		hr = pDlg->Show(GetForegroundWindow());
		if (SUCCEEDED(hr)) // 用户取消会返回 HRESULT_FROM_WIN32(ERROR_CANCELLED)
		{
			IShellItem *pItem = nullptr;
			if (SUCCEEDED(pDlg->GetResult(&pItem)))
			{
				PWSTR psz = nullptr;
				if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz != nullptr)
				{
					result = fs::path(psz);
					CoTaskMemFree(psz);
				}
				pItem->Release();
			}
		}
		pDlg->Release();
	}

	if (needUninit)
		CoUninitialize();
	return result;
}

#else

fs::path PickFolder(const fs::path &)
{
	return fs::path();
}

#endif