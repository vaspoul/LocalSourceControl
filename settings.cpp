#include "main.h"
#include "settings.h"
#include "util.h"

Settings				g_settings;
std::atomic<uint64_t>	g_lastSettingsSaveTick = 0;
std::atomic<uint64_t>	g_lastSettingsChangeTick = 0;

std::wstring INIPath()
{
	wchar_t appDataPath[MAX_PATH] = {};
	if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appDataPath)))
	{
		return L"settings.ini";
	}

	std::fs::path settingsDir = std::fs::path(appDataPath) / L"LocalSourceControl";

	std::error_code errorCode;
	std::fs::create_directories(settingsDir, errorCode);

	return (settingsDir / L"settings.ini").wstring();
}

static void SanitizeWindowPlacement(Settings& settings)
{
	int virtualDesktopLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
	int virtualDesktopTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
	int virtualDesktopWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	int virtualDesktopHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

	if (virtualDesktopWidth <= 0 || virtualDesktopHeight <= 0)
	{
		return;
	}

	RECT virtualDesktopRect = {};
	virtualDesktopRect.left = virtualDesktopLeft;
	virtualDesktopRect.top = virtualDesktopTop;
	virtualDesktopRect.right = virtualDesktopLeft + virtualDesktopWidth;
	virtualDesktopRect.bottom = virtualDesktopTop + virtualDesktopHeight;

	const int minWindowWidth = 640;
	const int minWindowHeight = 360;

	int windowWidth = settings.winW;
	int windowHeight = settings.winH;

	if (windowWidth < minWindowWidth)
	{
		windowWidth = minWindowWidth;
	}
	if (windowHeight < minWindowHeight)
	{
		windowHeight = minWindowHeight;
	}

	if (windowWidth > virtualDesktopWidth)
	{
		windowWidth = virtualDesktopWidth;
	}
	if (windowHeight > virtualDesktopHeight)
	{
		windowHeight = virtualDesktopHeight;
	}

	int windowLeft = settings.winX;
	int windowTop = settings.winY;

	RECT windowRect = {};
	windowRect.left = windowLeft;
	windowRect.top = windowTop;
	windowRect.right = windowLeft + windowWidth;
	windowRect.bottom = windowTop + windowHeight;

	RECT intersectionRect = {};
	BOOL intersects = IntersectRect(&intersectionRect, &windowRect, &virtualDesktopRect);

	if (!intersects)
	{
		// Window is entirely off-screen -> center it on the virtual desktop
		windowLeft = virtualDesktopRect.left + (virtualDesktopWidth - windowWidth) / 2;
		windowTop = virtualDesktopRect.top + (virtualDesktopHeight - windowHeight) / 2;

		windowRect.left = windowLeft;
		windowRect.top = windowTop;
		windowRect.right = windowLeft + windowWidth;
		windowRect.bottom = windowTop + windowHeight;
	}

	// Clamp position so the window stays fully within the virtual desktop
	int maxLeft = virtualDesktopRect.right - windowWidth;
	int maxTop = virtualDesktopRect.bottom - windowHeight;

	if (windowLeft < virtualDesktopRect.left)
	{
		windowLeft = virtualDesktopRect.left;
	}
	if (windowTop < virtualDesktopRect.top)
	{
		windowTop = virtualDesktopRect.top;
	}
	if (windowLeft > maxLeft)
	{
		windowLeft = maxLeft;
	}
	if (windowTop > maxTop)
	{
		windowTop = maxTop;
	}

	settings.winX = windowLeft;
	settings.winY = windowTop;
	settings.winW = windowWidth;
	settings.winH = windowHeight;
}

