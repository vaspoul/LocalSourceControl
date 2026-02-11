// main.cpp
// Continuous Backup (Win32 + Dear ImGui + DirectX 11)
//
// Modeled after:
// - https://github.com/ocornut/imgui/wiki/Getting-Started#example-if-you-are-using-raw-win32-api--directx11
// - https://github.com/ocornut/imgui/blob/master/examples/example_win32_directx11/main.cpp
//
// Assumptions:
// - Dear ImGui is in ./imgui
// - Backends are in the same folder: ./imgui/imgui_impl_win32.* and ./imgui/imgui_impl_dx11.*
// - You compile/link:
//		imgui/imgui.cpp
//		imgui/imgui_draw.cpp
//		imgui/imgui_tables.cpp
//		imgui/imgui_widgets.cpp
//		imgui/imgui_impl_win32.cpp
//		imgui/imgui_impl_dx11.cpp
//
// Link libs:
//		d3d11.lib dxgi.lib d3dcompiler.lib Shlwapi.lib Shell32.lib Ole32.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <ShlObj_core.h>	// BROWSEINFOW (requested)
#include <shobjidl.h>		// IFileDialog (new-style folder picker)
#include <shlwapi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace fs = std::filesystem;

// ============================================================================
// DX11 (modeled after example_win32_directx11)
// ============================================================================

static HWND					g_hWnd = nullptr;
static ID3D11Device*			g_pd3dDevice = nullptr;
static ID3D11DeviceContext*	g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*		g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

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

// ============================================================================
// App: backup logic + settings + watchers
// ============================================================================

static std::wstring UTF8ToW(const std::string& s)
{
	if (s.empty())
	{
		return L"";
	}
	int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring out;
	out.resize((size_t)len);
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
	return out;
}

static std::string WToUTF8(const std::wstring& s)
{
	if (s.empty())
	{
		return "";
	}
	int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
	std::string out;
	out.resize((size_t)len);
	WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len, nullptr, nullptr);
	return out;
}

static std::wstring TrimW(const std::wstring& s)
{
	size_t a = 0;
	while (a < s.size() && iswspace(s[a]))
	{
		++a;
	}
	size_t b = s.size();
	while (b > a && iswspace(s[b - 1]))
	{
		--b;
	}
	return s.substr(a, b - a);
}

static std::wstring ToLowerW(const std::wstring& s)
{
	std::wstring out = s;
	std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c)
	{
		return (wchar_t)towlower(c);
	});
	return out;
}

static std::string ToLowerA(const std::string& s)
{
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c)
	{
		return (char)tolower(c);
	});
	return out;
}

static std::vector<std::wstring> SplitCSVW(const std::wstring& csv)
{
	std::vector<std::wstring> out;
	std::wstring cur;

	for (wchar_t c : csv)
	{
		if (c == L',')
		{
			std::wstring t = TrimW(cur);
			if (!t.empty())
			{
				out.push_back(t);
			}
			cur.clear();
		}
		else
		{
			cur.push_back(c);
		}
	}

	std::wstring t = TrimW(cur);
	if (!t.empty())
	{
		out.push_back(t);
	}

	return out;
}

// Partial keyword match:
// - split query by whitespace
// - each token must be substring of haystackLower
static bool ContainsAllKeywords(const std::string& haystackLower, const std::string& query)
{
	std::string q = ToLowerA(query);
	size_t i = 0;

	while (i < q.size())
	{
		while (i < q.size() && isspace((unsigned char)q[i]))
		{
			++i;
		}
		if (i >= q.size())
		{
			break;
		}

		size_t j = i;
		while (j < q.size() && !isspace((unsigned char)q[j]))
		{
			++j;
		}

		std::string token = q.substr(i, j - i);
		if (!token.empty())
		{
			if (haystackLower.find(token) == std::string::npos)
			{
				return false;
			}
		}

		i = j;
	}

	return true;
}

