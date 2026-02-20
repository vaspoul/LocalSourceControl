#include "main.h"
#include "app.h"
#include "settings.h"
#include "resource.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

static HWND						g_hWnd = nullptr;
static ID3D11Device*			g_pd3dDevice = nullptr;
static ID3D11DeviceContext*		g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*			g_pSwapChain = nullptr;
static ID3D11RenderTargetView*	g_mainRenderTargetView = nullptr;
static const UINT				kTrayIconId = 1;
static const UINT				kTrayCallbackMessage = WM_APP + 1;
static const UINT				kTrayMenuRestoreId = 1001;
static const UINT				kTrayMenuExitId = 1002;
static bool						g_isInTray = false;
static NOTIFYICONDATAW			g_trayIconData = {};
static HICON					g_trayIconHandle = nullptr;
static HANDLE					g_singleInstanceMutex = nullptr;


LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static void BringExistingInstanceToFront()
{
	const wchar_t* kWindowClassName = L"ContinuousBackupDX11Wnd";
	HWND existingWindow = FindWindowW(kWindowClassName, nullptr);
	if (!existingWindow)
	{
		return;
	}

	if (IsIconic(existingWindow))
	{
		ShowWindow(existingWindow, SW_RESTORE);
	}
	else
	{
		ShowWindow(existingWindow, SW_SHOW);
	}

	SetForegroundWindow(existingWindow);
}

static void CreateRenderTarget()
{
	ID3D11Texture2D* pBackBuffer = nullptr;
	g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	if (pBackBuffer)
	{
		g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
		pBackBuffer->Release();
	}
}

static void CleanupRenderTarget()
{
	if (g_mainRenderTargetView)
	{
		g_mainRenderTargetView->Release();
		g_mainRenderTargetView = nullptr;
	}
}

static bool CreateDeviceD3D(HWND hWnd)
{
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Width = 0;
	sd.BufferDesc.Height = 0;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hWnd;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	UINT createDeviceFlags = 0;
	// createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;

	D3D_FEATURE_LEVEL featureLevel;
	const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	HRESULT res = D3D11CreateDeviceAndSwapChain(
		nullptr,
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		createDeviceFlags,
		featureLevelArray,
		2,
		D3D11_SDK_VERSION,
		&sd,
		&g_pSwapChain,
		&g_pd3dDevice,
		&featureLevel,
		&g_pd3dDeviceContext
	);

	if (res != S_OK)
	{
		return false;
	}

	CreateRenderTarget();

	return true;
}

static void CleanupDeviceD3D()
{
	CleanupRenderTarget();

	if (g_pSwapChain)
	{
		g_pSwapChain->Release();
		g_pSwapChain = nullptr;
	}
	if (g_pd3dDeviceContext)
	{
		g_pd3dDeviceContext->Release();
		g_pd3dDeviceContext = nullptr;
	}
	if (g_pd3dDevice)
	{
		g_pd3dDevice->Release();
		g_pd3dDevice = nullptr;
	}
}

static void TrayAdd()
{
	if (g_isInTray)
	{
		return;
	}

	if (!g_trayIconHandle)
	{
		// Prefer your embedded icon resource if you have it (IDI_APP_ICON).
		// Otherwise fall back to default application icon.
		g_trayIconHandle = (HICON)LoadImageW(
			GetModuleHandleW(nullptr),
			MAKEINTRESOURCEW(IDI_APP_ICON),
			IMAGE_ICON,
			GetSystemMetrics(SM_CXSMICON),
			GetSystemMetrics(SM_CYSMICON),
			0);

		if (!g_trayIconHandle)
		{
			g_trayIconHandle = LoadIconW(nullptr, IDI_APPLICATION);
		}
	}

	ZeroMemory(&g_trayIconData, sizeof(g_trayIconData));
	g_trayIconData.cbSize = sizeof(g_trayIconData);
	g_trayIconData.hWnd = g_hWnd;
	g_trayIconData.uID = kTrayIconId;
	g_trayIconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	g_trayIconData.uCallbackMessage = kTrayCallbackMessage;
	g_trayIconData.hIcon = g_trayIconHandle;
	wcscpy_s(g_trayIconData.szTip, L"LocalSourceControl");

	Shell_NotifyIconW(NIM_ADD, &g_trayIconData);

	g_isInTray = true;
}