void SaveSettings()
{
	std::wstring iniPath = INIPath();
	std::ofstream fileStream(iniPath, std::ios::binary);
	if (!fileStream)
	{
		return;
	}

	auto WriteText = [&](const std::string& text)
	{
		fileStream.write(text.data(), (std::streamsize)text.size());
	};

	WriteText("[Window]\n");
	WriteText("X=" + std::to_string(g_settings.winX) + "\n");
	WriteText("Y=" + std::to_string(g_settings.winY) + "\n");
	WriteText("W=" + std::to_string(g_settings.winW) + "\n");
	WriteText("H=" + std::to_string(g_settings.winH) + "\n\n");
	WriteText("MinimizeOnClose=" + std::to_string(g_settings.minimizeOnClose ? 1 : 0) + "\n");

	WriteText("[Backup]\n");
	WriteText("Root=" + WToUTF8(g_settings.backupRoot) + "\n");
	WriteText("MaxSizeMB=" + std::to_string(g_settings.maxBackupSizeMB) + "\n");
	WriteText("MaxBackupsPerFile=" + std::to_string(g_settings.maxBackupsPerFile) + "\n\n");

	// Diff tool settings (used by Ctrl+D in history)
	WriteText("[Tools]\n");
	WriteText("DiffTool=" + WToUTF8(g_settings.diffToolPath) + "\n\n");

	WriteText("[Watched]\n");
	WriteText("Count=" + std::to_string((int)g_settings.watched.size()) + "\n\n");

	for (size_t watchedIndex = 0; watchedIndex < g_settings.watched.size(); ++watchedIndex)
	{
		const auto& watchedFolder = g_settings.watched[watchedIndex];

		WriteText("[Watched." + std::to_string((int)watchedIndex) + "]\n");
		WriteText("Path=" + WToUTF8(watchedFolder.path) + "\n");
		WriteText("IncludeSub=" + std::to_string(watchedFolder.includeSubfolders ? 1 : 0) + "\n");
		WriteText("Include=" + WToUTF8(watchedFolder.includeFiltersCSV) + "\n");
		WriteText("Exclude=" + WToUTF8(watchedFolder.excludeFiltersCSV) + "\n\n");
	}
}

static std::string GetINIValue(
	const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& parsedIni,
	const std::string& sectionName,
	const std::string& keyName,
	const std::string& defaultValue)
{
	auto foundSection = parsedIni.find(sectionName);
	if (foundSection == parsedIni.end())
	{
		return defaultValue;
	}

	auto foundKey = foundSection->second.find(keyName);
	if (foundKey == foundSection->second.end())
	{
		return defaultValue;
	}

	return foundKey->second;
}