static std::wstring NowStamp()
{
	using namespace std::chrono;
	auto now = system_clock::now();
	auto tt = system_clock::to_time_t(now);
	tm tmv = {};
	localtime_s(&tmv, &tt);

	wchar_t buf[64] = {};
	swprintf_s(buf, L"%04d%02d%02d_%02d%02d%02d",
		tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
		tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

	return buf;
}

// Sanitize "C:\foo\bar" -> "C\foo\bar" so it can be nested under backup root
static std::wstring SanitizePathForBackup(const std::wstring& absPath)
{
	std::wstring out;
	out.reserve(absPath.size());

	for (wchar_t c : absPath)
	{
		if (c == L':')
		{
			continue;
		}
		if (c == L'/')
		{
			c = L'\\';
		}
		out.push_back(c);
	}

	return out;
}

struct WatchedFolder
{
	std::wstring	path;
	bool			includeSubfolders = true;
	std::wstring	includeFiltersCSV;
	std::wstring	excludeFiltersCSV;
};

struct BackupEntry
{
	std::wstring	backupPath;
	uint64_t		fileTime = 0;
};

struct BackupIndexItem
{
	std::wstring					originalPath;
	std::vector<BackupEntry>		backups;
};

struct BackupOperation
{
	std::wstring	timeStamp;
	std::wstring	originalPath;
	std::wstring	backupPath;
	std::wstring	result;
};

struct Settings
{
	int				winX = CW_USEDEFAULT;
	int				winY = CW_USEDEFAULT;
	int				winW = 1280;
	int				winH = 720;

	std::wstring	backupRoot = L"";
	uint32_t		maxBackupSizeMB = 10240;
	uint32_t		maxBackupsPerFile = 50;

	std::vector<WatchedFolder> watched;
};

static Settings											g_settings;

static std::mutex										g_opsMutex;
static std::vector<BackupOperation>						g_ops;

static std::shared_mutex								g_indexMutex;
static std::unordered_map<std::wstring, BackupIndexItem>	g_index;

struct FolderWatcher
{
	WatchedFolder						cfg;
	std::thread							thread;
	std::atomic<bool>					stop = false;

	std::mutex							debounceMutex;
	std::unordered_map<std::wstring, uint64_t> lastEventTick;
};

static std::mutex										g_watchersMutex;
static std::vector<std::unique_ptr<FolderWatcher>>		g_watchers;

// Settings auto-save (throttled)
static std::atomic<uint64_t>							g_lastSettingsSaveTick = 0;
static std::atomic<uint64_t>							g_lastSettingsChangeTick = 0;

static void MarkSettingsDirty()
{
	g_lastSettingsChangeTick.store(GetTickCount64(), std::memory_order_relaxed);
}

static std::wstring INIPath()
{
	wchar_t exePath[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	return (fs::path(exePath).parent_path() / L"settings.ini").wstring();
}

static void SaveSettings()
{
	std::wstring ini = INIPath();
	std::ofstream f(ini, std::ios::binary);
	if (!f)
	{
		return;
	}

	auto W = [&](const std::string& s)
	{
		f.write(s.data(), (std::streamsize)s.size());
	};

	W("[Window]\n");
	W("X=" + std::to_string(g_settings.winX) + "\n");
	W("Y=" + std::to_string(g_settings.winY) + "\n");
	W("W=" + std::to_string(g_settings.winW) + "\n");
	W("H=" + std::to_string(g_settings.winH) + "\n\n");

	W("[Backup]\n");
	W("Root=" + WToUTF8(g_settings.backupRoot) + "\n");
	W("MaxSizeMB=" + std::to_string(g_settings.maxBackupSizeMB) + "\n");
	W("MaxBackupsPerFile=" + std::to_string(g_settings.maxBackupsPerFile) + "\n\n");

	W("[Watched]\n");
	W("Count=" + std::to_string((int)g_settings.watched.size()) + "\n\n");

	for (size_t i = 0; i < g_settings.watched.size(); ++i)
	{
		const auto& wf = g_settings.watched[i];
		W("[Watched." + std::to_string((int)i) + "]\n");
		W("Path=" + WToUTF8(wf.path) + "\n");
		W("IncludeSub=" + std::to_string(wf.includeSubfolders ? 1 : 0) + "\n");
		W("Include=" + WToUTF8(wf.includeFiltersCSV) + "\n");
		W("Exclude=" + WToUTF8(wf.excludeFiltersCSV) + "\n\n");
	}
}

static void MaybeSaveSettingsThrottled()
{
	uint64_t now = GetTickCount64();
	uint64_t lastChange = g_lastSettingsChangeTick.load(std::memory_order_relaxed);
	uint64_t lastSave = g_lastSettingsSaveTick.load(std::memory_order_relaxed);

	if (lastChange == 0)
	{
		return;
	}

	// Wait for brief "settling" time after changes
	if (now < lastChange + 250)
	{
		return;
	}

	// Avoid excessive writes
	if (now < lastSave + 500)
	{
		return;
	}

	SaveSettings();
	g_lastSettingsSaveTick.store(now, std::memory_order_relaxed);
}

static std::string GetINIValue(const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& ini, const std::string& section, const std::string& key, const std::string& def)
{
	auto itS = ini.find(section);
	if (itS == ini.end())
	{
		return def;
	}
	auto itK = itS->second.find(key);
	if (itK == itS->second.end())
	{
		return def;
	}
	return itK->second;
}

static void LoadSettings()
{
	Settings s = {};

	std::wstring ini = INIPath();
	std::ifstream f(ini, std::ios::binary);
	if (!f)
	{
		g_settings = s;
		return;
	}

	std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> parsed;
	std::string curSection;

	auto trimA = [](const std::string& x)
	{
		size_t a = 0;
		while (a < x.size() && isspace((unsigned char)x[a]))
		{
			++a;
		}
		size_t b = x.size();
		while (b > a && isspace((unsigned char)x[b - 1]))
		{
			--b;
		}
		return x.substr(a, b - a);
	};

	size_t i = 0;
	while (i < text.size())
	{
		size_t j = text.find('\n', i);
		if (j == std::string::npos)
		{
			j = text.size();
		}
		std::string line = text.substr(i, j - i);
		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
		i = j + 1;

		line = trimA(line);
		if (line.empty() || line[0] == ';' || line[0] == '#')
		{
			continue;
		}
		if (line.front() == '[' && line.back() == ']')
		{
			curSection = line.substr(1, line.size() - 2);
			continue;
		}

		size_t eq = line.find('=');
		if (eq == std::string::npos)
		{
			continue;
		}

		std::string k = trimA(line.substr(0, eq));
		std::string v = trimA(line.substr(eq + 1));
		parsed[curSection][k] = v;
	}

	s.winX = std::stoi(GetINIValue(parsed, "Window", "X", std::to_string(s.winX)));
	s.winY = std::stoi(GetINIValue(parsed, "Window", "Y", std::to_string(s.winY)));
	s.winW = std::stoi(GetINIValue(parsed, "Window", "W", std::to_string(s.winW)));
	s.winH = std::stoi(GetINIValue(parsed, "Window", "H", std::to_string(s.winH)));

	s.backupRoot = UTF8ToW(GetINIValue(parsed, "Backup", "Root", WToUTF8(s.backupRoot)));
	s.maxBackupSizeMB = (uint32_t)std::stoul(GetINIValue(parsed, "Backup", "MaxSizeMB", std::to_string(s.maxBackupSizeMB)));
	s.maxBackupsPerFile = (uint32_t)std::stoul(GetINIValue(parsed, "Backup", "MaxBackupsPerFile", std::to_string(s.maxBackupsPerFile)));

	int count = std::stoi(GetINIValue(parsed, "Watched", "Count", "0"));
	for (int idx = 0; idx < count; ++idx)
	{
		WatchedFolder wf = {};
		std::string sec = "Watched." + std::to_string(idx);

		wf.path = UTF8ToW(GetINIValue(parsed, sec, "Path", ""));
		wf.includeSubfolders = GetINIValue(parsed, sec, "IncludeSub", "1") != "0";
		wf.includeFiltersCSV = UTF8ToW(GetINIValue(parsed, sec, "Include", ""));
		wf.excludeFiltersCSV = UTF8ToW(GetINIValue(parsed, sec, "Exclude", ""));

		if (!wf.path.empty())
		{
			s.watched.push_back(wf);
		}
	}

	g_settings = s;
}

static void PushOperation(const BackupOperation& op)
{
	std::lock_guard<std::mutex> lock(g_opsMutex);
	g_ops.push_back(op);
	if (g_ops.size() > 100)
	{
		g_ops.erase(g_ops.begin(), g_ops.begin() + (g_ops.size() - 100));
	}
}

static uint64_t FileTimeToU64(const FILETIME& ft)
{
	ULARGE_INTEGER u = {};
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	return u.QuadPart;
}

static std::optional<uint64_t> TryGetFileWriteTimeU64(const std::wstring& path)
{
	WIN32_FILE_ATTRIBUTE_DATA fad = {};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
	{
		return std::nullopt;
	}
	return FileTimeToU64(fad.ftLastWriteTime);
}

static bool FilterMatchToken(const std::wstring& fileNameLower, const std::wstring& extWithDotLower, const std::wstring& tokenRaw)
{
	std::wstring token = ToLowerW(TrimW(tokenRaw));
	if (token.empty())
	{
		return false;
	}

	if (token.size() > 1 && token[0] == L'.' && token.find(L'*') == std::wstring::npos && token.find(L'?') == std::wstring::npos)
	{
		return extWithDotLower == token;
	}

	if (token.find(L'*') != std::wstring::npos || token.find(L'?') != std::wstring::npos)
	{
		return PathMatchSpecW(fileNameLower.c_str(), token.c_str()) != FALSE;
	}

	if (token[0] == L'.')
	{
		return extWithDotLower == token;
	}

	if (!extWithDotLower.empty())
	{
		std::wstring extNoDot = extWithDotLower.substr(1);
		if (token == extNoDot)
		{
			return true;
		}
	}

	return fileNameLower.find(token) != std::wstring::npos;
}

static bool PassesFilters(const WatchedFolder& wf, const std::wstring& fullPath)
{
	std::wstring name = ToLowerW(fs::path(fullPath).filename().wstring());
	std::wstring ext = ToLowerW(fs::path(fullPath).extension().wstring());

	std::vector<std::wstring> includes = SplitCSVW(wf.includeFiltersCSV);
	std::vector<std::wstring> excludes = SplitCSVW(wf.excludeFiltersCSV);

	for (const auto& t : excludes)
	{
		if (FilterMatchToken(name, ext, t))
		{
			return false;
		}
	}

	if (includes.empty())
	{
		return true;
	}

	for (const auto& t : includes)
	{
		if (FilterMatchToken(name, ext, t))
		{
			return true;
		}
	}

	return false;
}

static void EnsureDirExists(const fs::path& p)
{
	std::error_code ec;
	fs::create_directories(p, ec);
}

static uint64_t ComputeFolderSizeBytes(const fs::path& root)
{
	uint64_t total = 0;
	std::error_code ec;

	if (!fs::exists(root, ec))
	{
		return 0;
	}

	for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
		it != fs::recursive_directory_iterator();
		++it)
	{
		if (ec)
		{
			ec.clear();
			continue;
		}
		if (it->is_regular_file(ec))
		{
			total += (uint64_t)it->file_size(ec);
		}
	}

	return total;
}

static void EnforcePerFileLimit_Locked(BackupIndexItem& item, uint32_t maxBackupsPerFile)
{
	while (item.backups.size() > maxBackupsPerFile)
	{
		BackupEntry oldest = item.backups.front();
		item.backups.erase(item.backups.begin());

		std::error_code ec;
		fs::remove(fs::path(oldest.backupPath), ec);
	}
}

static void EnforceGlobalSizeLimit(const fs::path& backupRoot, uint32_t maxSizeMB)
{
	if (backupRoot.empty())
	{
		return;
	}

	uint64_t maxBytes = (uint64_t)maxSizeMB * 1024ull * 1024ull;
	uint64_t curBytes = ComputeFolderSizeBytes(backupRoot);
	if (curBytes <= maxBytes)
	{
		return;
	}

	struct GlobalItem
	{
		std::wstring original;
		std::wstring backupPath;
		uint64_t	 time;
	};

	std::vector<GlobalItem> all;
	{
		std::shared_lock<std::shared_mutex> lock(g_indexMutex);
		for (const auto& kv : g_index)
		{
			for (const auto& b : kv.second.backups)
			{
				all.push_back(GlobalItem{ kv.first, b.backupPath, b.fileTime });
			}
		}
	}

	std::sort(all.begin(), all.end(), [](const GlobalItem& a, const GlobalItem& b)
	{
		return a.time < b.time;
	});

	size_t i = 0;
	while (curBytes > maxBytes && i < all.size())
	{
		std::error_code ec;
		uint64_t sz = 0;

		if (fs::exists(all[i].backupPath, ec))
		{
			sz = (uint64_t)fs::file_size(all[i].backupPath, ec);
		}

		fs::remove(all[i].backupPath, ec);

		{
			std::unique_lock<std::shared_mutex> lock(g_indexMutex);
			auto it = g_index.find(all[i].original);
			if (it != g_index.end())
			{
				auto& vec = it->second.backups;
				vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const BackupEntry& e)
				{
					return e.backupPath == all[i].backupPath;
				}), vec.end());
			}
		}

		if (sz > 0 && curBytes >= sz)
		{
			curBytes -= sz;
		}

		++i;
	}
}