static void TrayRemove()
{
	if (!g_isInTray)
	{
		return;
	}

	Shell_NotifyIconW(NIM_DELETE, &g_trayIconData);
	g_isInTray = false;

	ZeroMemory(&g_trayIconData, sizeof(g_trayIconData));
}

void TrayUpdateBackupCount(uint32_t count)
{
	if (!g_isInTray)
	{
		return;
	}

	wchar_t tooltip[128] = {};
	swprintf_s(tooltip, L"LocalSourceControl | Backups today: %u", count);
	wcsncpy_s(g_trayIconData.szTip, tooltip, _TRUNCATE);
	g_trayIconData.uFlags = NIF_TIP;
	Shell_NotifyIconW(NIM_MODIFY, &g_trayIconData);
}

static void TrayShowContextMenu(HWND windowHandle)
{
	POINT cursorPos = {};
	GetCursorPos(&cursorPos);

	HMENU menuHandle = CreatePopupMenu();
	if (!menuHandle)
	{
		return;
	}

	InsertMenuW(menuHandle, (UINT)-1, MF_BYPOSITION | MF_STRING, kTrayMenuRestoreId, L"Restore");
	InsertMenuW(menuHandle, (UINT)-1, MF_BYPOSITION | MF_STRING, kTrayMenuExitId, L"Exit");

	// Required so the menu dismisses correctly when clicking elsewhere.
	SetForegroundWindow(windowHandle);

	TrackPopupMenu(
		menuHandle,
		TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
		cursorPos.x,
		cursorPos.y,
		0,
		windowHandle,
		nullptr);

	DestroyMenu(menuHandle);
}

static void RestoreFromTray(HWND windowHandle)
{
	ShowWindow(windowHandle, SW_SHOW);
	ShowWindow(windowHandle, SW_RESTORE);
	SetForegroundWindow(windowHandle);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
#if defined(CI_BUILD)
	g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"LocalSourceControl_SingleInstance_Mutex");
	if (g_singleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS)
	{
		BringExistingInstanceToFront();
		CloseHandle(g_singleInstanceMutex);
		g_singleInstanceMutex = nullptr;
		return 0;
	}
#endif

	HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	(void)hrCo;

	LoadSettings();

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(WNDCLASSEXW);
	wc.style = CS_CLASSDC | CS_DBLCLKS;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = L"ContinuousBackupDX11Wnd";
	wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));

	RegisterClassExW(&wc);

	RECT wr = { 0, 0, g_settings.winW, g_settings.winH };
	AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

	g_hWnd = CreateWindowW(
		wc.lpszClassName,
		L"Continuous Backup",
		WS_OVERLAPPEDWINDOW,
		g_settings.winX,
		g_settings.winY,
		wr.right - wr.left,
		wr.bottom - wr.top,
		nullptr,
		nullptr,
		wc.hInstance,
		nullptr
	);

	if (!CreateDeviceD3D(g_hWnd))
	{
		CleanupDeviceD3D();
		UnregisterClassW(wc.lpszClassName, wc.hInstance);
		CoUninitialize();
		return 1;
	}

	ShowWindow(g_hWnd, SW_SHOWDEFAULT);
	UpdateWindow(g_hWnd);
	
	TrayAdd();

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	{
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;

		ImFont* loadedFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 13.0f);

		if (loadedFont)
		{
			io.FontDefault = loadedFont;
		}
		else
		{
			io.FontDefault = io.Fonts->AddFontDefault();
		}
	}

	ImGui_ImplWin32_Init(g_hWnd);
	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

	AppInit();

	bool done = false;
	while (!done)
	{
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);

			if (msg.message == WM_QUIT)
			{
				done = true;
			}
		}

		done |= AppLoop();

		if (done)
		{
			break;
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		RECT rc;
		GetClientRect(g_hWnd, &rc);

		ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2((float)(rc.right - rc.left), (float)(rc.bottom - rc.top)), ImGuiCond_Always);

		ImGuiWindowFlags rootFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
		if (ImGui::Begin("##root", nullptr, rootFlags))
		{
			done |= AppDraw();
		}
		ImGui::End();

		ImGui::Render();

		const float clearColor[4] = { 0.08f, 0.08f, 0.08f, 1.0f };
		g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
		g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

		g_pSwapChain->Present(1, 0);
	}

	AppShutdown();

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	CleanupDeviceD3D();

	DestroyWindow(g_hWnd);
	UnregisterClassW(wc.lpszClassName, wc.hInstance);

	CoUninitialize();

	if (g_singleInstanceMutex)
	{
		CloseHandle(g_singleInstanceMutex);
		g_singleInstanceMutex = nullptr;
	}
	return 0;
}