void LoadSettings()
{
	Settings loadedSettings = {};

	std::wstring iniPath = INIPath();
	std::ifstream fileStream(iniPath, std::ios::binary);
	if (!fileStream)
	{
		g_settings = loadedSettings;
		return;
	}

	std::string fileText((std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());

	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> parsedIni;
	std::string currentSection;

	auto TrimAscii = [](const std::string& input)
	{
		size_t beginIndex = 0;
		while (beginIndex < input.size() && isspace((unsigned char)input[beginIndex]))
		{
			++beginIndex;
		}

		size_t endIndex = input.size();
		while (endIndex > beginIndex && isspace((unsigned char)input[endIndex - 1]))
		{
			--endIndex;
		}

		return input.substr(beginIndex, endIndex - beginIndex);
	};

	size_t textIndex = 0;
	while (textIndex < fileText.size())
	{
		size_t lineEndIndex = fileText.find('\n', textIndex);
		if (lineEndIndex == std::string::npos)
		{
			lineEndIndex = fileText.size();
		}

		std::string lineText = fileText.substr(textIndex, lineEndIndex - textIndex);
		if (!lineText.empty() && lineText.back() == '\r')
		{
			lineText.pop_back();
		}

		textIndex = lineEndIndex + 1;

		lineText = TrimAscii(lineText);
		if (lineText.empty() || lineText[0] == ';' || lineText[0] == '#')
		{
			continue;
		}

		if (lineText.front() == '[' && lineText.back() == ']')
		{
			currentSection = lineText.substr(1, lineText.size() - 2);
			continue;
		}

		size_t equalsIndex = lineText.find('=');
		if (equalsIndex == std::string::npos)
		{
			continue;
		}

		std::string keyText = TrimAscii(lineText.substr(0, equalsIndex));
		std::string valueText = TrimAscii(lineText.substr(equalsIndex + 1));
		parsedIni[currentSection][keyText] = valueText;
	}

	loadedSettings.winX = std::stoi(GetINIValue(parsedIni, "Window", "X", std::to_string(loadedSettings.winX)));
	loadedSettings.winY = std::stoi(GetINIValue(parsedIni, "Window", "Y", std::to_string(loadedSettings.winY)));
	loadedSettings.winW = std::stoi(GetINIValue(parsedIni, "Window", "W", std::to_string(loadedSettings.winW)));
	loadedSettings.winH = std::stoi(GetINIValue(parsedIni, "Window", "H", std::to_string(loadedSettings.winH)));
	loadedSettings.minimizeOnClose = GetINIValue(parsedIni, "Window", "MinimizeOnClose", "1") != "0";


	loadedSettings.backupRoot = UTF8ToW(GetINIValue(parsedIni, "Backup", "Root", WToUTF8(loadedSettings.backupRoot)));
	loadedSettings.maxBackupSizeMB = (uint32_t)std::stoul(GetINIValue(parsedIni, "Backup", "MaxSizeMB", std::to_string(loadedSettings.maxBackupSizeMB)));
	loadedSettings.maxBackupsPerFile = (uint32_t)std::stoul(GetINIValue(parsedIni, "Backup", "MaxBackupsPerFile", std::to_string(loadedSettings.maxBackupsPerFile)));

	// Diff tool path
	loadedSettings.diffToolPath = UTF8ToW(GetINIValue(parsedIni, "Tools", "DiffTool", WToUTF8(loadedSettings.diffToolPath)));

	int watchedCount = std::stoi(GetINIValue(parsedIni, "Watched", "Count", "0"));
	for (int watchedIndex = 0; watchedIndex < watchedCount; ++watchedIndex)
	{
		WatchedFolder watchedFolder = {};
		std::string watchedSection = "Watched." + std::to_string(watchedIndex);

		watchedFolder.path = UTF8ToW(GetINIValue(parsedIni, watchedSection, "Path", ""));
		watchedFolder.includeSubfolders = GetINIValue(parsedIni, watchedSection, "IncludeSub", "1") != "0";
		watchedFolder.includeFiltersCSV = UTF8ToW(GetINIValue(parsedIni, watchedSection, "Include", ""));
		watchedFolder.excludeFiltersCSV = UTF8ToW(GetINIValue(parsedIni, watchedSection, "Exclude", ""));

		if (!watchedFolder.path.empty())
		{
			loadedSettings.watched.push_back(watchedFolder);
		}
	}

	SanitizeWindowPlacement(loadedSettings);

	g_settings = loadedSettings;
}

void MarkSettingsDirty()
{
	g_lastSettingsChangeTick.store(GetTickCount64(), std::memory_order_relaxed);
}

void MaybeSaveSettingsThrottled()
{
	uint64_t nowTick = GetTickCount64();
	uint64_t lastChangeTick = g_lastSettingsChangeTick.load(std::memory_order_relaxed);
	uint64_t lastSaveTick = g_lastSettingsSaveTick.load(std::memory_order_relaxed);

	if (lastChangeTick == 0 || lastChangeTick <= lastSaveTick)
	{
		return;
	}

	// Wait for brief "settling" time after changes
	if (nowTick < lastChangeTick + 250)
	{
		return;
	}

	// Avoid excessive writes
	if (nowTick < lastSaveTick + 500)
	{
		return;
	}

	SaveSettings();

	g_lastSettingsSaveTick.store(nowTick, std::memory_order_relaxed);
}