static std::wstring MakeBackupPath(const std::wstring& backupRoot, const std::wstring& originalFullPath)
{
	fs::path orig = fs::path(originalFullPath);
	fs::path origDir = orig.parent_path();
	fs::path stem = orig.stem();
	fs::path ext = orig.extension();

	std::wstring sanitizedDir = SanitizePathForBackup(origDir.wstring());
	fs::path dstDir = fs::path(backupRoot) / fs::path(sanitizedDir);

	std::wstring stamp = NowStamp();
	std::wstring dstFile = stem.wstring() + L"_backup_" + stamp + ext.wstring();

	return (dstDir / dstFile).wstring();
}

static bool CopyToBackupAndIndex(const WatchedFolder& wf, const std::wstring& filePath)
{
	(void)wf;

	if (g_settings.backupRoot.empty())
	{
		BackupOperation op = {};
		op.timeStamp = NowStamp();
		op.originalPath = filePath;
		op.result = L"Skipped (backup folder not set)";
		PushOperation(op);
		return false;
	}

	std::error_code ec;
	if (!fs::exists(filePath, ec) || !fs::is_regular_file(filePath, ec))
	{
		return false;
	}

	std::wstring dst = MakeBackupPath(g_settings.backupRoot, filePath);
	EnsureDirExists(fs::path(dst).parent_path());

	BOOL ok = CopyFileW(filePath.c_str(), dst.c_str(), FALSE);
	if (!ok)
	{
		DWORD err = GetLastError();
		BackupOperation op = {};
		op.timeStamp = NowStamp();
		op.originalPath = filePath;
		op.backupPath = dst;
		op.result = L"Copy failed (Win32 error " + std::to_wstring(err) + L")";
		PushOperation(op);
		return false;
	}

	uint64_t ft = 0;
	if (auto wt = TryGetFileWriteTimeU64(dst); wt.has_value())
	{
		ft = wt.value();
	}

	{
		std::unique_lock<std::shared_mutex> lock(g_indexMutex);
		auto& item = g_index[filePath];
		item.originalPath = filePath;
		item.backups.push_back(BackupEntry{ dst, ft });
		std::sort(item.backups.begin(), item.backups.end(), [](const BackupEntry& a, const BackupEntry& b)
		{
			return a.fileTime < b.fileTime;
		});
		EnforcePerFileLimit_Locked(item, g_settings.maxBackupsPerFile);
	}

	BackupOperation op = {};
	op.timeStamp = NowStamp();
	op.originalPath = filePath;
	op.backupPath = dst;
	op.result = L"OK";
	PushOperation(op);

	EnforceGlobalSizeLimit(fs::path(g_settings.backupRoot), g_settings.maxBackupSizeMB);
	return true;
}