static void UpdateWindowSettingsFromHWND(HWND hWnd)
{
	RECT windowRect = {};
	if (!GetWindowRect(hWnd, &windowRect))
	{
		return;
	}

	RECT clientRect = {};
	if (!GetClientRect(hWnd, &clientRect))
	{
		return;
	}

	int x = windowRect.left;
	int y = windowRect.top;
	int w = (clientRect.right - clientRect.left);
	int h = (clientRect.bottom - clientRect.top);

	bool changed =
		(x != g_settings.winX) ||
		(y != g_settings.winY) ||
		(w != g_settings.winW) ||
		(h != g_settings.winH);

	if (!changed)
	{
		return;
	}

	g_settings.winX = x;
	g_settings.winY = y;
	g_settings.winW = w;
	g_settings.winH = h;

	MarkSettingsDirty();
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
	{
		return true;
	}

	switch (msg)
	{
		case WM_MOVE:
		{
			UpdateWindowSettingsFromHWND(hWnd);
			MaybeSaveSettingsThrottled();
		} return 0;

		case WM_SIZE:
		{
			if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
			{
				CleanupRenderTarget();
				g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
				CreateRenderTarget();
			}

			if (wParam != SIZE_MINIMIZED)
			{
				UpdateWindowSettingsFromHWND(hWnd);
				MaybeSaveSettingsThrottled();
			}

			if (wParam == SIZE_MINIMIZED)
			{
				ShowWindow(hWnd, SW_HIDE);
				return 0;
			}

		} return 0;

		case WM_SYSCOMMAND:
		{
			if ((wParam & 0xfff0) == SC_KEYMENU)
			{
				return 0;
			}
		} break;

		case WM_DESTROY:
		{
			TrayRemove();
			PostQuitMessage(0);
			return 0;
		}

		case WM_CLOSE:
		{
			if (g_settings.minimizeOnClose)
			{
				ShowWindow(hWnd, SW_HIDE);
				return 0;
			}

			DestroyWindow(hWnd);
			return 0;
		}


		case kTrayCallbackMessage:
		{
			if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP || lParam == NIN_SELECT)
			{
				RestoreFromTray(hWnd);
				return 0;
			}
			else  if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
			{
				TrayShowContextMenu(hWnd);
				return 0;
			}
		} break;

		case WM_COMMAND:
		{
			UINT commandId = LOWORD(wParam);

			if (commandId == kTrayMenuRestoreId)
			{
				RestoreFromTray(hWnd);
				return 0;
			}
			if (commandId == kTrayMenuExitId)
			{
				DestroyWindow(hWnd);
				return 0;
			}
		} break;

	}

	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

