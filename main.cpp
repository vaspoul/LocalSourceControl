#include "main.h"
#include "app.h"
#include "util.h"
#include "settings.h"

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

struct FolderWatcher
{
	WatchedFolder						cfg;
	std::thread							thread;
	std::atomic<bool>					stop = false;

	std::mutex							debounceMutex;
	std::unordered_map<std::wstring, uint64_t> lastEventTick;
};


static std::mutex											g_opsMutex;
static std::vector<BackupOperation>							g_ops;

static std::shared_mutex									g_indexMutex;
static std::unordered_map<std::wstring, BackupIndexItem>	g_index;

static std::mutex											g_watchersMutex;
static std::vector<std::unique_ptr<FolderWatcher>>			g_watchers;

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
		std::wstring p = BrowseForFolder(L"Select folder to watch");
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
				std::wstring p = BrowseForFolder(L"Select folder to watch");
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
		std::wstring p = BrowseForFolder(L"Select backup folder");

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

void AppInit()
{
	ScanBackupFolder();
	StartWatchersFromSettings();
}

bool AppLoop()
{
	MaybeSaveSettingsThrottled();
	
	return false;
}

bool AppDraw()
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

	return false;
}

void AppShutdown()
{
	StopWatchers();
}