static void ScanBackupFolder()
{
	{
		std::unique_lock<std::shared_mutex> lock(g_indexMutex);
		g_index.clear();
	}

	if (g_settings.backupRoot.empty())
	{
		return;
	}

	fs::path root = fs::path(g_settings.backupRoot);
	std::error_code ec;
	if (!fs::exists(root, ec))
	{
		return;
	}

	for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
		it != fs::recursive_directory_iterator();
		++it)
	{
		if (ec)
		{
			ec.clear();
			continue;
		}
		if (!it->is_regular_file(ec))
		{
			continue;
		}

		fs::path p = it->path();
		std::wstring stem = p.stem().wstring();
		size_t pos = stem.rfind(L"_backup_");
		if (pos == std::wstring::npos)
		{
			continue;
		}

		std::wstring originalStem = stem.substr(0, pos);
		std::wstring ext = p.extension().wstring();

		fs::path relDir = fs::relative(p.parent_path(), root, ec);
		if (ec)
		{
			ec.clear();
			continue;
		}

		// Best-effort key for UI/search, derived from backup folder layout.
		fs::path originalKey = relDir / fs::path(originalStem + ext);

		uint64_t ft = 0;
		if (auto wt = TryGetFileWriteTimeU64(p.wstring()); wt.has_value())
		{
			ft = wt.value();
		}

		std::unique_lock<std::shared_mutex> lock(g_indexMutex);
		auto& item = g_index[originalKey.wstring()];
		item.originalPath = originalKey.wstring();
		item.backups.push_back(BackupEntry{ p.wstring(), ft });
	}

	{
		std::unique_lock<std::shared_mutex> lock(g_indexMutex);
		for (auto& kv : g_index)
		{
			auto& b = kv.second.backups;
			std::sort(b.begin(), b.end(), [](const BackupEntry& a, const BackupEntry& b2)
			{
				return a.fileTime < b2.fileTime;
			});
			EnforcePerFileLimit_Locked(kv.second, g_settings.maxBackupsPerFile);
		}
	}

	EnforceGlobalSizeLimit(fs::path(g_settings.backupRoot), g_settings.maxBackupSizeMB);
}