std::wstring BrowseForFolder(const std::wstring& title)
{
	IFileDialog* pfd = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
	if (FAILED(hr) || !pfd)
	{
		return L"";
	}

	DWORD options = 0;
	pfd->GetOptions(&options);
	pfd->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
	pfd->SetTitle(title.c_str());

	hr = pfd->Show(g_hWnd);
	if (FAILED(hr))
	{
		pfd->Release();
		return L"";
	}

	IShellItem* item = nullptr;
	hr = pfd->GetResult(&item);
	if (FAILED(hr) || !item)
	{
		pfd->Release();
		return L"";
	}

	PWSTR pszPath = nullptr;
	hr = item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
	item->Release();
	pfd->Release();

	if (FAILED(hr) || !pszPath)
	{
		return L"";
	}

	std::wstring out = pszPath;
	CoTaskMemFree(pszPath);
	return out;
}

std::wstring BrowseForExeFile()
{
	std::wstring selectedPath;

	HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	bool didInitCom = SUCCEEDED(initResult);

	IFileOpenDialog* fileDialog = nullptr;
	HRESULT createResult = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fileDialog));
	if (FAILED(createResult) || !fileDialog)
	{
		if (didInitCom)
		{
			CoUninitialize();
		}
		return L"";
	}

	DWORD options = 0;
	fileDialog->GetOptions(&options);
	fileDialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);

	COMDLG_FILTERSPEC filterSpecs[] =
	{
		{ L"Executables", L"*.exe" },
		{ L"All Files", L"*.*" }
	};

	fileDialog->SetFileTypes((UINT)std::size(filterSpecs), filterSpecs);
	fileDialog->SetTitle(L"Select diff tool executable");

	// Default folder = %ProgramFiles%
	{
		wchar_t programFilesPath[MAX_PATH] = {};
		size_t requiredSize = 0;

		errno_t envResult = _wgetenv_s(
			&requiredSize,
			programFilesPath,
			MAX_PATH,
			L"ProgramFiles");

		if (envResult == 0 && requiredSize > 0)
		{
			IShellItem* defaultFolderItem = nullptr;

			HRESULT shellItemResult = SHCreateItemFromParsingName(
				programFilesPath,
				nullptr,
				IID_PPV_ARGS(&defaultFolderItem));

			if (SUCCEEDED(shellItemResult) && defaultFolderItem)
			{
				fileDialog->SetFolder(defaultFolderItem);
				fileDialog->SetDefaultFolder(defaultFolderItem);
				defaultFolderItem->Release();
			}
		}
	}

	HRESULT showResult = fileDialog->Show(nullptr);
	if (SUCCEEDED(showResult))
	{
		IShellItem* resultItem = nullptr;
		if (SUCCEEDED(fileDialog->GetResult(&resultItem)) && resultItem)
		{
			PWSTR filePath = nullptr;
			if (SUCCEEDED(resultItem->GetDisplayName(SIGDN_FILESYSPATH, &filePath)) && filePath)
			{
				selectedPath = filePath;
				CoTaskMemFree(filePath);
			}
			resultItem->Release();
		}
	}

	fileDialog->Release();

	if (didInitCom)
	{
		CoUninitialize();
	}

	return selectedPath;
}

void OpenFileWithShell(const std::wstring& filePath)
{
	if (filePath.empty())
	{
		return;
	}

	(void)ShellExecuteW(nullptr, L"open", filePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenExplorerSelectPath(const std::wstring& filePath)
{
	if (filePath.empty())
	{
		return;
	}

	std::wstring explorerArgs = L"/select,\"" + filePath + L"\"";
	(void)ShellExecuteW(nullptr, L"open", L"explorer.exe", explorerArgs.c_str(), nullptr, SW_SHOWNORMAL);
}

bool FileExists(const std::wstring& filePath)
{
	std::error_code errorCode;
	return std::fs::exists(std::fs::path(filePath), errorCode);
}
