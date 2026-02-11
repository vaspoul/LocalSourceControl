#include "main.h"
#include "settings.h"
#include "util.h"

Settings				g_settings;
std::atomic<uint64_t>	g_lastSettingsSaveTick = 0;
std::atomic<uint64_t>	g_lastSettingsChangeTick = 0;

void MarkSettingsDirty()
{
	g_lastSettingsChangeTick.store(GetTickCount64(), std::memory_order_relaxed);
}

std::wstring INIPath()
{
	wchar_t appDataPath[MAX_PATH] = {};
	if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appDataPath)))
	{
		return L"settings.ini";
	}

	fs::path dir = fs::path(appDataPath) / L"LocalSourceControl";

	std::error_code ec;
	fs::create_directories(dir, ec);

	return (dir / L"settings.ini").wstring();
}

void SaveSettings()
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

void MaybeSaveSettingsThrottled()
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

std::string GetINIValue(const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& ini, const std::string& section, const std::string& key, const std::string& def)
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

void LoadSettings()
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