static bool ShouldDebounce(FolderWatcher& w, const std::wstring& path, uint64_t nowTick)
{
	std::lock_guard<std::mutex> lock(w.debounceMutex);

	auto it = w.lastEventTick.find(path);
	if (it != w.lastEventTick.end())
	{
		if (nowTick - it->second < 500)
		{
			it->second = nowTick;
			return true;
		}
	}

	w.lastEventTick[path] = nowTick;
	return false;
}

static void WatchThreadProc(FolderWatcher* watcher)
{
	const WatchedFolder cfg = watcher->cfg;

	HANDLE hDir = CreateFileW(
		cfg.path.c_str(),
		FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS,
		nullptr);

	if (hDir == INVALID_HANDLE_VALUE)
	{
		BackupOperation op = {};
		op.timeStamp = NowStamp();
		op.originalPath = cfg.path;
		op.result = L"Watcher failed to open folder";
		PushOperation(op);
		return;
	}

	DWORD flags = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
	std::vector<uint8_t> buffer;
	buffer.resize(64 * 1024);

	while (!watcher->stop.load())
	{
		DWORD bytesReturned = 0;
		BOOL ok = ReadDirectoryChangesW(
			hDir,
			buffer.data(),
			(DWORD)buffer.size(),
			cfg.includeSubfolders ? TRUE : FALSE,
			flags,
			&bytesReturned,
			nullptr,
			nullptr);

		if (!ok)
		{
			DWORD err = GetLastError();
			BackupOperation op = {};
			op.timeStamp = NowStamp();
			op.originalPath = cfg.path;
			op.result = L"ReadDirectoryChangesW failed (Win32 error " + std::to_wstring(err) + L")";
			PushOperation(op);
			break;
		}

		uint64_t tick = GetTickCount64();

		FILE_NOTIFY_INFORMATION* p = (FILE_NOTIFY_INFORMATION*)buffer.data();
		while (true)
		{
			std::wstring rel(p->FileName, p->FileNameLength / sizeof(wchar_t));
			std::wstring full = (fs::path(cfg.path) / fs::path(rel)).wstring();

			bool interesting =
				p->Action == FILE_ACTION_ADDED ||
				p->Action == FILE_ACTION_MODIFIED ||
				p->Action == FILE_ACTION_RENAMED_NEW_NAME;

			if (interesting)
			{
				if (!ShouldDebounce(*watcher, full, tick))
				{
					if (PassesFilters(cfg, full))
					{
						CopyToBackupAndIndex(cfg, full);
					}
				}
			}

			if (p->NextEntryOffset == 0)
			{
				break;
			}
			p = (FILE_NOTIFY_INFORMATION*)((uint8_t*)p + p->NextEntryOffset);
		}
	}

	CloseHandle(hDir);
}

static void StopWatchers()
{
	std::lock_guard<std::mutex> lock(g_watchersMutex);

	for (auto& w : g_watchers)
	{
		w->stop.store(true);
	}

	for (auto& w : g_watchers)
	{
		if (w->thread.joinable())
		{
			w->thread.join();
		}
	}

	g_watchers.clear();
}

static void StartWatchersFromSettings()
{
	StopWatchers();

	std::lock_guard<std::mutex> lock(g_watchersMutex);
	for (const auto& wf : g_settings.watched)
	{
		auto w = std::make_unique<FolderWatcher>();
		w->cfg = wf;
		w->stop.store(false);
		w->thread = std::thread(WatchThreadProc, w.get());
		g_watchers.push_back(std::move(w));
	}
}

// ============================================================================
// New-style folder picker (IFileDialog with FOS_PICKFOLDERS)
// ============================================================================

static std::wstring BrowseForFolderNew(HWND owner, const std::wstring& title)
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

	hr = pfd->Show(owner);
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

// ============================================================================
// UI helpers
// ============================================================================

static bool InputTextStdString(const char* label, std::string& s, ImGuiInputTextFlags flags = 0)
{
	if (s.capacity() < 256)
	{
		s.reserve(256);
	}

	flags |= ImGuiInputTextFlags_CallbackResize;
	return ImGui::InputText(label, s.data(), s.capacity() + 1, flags,
		[](ImGuiInputTextCallbackData* data)
		{
			if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
			{
				auto* str = (std::string*)data->UserData;
				str->resize((size_t)data->BufTextLen);
				data->Buf = str->data();
			}
			return 0;
		}, (void*)&s);
}

static void HelpTooltip(const char* text)
{
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
	{
		ImGui::SetTooltip("%s", text);
	}
}

// ============================================================================
// UI Tabs
// ============================================================================

static void UI_WatchedFolders()
{
	ImGui::TextUnformatted("Watched folders");
	HelpTooltip(
		"Tokens are comma-separated.\n"
		"Supported token types:\n"
		"  - Extension: .png or png\n"
		"  - Wildcards: *.tmp, foo*.txt (matches filename)\n"
		"  - Substring: foo (matches filename substring)\n"
		"Exclude wins over include. If include list is empty, everything is included (subject to exclude)."
	);

	if (ImGui::Button("Add Folder"))
	{
		std::wstring p = BrowseForFolderNew(g_hWnd, L"Select folder to watch");
		if (!p.empty())
		{
			WatchedFolder wf = {};
			wf.path = p;
			wf.includeSubfolders = true;

			g_settings.watched.push_back(wf);
			MarkSettingsDirty();
			SaveSettings();
			StartWatchersFromSettings();
		}
	}

	ImGui::SameLine();

	if (ImGui::Button("Apply All"))
	{
		MarkSettingsDirty();
		SaveSettings();
		StartWatchersFromSettings();
	}

	ImGui::Separator();

	for (int i = 0; i < (int)g_settings.watched.size(); ++i)
	{
		WatchedFolder& wf = g_settings.watched[i];
		ImGui::PushID(i);

		bool open = ImGui::CollapsingHeader(WToUTF8(wf.path).c_str(), ImGuiTreeNodeFlags_DefaultOpen);
		if (open)
		{
			if (ImGui::Checkbox("Include sub-folders", &wf.includeSubfolders))
			{
				MarkSettingsDirty();
			}

			std::string inc = WToUTF8(wf.includeFiltersCSV);
			if (inc.capacity() < 512)
			{
				inc.reserve(512);
			}
			ImGui::TextUnformatted("Include filters (CSV)");
			HelpTooltip("Examples:\n  .png, .jpg\n  png, jpg\n  *.tmp\n  foo*, *bar*\n  report\nIf empty: include everything (unless excluded).");
			ImGui::SetNextItemWidth(-1.0f);
			if (InputTextStdString("##include", inc))
			{
				wf.includeFiltersCSV = UTF8ToW(inc);
				MarkSettingsDirty();
			}

			std::string exc = WToUTF8(wf.excludeFiltersCSV);
			if (exc.capacity() < 512)
			{
				exc.reserve(512);
			}
			ImGui::TextUnformatted("Exclude filters (CSV)");
			HelpTooltip("Examples:\n  .tmp, .bak\n  *autosave*\nExclude is checked first and always wins.");
			ImGui::SetNextItemWidth(-1.0f);
			if (InputTextStdString("##exclude", exc))
			{
				wf.excludeFiltersCSV = UTF8ToW(exc);
				MarkSettingsDirty();
			}

			if (ImGui::Button("Change Path"))
			{
				std::wstring p = BrowseForFolderNew(g_hWnd, L"Select folder to watch");
				if (!p.empty())
				{
					wf.path = p;
					MarkSettingsDirty();
					SaveSettings();
					StartWatchersFromSettings();
				}
			}

			ImGui::SameLine();

			if (ImGui::Button("Remove"))
			{
				g_settings.watched.erase(g_settings.watched.begin() + i);
				MarkSettingsDirty();
				SaveSettings();
				StartWatchersFromSettings();
				ImGui::PopID();
				break;
			}
		}

		ImGui::PopID();
	}
}

static void UI_BackedUpFiles()
{
	static std::string search;

	ImGui::TextUnformatted("Search");
	HelpTooltip("Search is split into whitespace keywords.\nAll keywords must appear as substrings.\nExample: \"foo bar\" matches \"foontarbartastic\".");
	ImGui::SetNextItemWidth(-1.0f);
	InputTextStdString("##search", search);

	ImGui::Separator();

	if (ImGui::BeginTable("backups", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
	{
		ImGui::TableSetupColumn("File");
		ImGui::TableSetupColumn("Backups", ImGuiTableColumnFlags_WidthFixed, 90.0f);
		ImGui::TableSetupColumn("Last backup", ImGuiTableColumnFlags_WidthFixed, 220.0f);
		ImGui::TableHeadersRow();

		std::shared_lock<std::shared_mutex> lock(g_indexMutex);

		for (const auto& kv : g_index)
		{
			const auto& item = kv.second;
			std::string pathUtf8 = WToUTF8(item.originalPath);
			std::string pathLower = ToLowerA(pathUtf8);

			if (!search.empty())
			{
				if (!ContainsAllKeywords(pathLower, search))
				{
					continue;
				}
			}

			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(pathUtf8.c_str());

			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%d", (int)item.backups.size());

			ImGui::TableSetColumnIndex(2);
			if (!item.backups.empty())
			{
				const auto& last = item.backups.back();
				std::wstring bn = fs::path(last.backupPath).filename().wstring();
				size_t p = bn.rfind(L"_backup_");
				if (p != std::wstring::npos)
				{
					std::wstring stampPlus = bn.substr(p + 8);
					ImGui::TextUnformatted(WToUTF8(stampPlus).c_str());
				}
				else
				{
					ImGui::TextUnformatted("-");
				}
			}
			else
			{
				ImGui::TextUnformatted("-");
			}
		}

		ImGui::EndTable();
	}
}

static void UI_Operations()
{
	ImGui::TextUnformatted("Last 100 operations");
	HelpTooltip("Shows copy successes/failures and reasons for skips.");
	ImGui::Separator();

	if (ImGui::BeginTable("ops", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
	{
		ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 160.0f);
		ImGui::TableSetupColumn("Original");
		ImGui::TableSetupColumn("Backup");
		ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, 220.0f);
		ImGui::TableHeadersRow();

		std::lock_guard<std::mutex> lock(g_opsMutex);
		for (const auto& op : g_ops)
		{
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(WToUTF8(op.timeStamp).c_str());

			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(WToUTF8(op.originalPath).c_str());

			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(WToUTF8(op.backupPath).c_str());

			ImGui::TableSetColumnIndex(3);
			ImGui::TextUnformatted(WToUTF8(op.result).c_str());
		}

		ImGui::EndTable();
	}
}

static void UI_Settings()
{
	static std::string backupRootUtf8;
	if (backupRootUtf8.empty())
	{
		backupRootUtf8 = WToUTF8(g_settings.backupRoot);
		if (backupRootUtf8.capacity() < 512)
		{
			backupRootUtf8.reserve(512);
		}
	}

	ImGui::TextUnformatted("Backup folder");

	if (ImGui::Button("Browse Backup Folder"))
	{
		std::wstring p = BrowseForFolderNew(g_hWnd, L"Select backup folder");
		if (!p.empty())
		{
			g_settings.backupRoot = p;

			// Requirement: button updates the local root variable
			backupRootUtf8 = WToUTF8(g_settings.backupRoot);

			MarkSettingsDirty();
			SaveSettings();
			ScanBackupFolder();
		}
	}
	HelpTooltip("Backups are nested by original path under this root.");

	ImGui::SetNextItemWidth(-1.0f);
	if (InputTextStdString("##backupRoot", backupRootUtf8))
	{
		g_settings.backupRoot = UTF8ToW(backupRootUtf8);
		MarkSettingsDirty();
	}

	ImGui::Separator();

	int maxSize = (int)g_settings.maxBackupSizeMB;
	ImGui::TextUnformatted("Max backup folder size (MB)");
	HelpTooltip("When exceeded, oldest backups across all files are deleted until within the limit.");
	ImGui::SetNextItemWidth(240.0f);
	if (ImGui::InputInt("##maxsize", &maxSize))
	{
		if (maxSize < 1)
		{
			maxSize = 1;
		}
		g_settings.maxBackupSizeMB = (uint32_t)maxSize;
		MarkSettingsDirty();
	}

	int maxPerFile = (int)g_settings.maxBackupsPerFile;
	ImGui::TextUnformatted("Max backups per file");
	HelpTooltip("Per original file, keep at most this many backups. Oldest backups are deleted first.");
	ImGui::SetNextItemWidth(240.0f);
	if (ImGui::InputInt("##maxperfile", &maxPerFile))
	{
		if (maxPerFile < 1)
		{
			maxPerFile = 1;
		}
		g_settings.maxBackupsPerFile = (uint32_t)maxPerFile;
		MarkSettingsDirty();
	}

	ImGui::Separator();

	if (ImGui::Button("Apply"))
	{
		MarkSettingsDirty();
		SaveSettings();
		ScanBackupFolder();
		EnforceGlobalSizeLimit(fs::path(g_settings.backupRoot), g_settings.maxBackupSizeMB);
	}

	ImGui::SameLine();

	if (ImGui::Button("Rescan Backup Folder"))
	{
		ScanBackupFolder();
	}
}

// ============================================================================
// Window size/pos tracking
// ============================================================================

static void UpdateWindowSettingsFromHWND(HWND hWnd)
{
	RECT wrc = {};
	if (!GetWindowRect(hWnd, &wrc))
	{
		return;
	}

	int x = wrc.left;
	int y = wrc.top;
	int w = (wrc.right - wrc.left);
	int h = (wrc.bottom - wrc.top);

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

// ============================================================================
// Win32
// ============================================================================

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
			PostQuitMessage(0);
			return 0;
		}
	}

	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ============================================================================
// Entry
// ============================================================================

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
	HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	(void)hrCo;

	LoadSettings();

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(WNDCLASSEXW);
	wc.style = CS_CLASSDC;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = L"ContinuousBackupDX11Wnd";

	RegisterClassExW(&wc);

	RECT wr = { 0, 0, g_settings.winW, g_settings.winH };
	AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

	g_hWnd = CreateWindowW(
		wc.lpszClassName,
		L"Continuous Backup (DX11)",
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

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();

	ImGui_ImplWin32_Init(g_hWnd);
	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

	ScanBackupFolder();
	StartWatchersFromSettings();

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

		MaybeSaveSettingsThrottled();

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
			if (ImGui::BeginTabBar("tabs"))
			{
				if (ImGui::BeginTabItem("Watched Folders"))
				{
					UI_WatchedFolders();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Backed Up Files"))
				{
					UI_BackedUpFiles();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Operations"))
				{
					UI_Operations();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Settings"))
				{
					UI_Settings();
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
		}
		ImGui::End();

		ImGui::Render();

		const float clearColor[4] = { 0.08f, 0.08f, 0.08f, 1.0f };
		g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
		g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

		g_pSwapChain->Present(1, 0);
	}

	StopWatchers();

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	CleanupDeviceD3D();

	DestroyWindow(g_hWnd);
	UnregisterClassW(wc.lpszClassName, wc.hInstance);

	CoUninitialize();
	return 0;
}
