﻿﻿﻿// main.cpp

#include "main.h"
#include "app.h"
#include "util.h"
#include "settings.h"
#include "imgui/imgui_internal.h"

using namespace std::chrono;

typedef std::chrono::system_clock::time_point TimePoint;

struct BackupFile
{
	std::vector<TimePoint>	backups;
	std::wstring			originalPath;

	void SortBackupTimes()
	{
		std::sort(backups.begin(), backups.end(), [](const TimePoint& left, const TimePoint& right)
		{
			return left < right;
		});
	}
};

struct FolderWatcher
{
	WatchedFolder								config;
	std::thread									workerThread;
	HANDLE										directoryHandle = INVALID_HANDLE_VALUE;
	std::atomic<bool>							stopRequested = false;

	std::mutex									debounceMutex;
	std::unordered_map<std::wstring, uint64_t>	lastEventTickByPath;
};

static std::shared_mutex									g_indexMutex;
static std::list<BackupFile>								g_backupIndex;
static std::mutex											g_historyMutex;

struct HistoryEntry
{
	std::wstring originalPath;
	std::wstring backupPath;
	TimePoint timePoint = {};
};

static std::vector<HistoryEntry>							g_todayHistory;

static std::mutex											g_watchersMutex;
static std::vector<std::unique_ptr<FolderWatcher>>			g_watchers;

static std::atomic<uint32_t>								g_backupsToday;
static std::wstring											g_todayPrefix;
static std::atomic<bool>									g_isPaused;
static std::atomic<uint64_t>								g_pauseUntilTick;
		
// Sanitize "C:\foo\bar" -> "C\foo\bar" so it can be nested under backup root
static std::wstring SanitizePathForBackup(const std::wstring& absolutePath)
{
	std::wstring sanitizedPath;
	sanitizedPath.reserve(absolutePath.size());

	for (wchar_t currentChar : absolutePath)
	{
		if (currentChar == L':')
		{
			continue;
		}
		if (currentChar == L'/')
		{
			currentChar = L'\\';
		}
		sanitizedPath.push_back(currentChar);
	}

	return sanitizedPath;
}

static std::wstring UnsanitizePathFromBackupLayout(const std::wstring& relativePathUnderBackupRoot)
{
	// Expected: "C\temp\file.txt" -> "C:\temp\file.txt"
	// Best-effort. Also normalizes '/' to '\'.
	std::wstring out = relativePathUnderBackupRoot;

	for (wchar_t& currentChar : out)
	{
		if (currentChar == L'/')
		{
			currentChar = L'\\';
		}
	}

	if (out.size() >= 2)
	{
		wchar_t driveLetter = out[0];
		wchar_t slashChar = out[1];

		if (((driveLetter >= L'A' && driveLetter <= L'Z') || (driveLetter >= L'a' && driveLetter <= L'z')) &&
			(slashChar == L'\\'))
		{
			out.insert(1, L":");
		}
	}

	return out;
}

static std::wstring FormatTimestampForDisplay(const TimePoint& timePoint)
{
	using namespace std::chrono;
	auto tt = system_clock::to_time_t(timePoint);
	tm tmv = {};
	localtime_s(&tmv, &tt);

	static const wchar_t* monthNames[12] =
	{
		L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
		L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"
	};

	const wchar_t* monthName = (tmv.tm_mon >= 0 && tmv.tm_mon < 12) ? monthNames[tmv.tm_mon] : L"???";

	return fmt::format(
		L"{:02d} {} {:04d} {:02d}:{:02d}:{:02d}",
		tmv.tm_mday,
		monthName,
		tmv.tm_year + 1900,
		tmv.tm_hour,
		tmv.tm_min,
		tmv.tm_sec);
}

static bool TryParseBackupTimestampToTimePoint(const std::wstring& backupFileName, TimePoint& outTimePoint)
{
	size_t markerPos = backupFileName.rfind(L"_backup_");
	if (markerPos == std::wstring::npos)
	{
		return false;
	}

	std::wstring stamp = backupFileName.substr(markerPos + 8);

	int year = 0, month = 0, day = 0;
	int hour = 0, minute = 0, second = 0;

	if (swscanf_s(
		stamp.c_str(),
		L"%4d_%2d_%2d__%2d_%2d_%2d",
		&year, &month, &day,
		&hour, &minute, &second) != 6)
	{
		return false;
	}

	if (month < 1 || month > 12)
	{
		return false;
	}

	tm tmv = {};
	tmv.tm_year = year - 1900;
	tmv.tm_mon = month - 1;
	tmv.tm_mday = day;
	tmv.tm_hour = hour;
	tmv.tm_min = minute;
	tmv.tm_sec = second;
	tmv.tm_isdst = -1;

	time_t tt = mktime(&tmv);
	if (tt == (time_t)-1)
	{
		return false;
	}

	outTimePoint = std::chrono::system_clock::from_time_t(tt);
	return true;
}

static std::wstring BuildTodayPrefixFromTimePoint(const TimePoint& timePoint)
{
	using namespace std::chrono;
	auto tt = system_clock::to_time_t(timePoint);
	tm tmv = {};
	localtime_s(&tmv, &tt);

	return fmt::format(
		L"_backup_{:04d}_{:02d}_{:02d}__",
		tmv.tm_year + 1900,
		tmv.tm_mon + 1,
		tmv.tm_mday);
}

static std::wstring MakeBackupPathFromTimePoint(const std::wstring& backupRoot, const std::wstring& originalFullPath, const TimePoint& timePoint)
{
	std::fs::path originalPath(originalFullPath);
	std::fs::path originalDir = originalPath.parent_path();
	std::fs::path originalStem = originalPath.stem();
	std::fs::path originalExt = originalPath.extension();

	std::wstring sanitizedDir = SanitizePathForBackup(originalDir.wstring());
	std::fs::path destinationDir = std::fs::path(backupRoot) / std::fs::path(sanitizedDir);

	auto tt = system_clock::to_time_t(timePoint);
	tm tmv = {};
	localtime_s(&tmv, &tt);

	std::wstring timestamp = fmt::format(	L"{:04d}_{:02d}_{:02d}__{:02d}_{:02d}_{:02d}",
											tmv.tm_year + 1900,
											tmv.tm_mon + 1,
											tmv.tm_mday,
											tmv.tm_hour,
											tmv.tm_min,
											tmv.tm_sec);

	std::wstring destinationFileName = originalStem.wstring() + L"_backup_" + timestamp + originalExt.wstring();

	return (destinationDir / destinationFileName).wstring();
}

static std::wstring MakeBackupWildcardPath(const std::wstring& backupRoot, const std::wstring& originalFullPath)
{
	std::fs::path originalPath(originalFullPath);
	std::fs::path originalDir = originalPath.parent_path();
	std::fs::path originalStem = originalPath.stem();
	std::fs::path originalExt = originalPath.extension();

	std::wstring sanitizedDir = SanitizePathForBackup(originalDir.wstring());
	std::fs::path destinationDir = std::fs::path(backupRoot) / std::fs::path(sanitizedDir);

	std::wstring destinationFileName = originalStem.wstring() + L"_backup_*" + originalExt.wstring();

	return (destinationDir / destinationFileName).wstring();
}

static std::wstring NormalizePathSlashes(std::wstring value)
{
	for (wchar_t& currentChar : value)
	{
		if (currentChar == L'/')
		{
			currentChar = L'\\';
		}
	}

	return value;
}

static bool IsPaused()
{
	if (!g_isPaused.load(std::memory_order_relaxed))
	{
		return false;
	}

	uint64_t pauseUntil = g_pauseUntilTick.load(std::memory_order_relaxed);

	// infinite pause
	if (pauseUntil == 0)
	{
		return true;
	}

	uint64_t nowTick = GetTickCount64();
	if (nowTick >= pauseUntil)
	{
		g_pauseUntilTick.store(0, std::memory_order_relaxed);
		g_isPaused.store(false, std::memory_order_relaxed);
		return false;
	}

	return true;
}

static void RemoveFromTodayHistory(const std::wstring& originalPath, const TimePoint& timePoint)
{
	std::lock_guard<std::mutex> lock(g_historyMutex);

	g_todayHistory.erase(
		std::remove_if(g_todayHistory.begin(), g_todayHistory.end(), [&](const HistoryEntry& entry)
		{
			return entry.originalPath == originalPath && entry.timePoint == timePoint;
		}),
		g_todayHistory.end());
}

static void InsertTodayHistory(const std::wstring& originalPath, const TimePoint& timePoint)
{
	HistoryEntry item = {};
	item.originalPath = originalPath;
	item.timePoint = timePoint;
	item.backupPath = MakeBackupPathFromTimePoint(g_settings.backupRoot, originalPath, timePoint);

	std::lock_guard<std::mutex> lock(g_historyMutex);

	auto insertIt = std::lower_bound(g_todayHistory.begin(), g_todayHistory.end(), timePoint,
		[](const HistoryEntry& entry, const TimePoint& value)
		{
			return entry.timePoint > value;
		});

	g_todayHistory.insert(insertIt, std::move(item));

}

static void RebuildTodayHistory()
{
	std::vector<HistoryEntry> rebuilt;

	{
		std::shared_lock<std::shared_mutex> lock(g_indexMutex);

		for (const BackupFile& entry : g_backupIndex)
		{
			for (const TimePoint& timePoint : entry.backups)
			{
				if (BuildTodayPrefixFromTimePoint(timePoint) != g_todayPrefix)
				{
					continue;
				}

				HistoryEntry item = {};
				item.originalPath = entry.originalPath;
				item.timePoint = timePoint;
				item.backupPath = MakeBackupPathFromTimePoint(g_settings.backupRoot, entry.originalPath, timePoint);
				rebuilt.push_back(std::move(item));
			}
		}
	}

	std::sort(rebuilt.begin(), rebuilt.end(), [](const HistoryEntry& left, const HistoryEntry& right)
	{
		return left.timePoint > right.timePoint;
	});


	{
		std::lock_guard<std::mutex> lock(g_historyMutex);
		g_todayHistory = std::move(rebuilt);
	}
}

static BackupFile& GetOrCreateBackupEntry_Locked(const std::wstring& originalPath)
{
	auto itr = g_backupIndex.end();

	for (itr = g_backupIndex.begin(); itr != g_backupIndex.end(); ++itr)
	{
		if (itr->originalPath == originalPath)
		{
			return *itr;
		}
	}

	BackupFile entry = {};
	entry.originalPath = originalPath;
	g_backupIndex.push_back(std::move(entry));
	return g_backupIndex.back();
}

static bool FilterMatchToken(
	const std::wstring& fileNameLower,
	const std::wstring& extWithDotLower,
	const std::wstring& relativePathLowerWithLeadingSlash,
	const std::wstring& fullPathLower,
	const std::wstring& tokenRaw)
{
	std::wstring token = NormalizePathSlashes(ToLower(Trim(tokenRaw)));
	if (token.empty())
	{
		return false;
	}

	bool hasWildcard = (token.find(L'*') != std::wstring::npos || token.find(L'?') != std::wstring::npos);
	bool hasPathSeparator = (token.find(L'\\') != std::wstring::npos);

	if (hasPathSeparator)
	{
		if (hasWildcard)
		{
			if (PathMatchSpecW(relativePathLowerWithLeadingSlash.c_str(), token.c_str()) != FALSE)
			{
				return true;
			}

			// Path tokens that start with '\' are often intended as "anywhere under the watched tree".
			if (!token.empty() && token[0] == L'\\')
			{
				std::wstring anyDepthToken = L"*" + token;
				if (PathMatchSpecW(relativePathLowerWithLeadingSlash.c_str(), anyDepthToken.c_str()) != FALSE)
				{
					return true;
				}
			}

			return PathMatchSpecW(fullPathLower.c_str(), token.c_str()) != FALSE;
		}

		return relativePathLowerWithLeadingSlash.find(token) != std::wstring::npos;
	}

	if (token.size() > 1 && token[0] == L'.' && token.find(L'*') == std::wstring::npos && token.find(L'?') == std::wstring::npos)
	{
		return extWithDotLower == token;
	}

	if (hasWildcard)
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
	
	return fullPathLower.find(token) != std::wstring::npos;
}

static bool PassesFilters(const WatchedFolder& watchedFolder, const std::wstring& fullPath)
{
	std::wstring fileNameLower = ToLower(std::fs::path(fullPath).filename().wstring());
	std::wstring extLower = ToLower(std::fs::path(fullPath).extension().wstring());
	std::wstring fullPathLower = NormalizePathSlashes(ToLower(std::fs::path(fullPath).lexically_normal().wstring()));

	std::wstring relativePathLower = fileNameLower;
	std::error_code errorCode;
	std::fs::path relativePath = std::fs::relative(std::fs::path(fullPath), std::fs::path(watchedFolder.path), errorCode);
	if (!errorCode)
	{
		std::wstring relativePathCandidate = relativePath.wstring();
		if (!relativePathCandidate.empty() && relativePathCandidate.rfind(L"..", 0) != 0)
		{
			relativePathLower = ToLower(relativePathCandidate);
		}
	}

	relativePathLower = NormalizePathSlashes(relativePathLower);
	std::wstring relativePathLowerWithLeadingSlash = L"\\" + relativePathLower;

	std::vector<std::wstring> includeTokens = SplitCSV(watchedFolder.includeFiltersCSV);
	std::vector<std::wstring> excludeTokens = SplitCSV(watchedFolder.excludeFiltersCSV);

	for (const auto& excludeToken : excludeTokens)
	{
		if (FilterMatchToken(fileNameLower, extLower, relativePathLowerWithLeadingSlash, fullPathLower, excludeToken))
		{
			return false;
		}
	}

	if (includeTokens.empty())
	{
		return true;
	}

	for (const auto& includeToken : includeTokens)
	{
		if (FilterMatchToken(fileNameLower, extLower, relativePathLowerWithLeadingSlash, fullPathLower, includeToken))
		{
			return true;
		}
	}

	return false;
}

static void EnsureDirExists(const std::fs::path& directoryPath)
{
	std::error_code errorCode;
	std::fs::create_directories(directoryPath, errorCode);
}

static uint64_t ComputeFolderSizeBytes(const std::fs::path& rootPath)
{
	uint64_t totalBytes = 0;
	std::error_code errorCode;

	if (!std::fs::exists(rootPath, errorCode))
	{
		return 0;
	}

	for (auto iterator = std::fs::recursive_directory_iterator(rootPath, std::fs::directory_options::skip_permission_denied, errorCode);
		iterator != std::fs::recursive_directory_iterator();
		++iterator)
	{
		if (errorCode)
		{
			errorCode.clear();
			continue;
		}

		if (iterator->is_regular_file(errorCode))
		{
			totalBytes += (uint64_t)iterator->file_size(errorCode);
		}
	}

	return totalBytes;
}

static void EnforcePerFileLimit_Locked(BackupFile& entry, uint32_t maxBackupsPerFile)
{
	if (maxBackupsPerFile == 0)
	{
		return;
	}

	entry.SortBackupTimes();

	size_t validCount = entry.backups.size();
	while (validCount > maxBackupsPerFile)
	{
		auto oldestIt = entry.backups.begin();
		if (oldestIt == entry.backups.end())
		{
			break;
		}

		TimePoint oldestTimePoint = *oldestIt;
		entry.backups.erase(oldestIt);
		--validCount;

		std::wstring oldestBackupPath = MakeBackupPathFromTimePoint(g_settings.backupRoot, entry.originalPath, oldestTimePoint);
		std::error_code removeError;
		std::fs::remove(std::fs::path(oldestBackupPath), removeError);
		RemoveFromTodayHistory(entry.originalPath, oldestTimePoint);
	}
}

static void EnforceGlobalSizeLimit(const std::fs::path& backupRootPath, uint32_t maxSizeMB)
{
	if (backupRootPath.empty())
	{
		return;
	}

	if (maxSizeMB == 0)
	{
		return;
	}

	uint64_t maxBytes = (uint64_t)maxSizeMB * 1024ull * 1024ull;
	uint64_t currentBytes = ComputeFolderSizeBytes(backupRootPath);

	if (currentBytes <= maxBytes)
	{
		return;
	}

	struct GlobalBackupItem
	{
		std::wstring originalPath;
		TimePoint timePoint;
	};

	std::vector<GlobalBackupItem> allBackups;

	{
		std::unique_lock<std::shared_mutex> indexLock(g_indexMutex);

		for (const BackupFile& entry : g_backupIndex)
		{
			for (const TimePoint& timePoint : entry.backups)
			{
				allBackups.push_back(GlobalBackupItem{ entry.originalPath, timePoint });
			}
		}
	}

	std::sort(allBackups.begin(), allBackups.end(), [](const GlobalBackupItem& left, const GlobalBackupItem& right)
	{
		return left.timePoint < right.timePoint;
	});


	size_t globalIndex = 0;

	while (currentBytes > maxBytes && globalIndex < allBackups.size())
	{
		std::error_code errorCode;
		uint64_t removedFileSize = 0;

		const std::wstring& originalPath = allBackups[globalIndex].originalPath;
		const TimePoint& timePoint = allBackups[globalIndex].timePoint;
		std::wstring backupPath = MakeBackupPathFromTimePoint(backupRootPath.wstring(), originalPath, timePoint);

		if (std::fs::exists(backupPath, errorCode))
		{
			removedFileSize = (uint64_t)std::fs::file_size(backupPath, errorCode);
		}

		std::fs::remove(backupPath, errorCode);

		{
			std::unique_lock<std::shared_mutex> indexLock(g_indexMutex);
			auto entryItr = g_backupIndex.end();
			for (entryItr = g_backupIndex.begin(); entryItr != g_backupIndex.end(); ++entryItr)
			{
				if (entryItr->originalPath == originalPath)
				{
					break;
				}
			}
			if (entryItr != g_backupIndex.end())
			{
				auto& backups = entryItr->backups;
				backups.erase(
					std::remove(backups.begin(), backups.end(), timePoint),
					backups.end());
			}
		}
		RemoveFromTodayHistory(originalPath, timePoint);

		if (removedFileSize > 0 && currentBytes >= removedFileSize)
		{
			currentBytes -= removedFileSize;
		}

		++globalIndex;
	}
}


static bool CopyToBackupAndIndex(const WatchedFolder& watchedFolder, const std::wstring& filePath)
{
	(void)watchedFolder;

	if (IsPaused())
	{
		return false;
	}

	if (g_settings.backupRoot.empty())
	{
		return false;
	}

	std::error_code errorCode;
	if (!std::fs::exists(filePath, errorCode) || !std::fs::is_regular_file(filePath, errorCode))
	{
		return false;
	}

	TimePoint backupTimePoint = std::chrono::system_clock::now();
	std::wstring destinationPath = MakeBackupPathFromTimePoint(g_settings.backupRoot, filePath, backupTimePoint);
	EnsureDirExists(std::fs::path(destinationPath).parent_path());

	BOOL copySucceeded = CopyFileW(filePath.c_str(), destinationPath.c_str(), FALSE);
	if (!copySucceeded)
	{
		return false;
	}

	{
		std::unique_lock<std::shared_mutex> lock(g_indexMutex);

		BackupFile& entry = GetOrCreateBackupEntry_Locked(filePath);
		entry.backups.push_back(backupTimePoint);
		entry.SortBackupTimes();
		EnforcePerFileLimit_Locked(entry, g_settings.maxBackupsPerFile);
	}

	if (destinationPath.find(g_todayPrefix) != std::wstring::npos)
	{
		InsertTodayHistory(filePath, backupTimePoint);
		++g_backupsToday;
		TrayUpdateBackupCount(g_backupsToday);
	}

	EnforceGlobalSizeLimit(std::fs::path(g_settings.backupRoot), g_settings.maxBackupSizeMB);
	return true;
}

static void ScanBackupFolder()
{
	{
		std::unique_lock<std::shared_mutex> lock(g_indexMutex);
		g_backupIndex.clear();
	}

	if (g_settings.backupRoot.empty())
	{
		return;
	}

	std::fs::path backupRootPath(g_settings.backupRoot);
	std::error_code errorCode;

	if (!std::fs::exists(backupRootPath, errorCode))
	{
		return;
	}
	
	g_backupsToday = 0;

	for (auto iterator = std::fs::recursive_directory_iterator(backupRootPath, std::fs::directory_options::skip_permission_denied, errorCode); iterator != std::fs::recursive_directory_iterator(); ++iterator)
	{
		if (errorCode)
		{
			errorCode.clear();
			continue;
		}

		if (!iterator->is_regular_file(errorCode))
		{
			continue;
		}

		std::fs::path backupFilePath = iterator->path();
		std::wstring backupStem = backupFilePath.stem().wstring();

		size_t backupMarkerPos = backupStem.rfind(L"_backup_");
		if (backupMarkerPos == std::wstring::npos)
		{
			continue;
		}

		std::wstring originalStem = backupStem.substr(0, backupMarkerPos);
		std::wstring originalExt = backupFilePath.extension().wstring();

		if (backupStem.find(g_todayPrefix) != std::wstring::npos)
		{
			g_backupsToday += 1;
		}

		std::fs::path relativeDir = std::fs::relative( backupFilePath.parent_path(), backupRootPath, errorCode);

		if (errorCode)
		{
			errorCode.clear();
			continue;
		}

		std::fs::path originalRelativePath = relativeDir / std::fs::path(originalStem + originalExt);
		
		std::wstring originalFullPath = UnsanitizePathFromBackupLayout(originalRelativePath.wstring());

		TimePoint timePoint;
		if (!TryParseBackupTimestampToTimePoint(backupStem, timePoint))
		{
			continue;
		}

		{
			std::unique_lock<std::shared_mutex> lock(g_indexMutex);
			BackupFile& entry = GetOrCreateBackupEntry_Locked(originalFullPath);
			entry.backups.push_back(timePoint);
		}
	}

	{
		std::unique_lock<std::shared_mutex> lock(g_indexMutex);

		for (BackupFile& entry : g_backupIndex)
		{
			entry.SortBackupTimes();
			EnforcePerFileLimit_Locked(entry, g_settings.maxBackupsPerFile);
		}
	}

	EnforceGlobalSizeLimit(std::fs::path(g_settings.backupRoot), g_settings.maxBackupSizeMB);
	RebuildTodayHistory();

	TrayUpdateBackupCount(g_backupsToday);
}

static bool SkipBackup(FolderWatcher& folderWatcher, const std::wstring& filePath, uint64_t nowTick)
{
	std::lock_guard<std::mutex> lock(folderWatcher.debounceMutex);

	auto foundTick = folderWatcher.lastEventTickByPath.find(filePath);
	if (foundTick != folderWatcher.lastEventTickByPath.end())
	{
		if (nowTick - foundTick->second < 500)
		{
			foundTick->second = nowTick;
			return true;
		}
	}

	folderWatcher.lastEventTickByPath[filePath] = nowTick;
	return false;
}

static void WatchThreadProc(FolderWatcher* watcher)
{
	const WatchedFolder watchedFolder = watcher->config;

	watcher->directoryHandle = CreateFileW(
		watchedFolder.path.c_str(),
		FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS,
		nullptr);

	if (watcher->directoryHandle == INVALID_HANDLE_VALUE)
	{
		return;
	}

	DWORD notifyFlags = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
	std::vector<uint8_t> notifyBuffer;
	notifyBuffer.resize(64 * 1024);

	while (!watcher->stopRequested.load())
	{
		DWORD bytesReturned = 0;

		BOOL readSucceeded = ReadDirectoryChangesW(
			watcher->directoryHandle,
			notifyBuffer.data(),
			(DWORD)notifyBuffer.size(),
			watchedFolder.includeSubfolders ? TRUE : FALSE,
			notifyFlags,
			&bytesReturned,
			nullptr,
			nullptr);

		if (!readSucceeded)
		{
			if (!watcher->stopRequested.load())
			{
			}

			break;
		}

		uint64_t nowTick = GetTickCount64();

		FILE_NOTIFY_INFORMATION* notifyInfo = (FILE_NOTIFY_INFORMATION*)notifyBuffer.data();
		while (true)
		{
			std::wstring relativePath(notifyInfo->FileName, notifyInfo->FileNameLength / sizeof(wchar_t));
			std::wstring fullPath = (std::fs::path(watchedFolder.path) / std::fs::path(relativePath)).wstring();

			bool isInteresting =
				notifyInfo->Action == FILE_ACTION_ADDED ||
				notifyInfo->Action == FILE_ACTION_MODIFIED ||
				notifyInfo->Action == FILE_ACTION_RENAMED_NEW_NAME;

			if (isInteresting)
			{
				// Exclude anything inside backup root
				if (!IsPathUnderRoot(fullPath, g_settings.backupRoot))
				{
					if (PassesFilters(watchedFolder, fullPath))
					{
						if (!SkipBackup(*watcher, fullPath, nowTick))
						{
							CopyToBackupAndIndex(watchedFolder, fullPath);
						}
					}
				}
			}

			if (notifyInfo->NextEntryOffset == 0)
			{
				break;
			}

			notifyInfo = (FILE_NOTIFY_INFORMATION*)((uint8_t*)notifyInfo + notifyInfo->NextEntryOffset);
		}
	}

	CloseHandle(watcher->directoryHandle);
}

static void StopWatchers()
{
	std::lock_guard<std::mutex> lock(g_watchersMutex);

	for (std::unique_ptr<FolderWatcher>& watcher : g_watchers)
	{
		watcher->stopRequested.store(true);
		CancelIoEx(watcher->directoryHandle, nullptr);
	}

	for (auto& watcher : g_watchers)
	{
		if (watcher->workerThread.joinable())
		{
			watcher->workerThread.join();
		}
	}

	g_watchers.clear();
}

static void StartWatchersFromSettings()
{
	StopWatchers();

	std::lock_guard<std::mutex> lock(g_watchersMutex);

	for (const auto& watchedFolder : g_settings.watched)
	{
		auto folderWatcher = std::make_unique<FolderWatcher>();
		folderWatcher->config = watchedFolder;
		folderWatcher->stopRequested.store(false);
		folderWatcher->workerThread = std::thread(WatchThreadProc, folderWatcher.get());

		g_watchers.push_back(std::move(folderWatcher));
	}
}

void LaunchDiffTool(const std::wstring& diffToolPath, const std::wstring& backupFilePath, const std::wstring& originalFilePath)
{
	if (diffToolPath.empty())
	{
		return;
	}

	if (!FileExists(diffToolPath))
	{
		return;
	}

	if (backupFilePath.empty() || originalFilePath.empty())
	{
		return;
	}

	// Convention: diffTool.exe "<backup>" "<original>"
	std::wstring parameters = L"\"" + backupFilePath + L"\"" + L" " + L"\"" + originalFilePath + L"\"";

	HINSTANCE resultHandle = ShellExecuteW(
		nullptr,
		L"open",
		diffToolPath.c_str(),
		parameters.c_str(),
		nullptr,
		SW_SHOWNORMAL);

	if ((INT_PTR)resultHandle <= 32)
	{
		return;
	}
}

static void UI_WatchedFolders()
{
	ImGui::Dummy(ImVec2(0,4));
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0,4));

	static int selectedFolderIndex = -1;

	if (g_settings.watched.empty())
	{
		selectedFolderIndex = -1;
	}
	else if (selectedFolderIndex < 0 || selectedFolderIndex >= (int)g_settings.watched.size())
	{
		selectedFolderIndex = 0;
	}

	bool removeSelectedFolder = false;
	bool hasSelectedFolderRect = false;
	ImVec2 selectedFolderRectMin = {};
	ImVec2 selectedFolderRectMax = {};
	ImVec2 propertiesRectMin = {};
	ImVec2 propertiesRectMax = {};
	bool hasPropertiesRect = false;

	static float leftPaneWidth = 320.0f;
	const float splitterWidth = 6.0f;
	const float minLeftPaneWidth = 180.0f;
	const float minRightPaneWidth = 320.0f;

	float totalWidth = ImGui::GetContentRegionAvail().x;
	float maxLeftPaneWidth = totalWidth - splitterWidth - minRightPaneWidth;
	if (maxLeftPaneWidth < minLeftPaneWidth)
	{
		maxLeftPaneWidth = minLeftPaneWidth;
	}
	if (leftPaneWidth < minLeftPaneWidth)
	{
		leftPaneWidth = minLeftPaneWidth;
	}
	if (leftPaneWidth > maxLeftPaneWidth)
	{
		leftPaneWidth = maxLeftPaneWidth;
	}

	if (ImGui::BeginChild("watched_folders_list", ImVec2(leftPaneWidth, 0)))
	{
		for (int folderIndex = 0; folderIndex < (int)g_settings.watched.size(); ++folderIndex)
		{
			const std::wstring& watchedPath = g_settings.watched[folderIndex].path;
			std::string watchedPathUtf8 = WToUTF8(watchedPath);
			
			bool isSelected = (folderIndex == selectedFolderIndex);

			ImGui::PushID(folderIndex);

			if (ImGui::BeginChild("folder_outer", ImVec2(0.0f, 60.0f)))
			{
				ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(8,8));

				if (ImGui::BeginChild("folder_inner", ImGui::GetContentRegionAvail() - ImVec2(0,8), ImGuiChildFlags_Borders))
				{
					ImVec2 innerMin = ImGui::GetWindowPos();
					ImVec2 innerSize = ImGui::GetWindowSize();
					ImVec2 innerMax(innerMin.x + innerSize.x, innerMin.y + innerSize.y);

					bool isHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
					if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
					{
						selectedFolderIndex = folderIndex;
						isSelected = true;
					}

					ImDrawList* innerDrawList = ImGui::GetWindowDrawList();
					if (isSelected)
					{
						innerDrawList->AddRectFilled(
							ImVec2(innerMin.x + 1.0f, innerMin.y + 1.0f),
							ImVec2(innerMax.x - 1.0f, innerMax.y - 1.0f),
							ImGui::GetColorU32(ImGuiCol_Header));
					}
					else if (isHovered)
					{
						innerDrawList->AddRectFilled(
							ImVec2(innerMin.x + 1.0f, innerMin.y + 1.0f),
							ImVec2(innerMax.x - 1.0f, innerMax.y - 1.0f),
							ImGui::GetColorU32(ImGuiCol_HeaderHovered));
					}

					const float textPaddingX = 10.0f;
					ImVec2 textSize = ImGui::CalcTextSize(watchedPathUtf8.c_str());
					float textY = innerMin.y + ((innerSize.y - textSize.y) * 0.5f);
					ImGui::SetCursorScreenPos(ImVec2(innerMin.x + textPaddingX, textY));
					ImGui::TextUnformatted(watchedPathUtf8.c_str());

					float visibleTextWidth = innerSize.x - (textPaddingX * 2.0f);
					bool isPathClipped = textSize.x > visibleTextWidth;
					if (isPathClipped && isHovered)
					{
						ImGui::SetTooltip("%s", watchedPathUtf8.c_str());
					}
				}
				ImGui::EndChild();
			}
			ImGui::EndChild();

			if (isSelected)
			{
				hasSelectedFolderRect = true;
				selectedFolderRectMin = ImGui::GetItemRectMin();
				selectedFolderRectMax = ImGui::GetItemRectMax();
			}

			ImGui::PopID();
		}

		ImGui::Dummy(ImVec2(10, 10));

		const float addButtonWidth = 150.0f;
		float addButtonX = ImGui::GetCursorPosX();
		float addButtonAvail = ImGui::GetContentRegionAvail().x;
		if (addButtonAvail > addButtonWidth)
		{
			addButtonX += (addButtonAvail - addButtonWidth) * 0.5f;
		}

		ImGui::SetCursorPosX(addButtonX);

		if (ImGui::Button("Add Folder", ImVec2(addButtonWidth, 30.0f)))
		{
			std::wstring selectedPath = BrowseForFolder(L"Select folder to watch");
			if (!selectedPath.empty())
			{
				WatchedFolder newWatchedFolder = {};
				newWatchedFolder.path = selectedPath;
				newWatchedFolder.includeSubfolders = true;

				g_settings.watched.push_back(newWatchedFolder);
				selectedFolderIndex = (int)g_settings.watched.size() - 1;

				MarkSettingsDirty();
				SaveSettings();
				StartWatchersFromSettings();
			}
		}
	}
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::InvisibleButton("watched_folders_splitter", ImVec2(splitterWidth, ImGui::GetContentRegionAvail().y));

	if (ImGui::IsItemActive())
	{
		leftPaneWidth += ImGui::GetIO().MouseDelta.x;

		if (leftPaneWidth < minLeftPaneWidth)
		{
			leftPaneWidth = minLeftPaneWidth;
		}

		if (leftPaneWidth > maxLeftPaneWidth)
		{
			leftPaneWidth = maxLeftPaneWidth;
		}
	}

	if (ImGui::IsItemHovered() || ImGui::IsItemActive())
	{
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
	}

	ImGui::SameLine();

	if (ImGui::BeginChild("properties", ImVec2(0, 0)))
	{
		ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(8,8));

		ImGui::PushID(selectedFolderIndex);

		if (ImGui::BeginChild("properties_inner", ImGui::GetContentRegionAvail() - ImVec2(8,8)))
		{
			if (selectedFolderIndex < 0 || selectedFolderIndex >= (int)g_settings.watched.size())
			{
				ImGui::TextDisabled("No watched folder selected.");
			}
			else
			{
				WatchedFolder& watchedFolder = g_settings.watched[selectedFolderIndex];

				if (ImGui::BeginTable("watched_folder_props_grid", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit))
				{
					ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 170.0f);
					ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);

					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted("Path");
					ImGui::TableNextColumn();
					std::string watchedPathUtf8 = WToUTF8(watchedFolder.path);
					ImGui::TextClickable("%s", watchedPathUtf8.c_str());
					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						OpenExplorerSelectPath(watchedFolder.path);
					}
					if (ImGui::BeginPopupContextItem("watched_path_context"))
					{
						if (ImGui::MenuItem("Show in Explorer"))
						{
							OpenExplorerSelectPath(watchedFolder.path);
						}
						ImGui::EndPopup();
					}

					ImGui::SameLine();

					if (ImGui::Button("..."))
					{
						std::wstring selectedPath = BrowseForFolder(L"Select folder to watch");
						if (!selectedPath.empty())
						{
							watchedFolder.path = selectedPath;
							MarkSettingsDirty();
							SaveSettings();
							StartWatchersFromSettings();
						}
					}

					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted("Include sub-folders");
					ImGui::TableNextColumn();
					if (ImGui::Checkbox("##include_subfolders", &watchedFolder.includeSubfolders))
					{
						MarkSettingsDirty();
					}

					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted("Include filters");

					ImGui::SameLine();
					ImGui::HelpTooltip("Examples: .png, png, *.tmp, foo*, *bar*");

					ImGui::TableNextColumn();
					{
						ImGui::SetNextItemWidth(-1.0f);
						if (ImGui::InputTextMultilineStdString("##include_filters", watchedFolder.includeFiltersCSV, ImVec2(-1.0f, 60.0f), ImGuiInputTextFlags_WordWrap))
						{
							MarkSettingsDirty();
						}
					}

					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted("Exclude filters");

					ImGui::SameLine();
					ImGui::HelpTooltip("Examples: .tmp, *autosave*, \\\\.*");

					ImGui::TableNextColumn();
					{
						ImGui::SetNextItemWidth(-1.0f);
						if (ImGui::InputTextMultilineStdString("##exclude_filters", watchedFolder.excludeFiltersCSV, ImVec2(-1.0f, 60.0f), ImGuiInputTextFlags_WordWrap))
						{
							MarkSettingsDirty();
						}
					}

					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted("Actions");
					ImGui::TableNextColumn();
					if (ImGui::Button("Apply"))
					{
						MarkSettingsDirty();
						SaveSettings();
						StartWatchersFromSettings();
					}
					ImGui::SameLine();
					if (ImGui::Button("Remove Folder"))
					{
						removeSelectedFolder = true;
					}

					ImGui::EndTable();
				}
			}
		}
		ImGui::EndChild();
	
		ImGui::PopID();
	}
	ImGui::EndChild();

	hasPropertiesRect = true;
	propertiesRectMin = ImGui::GetItemRectMin();
	propertiesRectMax = ImGui::GetItemRectMax();

	if (hasPropertiesRect)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImU32 borderColor = ImGui::GetColorU32(ImGuiCol_Tab) | 0xFF000000;
		float borderThickness = 5.0f;

		if (hasSelectedFolderRect)
		{
			//
			//                       2                        3
			//                       .------------------------.
			// 0                    1|                        |
			// .---------------------.                        |
			// |                                              |
			// .---------------------.                        |
			// 7                   6 |                        |
			//                       |                        |
			//                     5 .------------------------. 4
			//

			ImVec2 p0 = ImVec2(selectedFolderRectMin.x, selectedFolderRectMin.y);
			ImVec2 p1 = ImVec2(propertiesRectMin.x, selectedFolderRectMin.y);
			ImVec2 p2 = ImVec2(propertiesRectMin.x, propertiesRectMin.y);
			ImVec2 p3 = ImVec2(propertiesRectMax.x, propertiesRectMin.y);
			ImVec2 p4 = ImVec2(propertiesRectMax.x, propertiesRectMax.y);
			ImVec2 p5 = ImVec2(propertiesRectMin.x, propertiesRectMax.y);
			ImVec2 p6 = ImVec2(propertiesRectMin.x, selectedFolderRectMax.y);
			ImVec2 p7 = ImVec2(selectedFolderRectMin.x, selectedFolderRectMax.y);

			if (p1.y == p2.y)
			{
				ImVec2 outlinePoints[] = { p0, p3, p4, p5, p6, p7 };
				drawList->AddPolyline(outlinePoints, IM_ARRAYSIZE(outlinePoints), borderColor, ImDrawFlags_RoundCornersAll | ImDrawFlags_Closed, borderThickness);
			}
			else if (p6.y == p5.y)
			{
				ImVec2 outlinePoints[] = { p0, p1, p2, p3, p4, p7 };
				drawList->AddPolyline(outlinePoints, IM_ARRAYSIZE(outlinePoints), borderColor, ImDrawFlags_RoundCornersAll | ImDrawFlags_Closed, borderThickness);
			}
			else
			{
				ImVec2 outlinePoints[] = { p0, p1, p2, p3, p4, p5, p6, p7 };
				drawList->AddPolyline(outlinePoints, IM_ARRAYSIZE(outlinePoints), borderColor, ImDrawFlags_RoundCornersAll | ImDrawFlags_Closed, borderThickness);
			}
		}
		else
		{
			ImVec2 propertiesRectOutline[] =
			{
				ImVec2(propertiesRectMin.x, propertiesRectMin.y),
				ImVec2(propertiesRectMax.x, propertiesRectMin.y),
				ImVec2(propertiesRectMax.x, propertiesRectMax.y),
				ImVec2(propertiesRectMin.x, propertiesRectMax.y),
			};
			drawList->AddPolyline(propertiesRectOutline, IM_ARRAYSIZE(propertiesRectOutline), borderColor, ImDrawFlags_Closed, borderThickness);
		}
	}

	if (removeSelectedFolder && selectedFolderIndex >= 0 && selectedFolderIndex < (int)g_settings.watched.size())
	{
		g_settings.watched.erase(g_settings.watched.begin() + selectedFolderIndex);
		MarkSettingsDirty();
		SaveSettings();
		StartWatchersFromSettings();

		if (g_settings.watched.empty())
		{
			selectedFolderIndex = -1;
		}
		else if (selectedFolderIndex >= (int)g_settings.watched.size())
		{
			selectedFolderIndex = (int)g_settings.watched.size() - 1;
		}
	}
}

static bool HandleRowSelectAndHighlight(int rowIndex, int& selectedRowIndex, float rowMinY, float rowMaxY)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();

	if (!window)
	{
		return false;
	}

	ImVec2 windowPos = window->Pos;
	ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
	ImVec2 contentMax = ImGui::GetWindowContentRegionMax();

	ImVec2 rowMin(windowPos.x + contentMin.x, rowMinY);
	ImVec2 rowMax(windowPos.x + contentMax.x, rowMaxY);

	bool isHovered = ImGui::IsMouseHoveringRect(rowMin, rowMax, false);

	if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		selectedRowIndex = rowIndex;
		return true;
	}

	if (rowIndex == selectedRowIndex)
	{
		ImU32 bgColor = ImGui::GetColorU32(ImGuiCol_Header);
		ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bgColor);
	}
	else if (isHovered)
	{
		ImU32 hoverColor = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
		ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, hoverColor);
	}

	return false;
}

static void UI_BackedUpFiles()
{
	ImGui::Dummy(ImVec2(0,4));
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0,4));

	static std::wstring searchText;

	static std::set<std::wstring> selectedOriginalPaths;
	static std::wstring selectedBackupPath;
	static int lastClickIndex = -1;
	static int rangeSelectMinIndex = -1;
	static int rangeSelectMaxIndex = -1;
	static int lastSortColumn = -1;
	static ImGuiSortDirection lastSortDirection = ImGuiSortDirection_Ascending;
		
	static size_t pendingDeleteBackupCount = 0;


	std::list<BackupFile>::iterator currentSelectionItr = g_backupIndex.end();
	std::wstring latestBackupPath;

	static float leftPaneWidth = ImGui::GetContentRegionAvail().x * 0.5f;

	ImGui::TextUnformatted("Filter:");
	ImGui::SameLine();
	ImGui::InputTextStdString("##search", searchText);
	ImGui::SameLine();
	if (ImGui::Button("Clear", ImVec2(80, 0)))
	{
		searchText.clear();
	}

	ImGui::Dummy(ImVec2(0,4));

	bool isCtrlDown = ImGui::GetIO().KeyCtrl;
	bool isShiftDown = ImGui::GetIO().KeyShift;
	bool isDiffPressed = isCtrlDown && ImGui::IsKeyPressed(ImGuiKey_D, false);
	bool refreshRequested = ImGui::IsKeyPressed(ImGuiKey_F5, false);
	bool deleteRequested = ImGui::IsKeyPressed(ImGuiKey_Delete, false);
	bool deleteModalOpen = ImGui::IsPopupOpen("Delete Backups");
	if (deleteModalOpen)
	{
		isCtrlDown = false;
		isShiftDown = false;
		isDiffPressed = false;
		refreshRequested = false;
		deleteRequested = false;
	}

	const float splitterWidth = 6.0f;
	const float minLeftPaneWidth = 340.0f;
	const float minRightPaneWidth = 320.0f;
	float totalWidth = ImGui::GetContentRegionAvail().x;
	float paneHeight = ImGui::GetContentRegionAvail().y;

	float maxLeftPaneWidth = totalWidth - splitterWidth - minRightPaneWidth;
	if (maxLeftPaneWidth < minLeftPaneWidth)
	{
		maxLeftPaneWidth = minLeftPaneWidth;
	}
	if (leftPaneWidth < minLeftPaneWidth)
	{
		leftPaneWidth = minLeftPaneWidth;
	}
	if (leftPaneWidth > maxLeftPaneWidth)
	{
		leftPaneWidth = maxLeftPaneWidth;
	}

	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));

	auto SortBackupIndex_Locked = [&](int column, ImGuiSortDirection direction)
	{
		g_backupIndex.sort([&](const BackupFile& left, const BackupFile& right)
		{
			int compareResult = 0;

			switch (column)
			{
			case 0: // Path
				{
					const std::fs::path leftPath(left.originalPath);
					const std::fs::path rightPath(right.originalPath);
					const std::wstring leftDir = leftPath.parent_path().wstring();
					const std::wstring rightDir = rightPath.parent_path().wstring();
					compareResult = leftDir.compare(rightDir);
					if (compareResult == 0)
					{
						compareResult = leftPath.filename().wstring().compare(rightPath.filename().wstring());
					}
				}
				break;
			case 1: // Filename
				compareResult = std::fs::path(left.originalPath).filename().wstring().compare(
					std::fs::path(right.originalPath).filename().wstring());
				break;
			case 2: // #
				compareResult = (left.backups.size() < right.backups.size()) ? -1 : (left.backups.size() > right.backups.size() ? 1 : 0);
				break;
			case 3: // Latest Backup
				compareResult = (left.backups.back() < right.backups.back()) ? -1 : (left.backups.back() > right.backups.back() ? 1 : 0);
				break;
			default:
				break;
			}

			if (direction == ImGuiSortDirection_Descending)
			{
				compareResult = -compareResult;
			}

			if (compareResult == 0)
			{
				return left.originalPath < right.originalPath;
			}

			return compareResult < 0;
		});
	};

	{
		std::shared_lock<std::shared_mutex> indexLock(g_indexMutex);
	
		bool selectedIsVisible = false;
		bool hasVisibleEntries = false;
		std::list<BackupFile>::iterator firstVisibleItr = g_backupIndex.end();

		if (ImGui::BeginChild("backed_up_files_left", ImVec2(leftPaneWidth, paneHeight), false))
		{
			if (ImGui::BeginTable("backed_up_files_left_table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable))
			{
				ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 1.0f);
				ImGui::TableSetupColumn("Filename", ImGuiTableColumnFlags_WidthStretch, 0.6f);
				ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 60.0f);
				ImGui::TableSetupColumn("Latest Backup", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 170.0f);
				ImGui::TableHeadersRow();

				if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
				{
					if (sortSpecs->SpecsCount > 0)
					{
						const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
						lastSortColumn = spec.ColumnIndex;
						lastSortDirection = spec.SortDirection;

						if (sortSpecs->SpecsDirty)
						{
							SortBackupIndex_Locked(lastSortColumn, lastSortDirection);
							sortSpecs->SpecsDirty = false;
						}
					}
				}

				bool rangeSelectPending = rangeSelectMinIndex >=0 && rangeSelectMaxIndex >= 0;

				int currentIndex = 0;

				for (auto entryIt = g_backupIndex.begin(); entryIt != g_backupIndex.end(); ++entryIt, ++currentIndex)
				{
					const BackupFile& entry = *entryIt;

					if (entry.backups.empty())
					{
						continue;
					}

					if (!ContainsAllKeywords(entry.originalPath, searchText))
					{
						continue;
					}

					if (!hasVisibleEntries)
					{
						firstVisibleItr = entryIt;
						hasVisibleEntries = true;
					}

					if (selectedOriginalPaths.count(entry.originalPath))
					{
						currentSelectionItr = entryIt;
						selectedIsVisible = true;
					}

					if (rangeSelectPending)
					{
						if (currentIndex >= rangeSelectMinIndex && currentIndex <= rangeSelectMaxIndex)
						{
							selectedOriginalPaths.insert(entry.originalPath);
						}
					}

					std::fs::path originalFsPath(entry.originalPath);
					std::wstring originalNameWide = originalFsPath.filename().wstring();
					std::wstring originalFolderWide = originalFsPath.parent_path().wstring();
					std::string originalNameUtf8 = WToUTF8(originalNameWide);
					std::string originalFolderUtf8 = WToUTF8(originalFolderWide);
					std::string originalPathUtf8 = WToUTF8(entry.originalPath);

					ImGui::PushID(entry.originalPath.data());
					ImGui::TableNextRow();
					float rowMinY = ImGui::GetCursorScreenPos().y;

					ImGui::TableNextColumn();
					ImGui::TextClickable("%s", originalFolderUtf8.c_str());
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("%s", originalFolderUtf8.c_str());
					}
					if (!deleteModalOpen && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						OpenExplorerSelectPath(originalFolderWide);
					}
					if (!deleteModalOpen && ImGui::BeginPopupContextItem("folder_context"))
					{
						if (ImGui::MenuItem("Show in Explorer"))
						{
							OpenExplorerSelectPath(originalFolderWide);
						}
						ImGui::EndPopup();
					}

					ImGui::TableNextColumn();
					ImGui::TextClickable("%s", originalNameUtf8.c_str());
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("%s", originalPathUtf8.c_str());
					}

					if (!deleteModalOpen && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						selectedOriginalPaths.clear();
						selectedOriginalPaths.insert(entry.originalPath);
						selectedBackupPath.clear();
						currentSelectionItr = entryIt;
						selectedIsVisible = true;
						OpenFileWithShell(entry.originalPath);
					}
					if (!deleteModalOpen && ImGui::BeginPopupContextItem("original_context"))
					{
						if (ImGui::MenuItem("Show in Explorer"))
						{
							selectedOriginalPaths.clear();
							selectedOriginalPaths.insert(entry.originalPath);
							selectedBackupPath.clear();
							currentSelectionItr = entryIt;
							selectedIsVisible = true;
							OpenExplorerSelectPath(entry.originalPath);
						}
						ImGui::EndPopup();
					}

					ImGui::TableNextColumn();
					{
						ImGui::Text("%d", (int)entry.backups.size());
					}

					ImGui::TableNextColumn();
					{
						std::wstring latestTimestamp = FormatTimestampForDisplay(entry.backups.back());
						ImGui::TextUnformatted(WToUTF8(latestTimestamp).c_str());
					}

					float rowMaxY = ImGui::GetCursorScreenPos().y;
					ImGuiWindow* tableWindow = ImGui::GetCurrentWindow();
					if (tableWindow)
					{
						ImVec2 windowPos = tableWindow->Pos;
						ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
						ImVec2 contentMax = ImGui::GetWindowContentRegionMax();
						ImVec2 rowMin(windowPos.x + contentMin.x, rowMinY);
						ImVec2 rowMax(windowPos.x + contentMax.x, rowMaxY);

						bool isHovered = !deleteModalOpen && ImGui::IsMouseHoveringRect(rowMin, rowMax, false);
						bool isSelected = (selectedOriginalPaths.count(entry.originalPath));
						if (!deleteModalOpen && isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
						{
							if (isCtrlDown)
							{
								if (selectedOriginalPaths.count(entry.originalPath))
								{
									selectedOriginalPaths.erase(entry.originalPath);
								}
								else
								{
									selectedOriginalPaths.insert(entry.originalPath);
								}
								lastClickIndex = currentIndex;
							}
							else if (isShiftDown && lastClickIndex >= 0)
							{
								rangeSelectMinIndex = std::min(lastClickIndex, currentIndex);
								rangeSelectMaxIndex = std::max(lastClickIndex, currentIndex);
								lastClickIndex = currentIndex;
							}
							else
							{
								lastClickIndex = currentIndex;
								selectedOriginalPaths.clear();
								selectedOriginalPaths.insert(entry.originalPath);
							}

							selectedBackupPath.clear();
							currentSelectionItr = entryIt;
							selectedIsVisible = true;
							isSelected = true;
						}

						if (isSelected)
						{
							ImU32 bgColor = ImGui::GetColorU32(ImGuiCol_Header);
							ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bgColor);
						}
						else if (isHovered)
						{
							ImU32 hoverColor = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
							ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, hoverColor);
						}
					}

					ImGui::PopID();
				}

				if (rangeSelectPending)
				{
					rangeSelectMinIndex = -1;
					rangeSelectMaxIndex = -1;
				}

				ImGui::EndTable();
			}
		}
		ImGui::EndChild();

		if (!hasVisibleEntries)
		{
			selectedOriginalPaths.clear();
			selectedBackupPath.clear();
			currentSelectionItr = g_backupIndex.end();
		}
		else if (!selectedIsVisible)
		{
			currentSelectionItr = firstVisibleItr;
			selectedOriginalPaths.clear();
			selectedOriginalPaths.insert( currentSelectionItr->originalPath );
			selectedBackupPath.clear();
		}

		ImGui::SameLine(0.0f, 0.0f);
		ImGui::InvisibleButton("backed_up_files_splitter", ImVec2(splitterWidth, paneHeight));
		if (ImGui::IsItemActive())
		{
			leftPaneWidth += ImGui::GetIO().MouseDelta.x;
			if (leftPaneWidth < minLeftPaneWidth)
			{
				leftPaneWidth = minLeftPaneWidth;
			}
			if (leftPaneWidth > maxLeftPaneWidth)
			{
				leftPaneWidth = maxLeftPaneWidth;
			}
		}
		if (ImGui::IsItemHovered() || ImGui::IsItemActive())
		{
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
		}

		ImGui::SameLine(0.0f, 0.0f);
		float rightPaneWidth = totalWidth - leftPaneWidth - splitterWidth;
		if (rightPaneWidth < minRightPaneWidth)
		{
			rightPaneWidth = minRightPaneWidth;
		}

		if (ImGui::BeginChild("backed_up_files_right", ImVec2(rightPaneWidth, paneHeight), false))
		{
			if (selectedOriginalPaths.empty())
			{
				ImGui::TextDisabled("No backed up file selected.");
			}
			else if (selectedOriginalPaths.size() > 1)
			{
				ImGui::TextDisabled("Multiple entries selected.");
			}
			else
			{
				if (currentSelectionItr == g_backupIndex.end())
				{
					ImGui::TextDisabled("No backups available for selected file.");
				}
				else
				{
					const BackupFile& selectedEntry = *currentSelectionItr;

					if (selectedEntry.backups.empty())
					{
						ImGui::TextDisabled("No backups available for selected file.");
					}

					if (!selectedEntry.backups.empty())
					{
						latestBackupPath = MakeBackupPathFromTimePoint(g_settings.backupRoot, selectedEntry.originalPath, selectedEntry.backups.back());

						if (ImGui::BeginTable("selected_file_backups_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY))
						{
							ImGui::TableSetupColumn("Date & Time", ImGuiTableColumnFlags_WidthFixed, 190.0f);
							ImGui::TableSetupColumn("Backup File", ImGuiTableColumnFlags_WidthStretch, 1.0f);
							ImGui::TableSetupColumn("##Actions", ImGuiTableColumnFlags_WidthFixed, 250.0f);
							ImGui::TableHeadersRow();

							for (int backupIndex = (int)selectedEntry.backups.size() - 1; backupIndex >= 0; --backupIndex)
							{
								const TimePoint& backupTimePoint = selectedEntry.backups[backupIndex];
								std::wstring backupPath = MakeBackupPathFromTimePoint(g_settings.backupRoot, selectedEntry.originalPath, backupTimePoint);
								std::wstring backupFileName = std::fs::path(backupPath).filename().wstring();
								std::string backupFileNameUtf8 = WToUTF8(backupFileName);

								ImGui::PushID(backupIndex);

								ImGui::TableNextRow();
								float backupRowMinY = ImGui::GetCursorScreenPos().y;

								ImGui::TableNextColumn();
								{
									std::wstring timestamp = FormatTimestampForDisplay(backupTimePoint);
						
									ImGui::TextUnformatted(WToUTF8(timestamp).c_str());
								}

								ImGui::TableNextColumn();
								{
									ImGui::TextClickable("%s", backupFileNameUtf8.c_str());

									if (ImGui::IsItemHovered())
									{
										ImGui::SetTooltip("%s", WToUTF8(backupPath).c_str());
									}

									if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
									{
										OpenFileWithShell(backupPath);
									}
								}

								ImGui::TableNextColumn();
								{
									bool hasPreviousBackup = (backupIndex > 0);
									if (!hasPreviousBackup)
									{
										ImGui::BeginDisabled();
									}
									if (ImGui::Button("Diff Previous"))
									{
										const TimePoint& previousTimePoint = selectedEntry.backups[backupIndex - 1];
										std::wstring previousBackupPath = MakeBackupPathFromTimePoint(g_settings.backupRoot, selectedEntry.originalPath, previousTimePoint);
										LaunchDiffTool(g_settings.diffToolPath, previousBackupPath, backupPath);
									}
									if (!hasPreviousBackup)
									{
										ImGui::EndDisabled();
									}

									ImGui::SameLine();
									if (ImGui::Button("Diff Current"))
									{
										LaunchDiffTool(g_settings.diffToolPath, backupPath, selectedEntry.originalPath);
									}
								}

								float backupRowMaxY = ImGui::GetCursorScreenPos().y;
								ImGuiWindow* backupsTableWindow = ImGui::GetCurrentWindow();
								if (backupsTableWindow)
								{
									ImVec2 windowPos = backupsTableWindow->Pos;
									ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
									ImVec2 contentMax = ImGui::GetWindowContentRegionMax();
									ImVec2 rowMin(windowPos.x + contentMin.x, backupRowMinY);
									ImVec2 rowMax(windowPos.x + contentMax.x, backupRowMaxY);

									bool contextMenuShowing = ImGui::IsPopupOpen((ImGuiID)0, ImGuiPopupFlags_AnyPopupId);
									bool isHovered = ImGui::IsMouseHoveringRect(rowMin, rowMax, false) && !contextMenuShowing;
									bool isSelected = (selectedBackupPath == backupPath);

									if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
									{
										selectedBackupPath = backupPath;
										isSelected = true;
									}
									else if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
									{
										selectedBackupPath = backupPath;
										isSelected = true;

										ImGui::OpenPopup("backup_context");
									}

									if (isSelected)
									{
										ImU32 bgColor = ImGui::GetColorU32(ImGuiCol_Header);
										ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bgColor);
									}
									else if (isHovered)
									{
										ImU32 hoverColor = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
										ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, hoverColor);
									}

									if (ImGui::BeginPopup("backup_context"))
									{
										selectedBackupPath = backupPath;

										if (ImGui::MenuItem("Open"))
										{
											OpenFileWithShell(backupPath);
										}

										if (backupIndex > 0)
										{
											if (ImGui::MenuItem("Diff Previous"))
											{
												const TimePoint& previousTimePoint = selectedEntry.backups[backupIndex - 1];
												std::wstring previousBackupPath = MakeBackupPathFromTimePoint(g_settings.backupRoot, selectedEntry.originalPath, previousTimePoint);
												LaunchDiffTool(g_settings.diffToolPath, previousBackupPath, backupPath);
											}
										}

										if (ImGui::MenuItem("Diff Current"))
										{
											LaunchDiffTool(g_settings.diffToolPath, backupPath, selectedEntry.originalPath);
										}

										if (ImGui::MenuItem("Show in Explorer"))
										{
											OpenExplorerSelectPath(backupPath);
										}

										ImGui::EndPopup();
									}
								}

								ImGui::PopID();
							}

							ImGui::EndTable();
						}
					}
				}
			}
		}
		ImGui::EndChild();

		ImGui::PopStyleVar();
	}

	if (isDiffPressed)
	{
		if (selectedBackupPath.empty())
			selectedBackupPath = latestBackupPath;

		if (currentSelectionItr != g_backupIndex.end())
		{
			const BackupFile& selectedEntry = *currentSelectionItr;

			if (!selectedEntry.originalPath.empty() && !selectedBackupPath.empty())
			{
				bool hasPrevious = false;
				std::wstring previousBackupPath;
				for (size_t i = 0; i < selectedEntry.backups.size(); ++i)
				{
					if (MakeBackupPathFromTimePoint(g_settings.backupRoot, selectedEntry.originalPath, selectedEntry.backups[i]) == selectedBackupPath)
					{
						if (i > 0)
						{
							hasPrevious = true;
							previousBackupPath = MakeBackupPathFromTimePoint(g_settings.backupRoot, selectedEntry.originalPath, selectedEntry.backups[i - 1]);
						}
						break;
					}
				}

				if (hasPrevious)
				{
					LaunchDiffTool(g_settings.diffToolPath, previousBackupPath, selectedBackupPath);
				}
			}
		}
	}

	if (refreshRequested)
	{
		ScanBackupFolder();

		if (lastSortColumn >= 0)
		{
			std::unique_lock<std::shared_mutex> sortLock(g_indexMutex);
			SortBackupIndex_Locked(lastSortColumn, lastSortDirection);
		}
	}

	if (deleteRequested)
	{
		pendingDeleteBackupCount = 0;

		if (!selectedOriginalPaths.empty())
		{
			for (auto entryItr = g_backupIndex.begin(); entryItr != g_backupIndex.end(); ++entryItr)
			{
				if (selectedOriginalPaths.count(entryItr->originalPath))
				{
					pendingDeleteBackupCount += (*entryItr).backups.size(); 
				}
			}

			if (pendingDeleteBackupCount > 0)
			{
				ImGui::OpenPopup("Delete Backups");
			}
		}
	}

	if (ImGui::BeginPopupModal("Delete Backups", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			pendingDeleteBackupCount = 0;
			ImGui::CloseCurrentPopup();
		}

		ImGui::Text("Delete %zu backup files for %zu originals?", pendingDeleteBackupCount, selectedOriginalPaths.size());
		ImGui::Separator();

		if (ImGui::BeginChild("delete_list", ImVec2(720.0f, 220.0f), true))
		{
			for (const auto& path : selectedOriginalPaths)
			{
				std::wstring wildcardPath = MakeBackupWildcardPath(g_settings.backupRoot, path);

				ImGui::TextUnformatted(WToUTF8(wildcardPath).c_str());
			}
		}
		ImGui::EndChild();

		if (ImGui::Button("Delete", ImVec2(120, 0)))
		{
			std::unique_lock<std::shared_mutex> indexLock(g_indexMutex);

			for (auto entryItr = g_backupIndex.begin(); entryItr != g_backupIndex.end(); )
			{
				if (selectedOriginalPaths.count(entryItr->originalPath))
				{
					for (const TimePoint& timePoint : entryItr->backups)
					{
						std::wstring backupPath = MakeBackupPathFromTimePoint(g_settings.backupRoot, (*entryItr).originalPath, timePoint);
						std::error_code errorCode;
						std::fs::remove(backupPath, errorCode);
						RemoveFromTodayHistory((*entryItr).originalPath, timePoint);
					}

					entryItr = g_backupIndex.erase(entryItr);
				}
				else
				{
					++entryItr;
				}
			}

			pendingDeleteBackupCount = 0;
			selectedBackupPath.clear();
			selectedOriginalPaths.clear();
			currentSelectionItr = g_backupIndex.end();
			lastClickIndex = -1;
			rangeSelectMinIndex = -1;
			rangeSelectMaxIndex = -1;

			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0)))
		{
			pendingDeleteBackupCount = 0;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}

static void UI_History()
{
	ImGui::Dummy(ImVec2(0,4));
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0,4));

	static int selectedOperationIndex = -1;
	static std::set<int> selectedOperationIndices;
	static int lastHistoryClickIndex = -1;
	static size_t pendingDeleteCount = 0;

	ImGui::Text("Today's operations");

	ImGui::Separator();

	bool isCtrlDown = ImGui::GetIO().KeyCtrl;
	bool isDiffPressed = isCtrlDown && ImGui::IsKeyPressed(ImGuiKey_D, false);
	bool deleteRequested = ImGui::IsKeyPressed(ImGuiKey_Delete, false);

	HistoryEntry selectedOperationCopy = {};
	bool hasSelectedOperation = false;

	{
		std::lock_guard<std::mutex> lock(g_historyMutex);

		if (selectedOperationIndex < 0 || selectedOperationIndex >= (int)g_todayHistory.size())
		{
			selectedOperationIndex = -1;
		}

		if (!selectedOperationIndices.empty())
		{
			for (auto it = selectedOperationIndices.begin(); it != selectedOperationIndices.end(); )
			{
				if (*it < 0 || *it >= (int)g_todayHistory.size())
				{
					it = selectedOperationIndices.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));

		if (ImGui::BeginTable("ops", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
		{
			ImGui::TableSetupColumn("Path");
			ImGui::TableSetupColumn("Backup Time", ImGuiTableColumnFlags_WidthFixed, 160.0f);
			ImGui::TableSetupColumn("Backup Path");
			ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 260.0f);
			ImGui::TableHeadersRow();

			for (int operationIndex = 0; operationIndex < (int)g_todayHistory.size(); ++operationIndex)
			{
				const HistoryEntry& backupOperation = g_todayHistory[operationIndex];

				ImGui::PushID(operationIndex);

				ImGui::TableNextRow();

				float rowMinY = ImGui::GetCursorScreenPos().y;

				ImGui::TableNextColumn();
				{
					std::string originalUtf8 = WToUTF8(backupOperation.originalPath);

					ImGui::TextClickable("%s", originalUtf8.c_str());
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("%s", originalUtf8.c_str());
					}

					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						selectedOperationIndex = operationIndex;
						if (!backupOperation.originalPath.empty())
						{
							OpenFileWithShell(backupOperation.originalPath);
						}
					}

					if (ImGui::BeginPopupContextItem("original_context"))
					{
						if (ImGui::MenuItem("Show in Explorer"))
						{
							selectedOperationIndex = operationIndex;
							OpenExplorerSelectPath(backupOperation.originalPath);
						}
						ImGui::EndPopup();
					}
				}

				ImGui::TableNextColumn();
				std::wstring formattedTimeStamp = FormatTimestampForDisplay(backupOperation.timePoint);
				ImGui::TextUnformatted(WToUTF8(formattedTimeStamp).c_str());

				ImGui::TableNextColumn();
				{
					std::string backupUtf8 = WToUTF8(backupOperation.backupPath);

					ImGui::TextClickable("%s", backupUtf8.c_str());
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("%s", backupUtf8.c_str());
					}

					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						selectedOperationIndex = operationIndex;
						if (!backupOperation.backupPath.empty())
						{
							OpenFileWithShell(backupOperation.backupPath);
						}
					}

					if (ImGui::BeginPopupContextItem("backup_context"))
					{
						if (ImGui::MenuItem("Show in Explorer"))
						{
							selectedOperationIndex = operationIndex;
							OpenExplorerSelectPath(backupOperation.backupPath);
						}
						ImGui::EndPopup();
					}
				}

			ImGui::TableNextColumn();
			{
				bool hasPrevious = false;
				std::wstring previousBackupPath;
				{
					std::shared_lock<std::shared_mutex> indexLock(g_indexMutex);
					auto entryItr = g_backupIndex.end();
					for (entryItr = g_backupIndex.begin(); entryItr != g_backupIndex.end(); ++entryItr)
					{
						if (entryItr->originalPath == backupOperation.originalPath)
						{
							break;
						}
					}
					if (entryItr != g_backupIndex.end())
					{
						const auto& backups = entryItr->backups;
						for (size_t i = 0; i < backups.size(); ++i)
						{
							if (backups[i] == backupOperation.timePoint)
							{
								if (i > 0)
								{
									hasPrevious = true;
									previousBackupPath = MakeBackupPathFromTimePoint(g_settings.backupRoot, backupOperation.originalPath, backups[i - 1]);
								}
								break;
							}
						}
					}
				}
				if (!hasPrevious)
				{
					ImGui::BeginDisabled();
				}
				if (ImGui::Button("Diff Previous"))
				{
					LaunchDiffTool(g_settings.diffToolPath, previousBackupPath, backupOperation.backupPath);
				}
				if (!hasPrevious)
				{
					ImGui::EndDisabled();
				}

				ImGui::SameLine();
				if (ImGui::Button("Diff Current"))
				{
					LaunchDiffTool(g_settings.diffToolPath, backupOperation.backupPath, backupOperation.originalPath);
				}
			}

				float rowMaxY = ImGui::GetCursorScreenPos().y;

				ImGuiWindow* tableWindow = ImGui::GetCurrentWindow();
				if (tableWindow)
				{
					ImVec2 windowPos = tableWindow->Pos;
					ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
					ImVec2 contentMax = ImGui::GetWindowContentRegionMax();
					ImVec2 rowMin(windowPos.x + contentMin.x, rowMinY);
					ImVec2 rowMax(windowPos.x + contentMax.x, rowMaxY);

					bool isHovered = ImGui::IsMouseHoveringRect(rowMin, rowMax, false);
					bool isSelected = (selectedOperationIndices.find(operationIndex) != selectedOperationIndices.end());

					if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
					{
						if (isDiffPressed)
						{
							isDiffPressed = false;
						}

						if (ImGui::GetIO().KeyShift && lastHistoryClickIndex >= 0)
						{
							int rangeStart = std::min(lastHistoryClickIndex, operationIndex);
							int rangeEnd = std::max(lastHistoryClickIndex, operationIndex);
							for (int idx = rangeStart; idx <= rangeEnd; ++idx)
							{
								selectedOperationIndices.insert(idx);
							}
						}
						else if (ImGui::GetIO().KeyCtrl)
						{
							selectedOperationIndices.insert(operationIndex);
						}
						else
						{
							selectedOperationIndices.clear();
							selectedOperationIndices.insert(operationIndex);
						}

						lastHistoryClickIndex = operationIndex;
						selectedOperationIndex = operationIndex;
						isSelected = (selectedOperationIndices.find(operationIndex) != selectedOperationIndices.end());
					}

					if (isSelected)
					{
						ImU32 bgColor = ImGui::GetColorU32(ImGuiCol_Header);
						ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bgColor);
					}
					else if (isHovered)
					{
						ImU32 hoverColor = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
						ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, hoverColor);
					}
				}

				if (operationIndex == selectedOperationIndex)
				{
					selectedOperationCopy = backupOperation;
					hasSelectedOperation = true;
				}

				ImGui::PopID();
			}

				ImGui::EndTable();
		}

		ImGui::PopStyleVar();
	}

	if (deleteRequested && !selectedOperationIndices.empty())
	{
		pendingDeleteCount = selectedOperationIndices.size();
		ImGui::OpenPopup("Delete Backups");
	}

	if (ImGui::BeginPopupModal("Delete Backups", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Delete %zu history entries?", pendingDeleteCount);
		ImGui::Separator();

		if (ImGui::Button("Delete", ImVec2(120, 0)))
		{
			std::vector<HistoryEntry> entriesToDelete;
			entriesToDelete.reserve(selectedOperationIndices.size());
			{
				std::lock_guard<std::mutex> lock(g_historyMutex);
				for (int idx : selectedOperationIndices)
				{
					if (idx >= 0 && idx < (int)g_todayHistory.size())
					{
						entriesToDelete.push_back(g_todayHistory[idx]);
					}
				}
			}

			{
				std::unique_lock<std::shared_mutex> indexLock(g_indexMutex);
				for (const auto& entry : entriesToDelete)
				{
					auto entryItr = g_backupIndex.end();
					for (entryItr = g_backupIndex.begin(); entryItr != g_backupIndex.end(); ++entryItr)
					{
						if (entryItr->originalPath == entry.originalPath)
						{
							break;
						}
					}
					if (entryItr != g_backupIndex.end())
					{
						auto& backups = entryItr->backups;
						backups.erase(
							std::remove(backups.begin(), backups.end(), entry.timePoint),
							backups.end());
					}
				}
			}

			for (const auto& entry : entriesToDelete)
			{
				std::error_code errorCode;
				std::fs::remove(entry.backupPath, errorCode);
				RemoveFromTodayHistory(entry.originalPath, entry.timePoint);
			}

			{
				std::lock_guard<std::mutex> lock(g_historyMutex);
				for (auto it = selectedOperationIndices.rbegin(); it != selectedOperationIndices.rend(); ++it)
				{
					int idx = *it;
					if (idx >= 0 && idx < (int)g_todayHistory.size())
					{
						g_todayHistory.erase(g_todayHistory.begin() + idx);
					}
				}
			}

			selectedOperationIndices.clear();
			selectedOperationIndex = -1;
			lastHistoryClickIndex = -1;
			pendingDeleteCount = 0;

			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0)))
		{
			pendingDeleteCount = 0;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	if (isDiffPressed && hasSelectedOperation)
	{
		bool hasPrevious = false;
		std::wstring previousBackupPath;
		{
			std::shared_lock<std::shared_mutex> indexLock(g_indexMutex);
			auto entryItr = g_backupIndex.end();
			for (entryItr = g_backupIndex.begin(); entryItr != g_backupIndex.end(); ++entryItr)
			{
				if (entryItr->originalPath == selectedOperationCopy.originalPath)
				{
					break;
				}
			}
			if (entryItr != g_backupIndex.end())
			{
				const auto& backups = entryItr->backups;
				for (size_t i = 0; i < backups.size(); ++i)
				{
					if (backups[i] == selectedOperationCopy.timePoint)
					{
						if (i > 0)
						{
							hasPrevious = true;
							previousBackupPath = MakeBackupPathFromTimePoint(g_settings.backupRoot, selectedOperationCopy.originalPath, backups[i - 1]);
						}
						break;
					}
				}
			}
		}

		if (hasPrevious)
		{
			LaunchDiffTool(g_settings.diffToolPath, previousBackupPath, selectedOperationCopy.backupPath);
		}
	}
}

static void UI_Settings()
{
	ImGui::Dummy(ImVec2(0,4));
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0,4));

	static std::string backupRootUtf8;
	static std::string diffToolUtf8;

	if (backupRootUtf8.empty())
	{
		backupRootUtf8 = WToUTF8(g_settings.backupRoot);
		if (backupRootUtf8.capacity() < 512)
		{
			backupRootUtf8.reserve(512);
		}
	}

	int maxFolderSizeMB = (int)g_settings.maxBackupSizeMB;
	int maxPerFileBackups = (int)g_settings.maxBackupsPerFile;
	if (diffToolUtf8.empty())
	{
		diffToolUtf8 = WToUTF8(g_settings.diffToolPath);
		if (diffToolUtf8.capacity() < 512)
		{
			diffToolUtf8.reserve(512);
		}
	}

	if (ImGui::BeginTable("settings_grid", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit))
	{
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted("Backup folder");

		ImGui::SameLine();

		ImGui::TableNextColumn();

		ImGui::SetNextItemWidth(400);

		if (ImGui::InputTextStdString("##backupRoot", backupRootUtf8))
		{
			g_settings.backupRoot = UTF8ToW(backupRootUtf8);
			MarkSettingsDirty();
		}

		ImGui::SameLine();

		if (ImGui::Button("..."))
		{
			std::wstring selectedPath = BrowseForFolder(L"Select backup folder");
			if (!selectedPath.empty())
			{
				g_settings.backupRoot = selectedPath;
				backupRootUtf8 = WToUTF8(g_settings.backupRoot);
				MarkSettingsDirty();
				SaveSettings();
				ScanBackupFolder();
			}
		}

		ImGui::SameLine();

		if (ImGui::Button("Explore"))
		{
			if (!g_settings.backupRoot.empty())
			{
				(void)ShellExecuteW(nullptr, L"open", g_settings.backupRoot.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		
		ImGui::TextUnformatted("Max backup folder size (MB)");
		
		ImGui::SameLine();

		ImGui::HelpTooltip("When exceeded, oldest backups across all files are deleted until within the limit.");
		
		ImGui::TableNextColumn();
		
		ImGui::SetNextItemWidth(240.0f);
		
		if (ImGui::InputInt("##maxsize", &maxFolderSizeMB))
		{
			if (maxFolderSizeMB < 1)
			{
				maxFolderSizeMB = 1;
			}
			g_settings.maxBackupSizeMB = (uint32_t)maxFolderSizeMB;
			MarkSettingsDirty();
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted("Max backups per file");
		ImGui::SameLine();
		ImGui::HelpTooltip("Per original file, keep at most this many backups. Oldest backups are deleted first.");
		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(240.0f);
		if (ImGui::InputInt("##maxperfile", &maxPerFileBackups))
		{
			if (maxPerFileBackups < 1)
			{
				maxPerFileBackups = 1;
			}
			g_settings.maxBackupsPerFile = (uint32_t)maxPerFileBackups;
			MarkSettingsDirty();
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted("Pause duration (minutes)");
		ImGui::SameLine();
		ImGui::HelpTooltip("Used by the 'Pause For N Minutes' button.");
		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(240.0f);
		int pauseMinutes = (int)g_settings.pauseMinutes;
		if (ImGui::InputInt("##pauseMinutes", &pauseMinutes))
		{
			if (pauseMinutes < 1)
			{
				pauseMinutes = 1;
			}
			g_settings.pauseMinutes = (uint32_t)pauseMinutes;
			MarkSettingsDirty();
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted("Diff tool executable");
		ImGui::SameLine();
		ImGui::HelpTooltip( "Used by Ctrl+D in Backup History.\n"
							"The tool is launched as:\n"
							"  <diffTool.exe> \"<backup>\" \"<original>\"\n"
							"Pick a diff tool that accepts two file arguments.");

		ImGui::TableNextColumn();

		ImGui::SetNextItemWidth(400);
		if (ImGui::InputTextStdString("##diffTool", diffToolUtf8))
		{
			g_settings.diffToolPath = UTF8ToW(diffToolUtf8);
			MarkSettingsDirty();
		}

		ImGui::SameLine();

		if (ImGui::Button("Browse Diff Tool"))
		{
			std::wstring selectedExePath = BrowseForExeFile();
			if (!selectedExePath.empty())
			{
				g_settings.diffToolPath = selectedExePath;
				diffToolUtf8 = WToUTF8(g_settings.diffToolPath);
				MarkSettingsDirty();
				SaveSettings();
			}
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted("Minimize to tray on close");
		ImGui::TableNextColumn();
		if (ImGui::Checkbox("##minimizeOnClose", &g_settings.minimizeOnClose))
		{
			MarkSettingsDirty();
		}

		ImGui::EndTable();


		if (ImGui::Button("Apply", ImVec2(80, 30)))
		{
			MarkSettingsDirty();
			SaveSettings();
			ScanBackupFolder();
			EnforceGlobalSizeLimit(std::fs::path(g_settings.backupRoot), g_settings.maxBackupSizeMB);
		}
	}
}

void AppInit()
{
	using namespace std::chrono;
	auto now = system_clock::now();
	auto tt = system_clock::to_time_t(now);
	tm tmv = {};
	localtime_s(&tmv, &tt);
	g_todayPrefix = BuildTodayPrefixFromTimePoint(now);

	ScanBackupFolder();
	StartWatchersFromSettings();
}

bool AppLoop()
{
	MaybeSaveSettingsThrottled();

	static uint64_t lastTodayPrefixCheck = 0;
	if ((GetTickCount64() - lastTodayPrefixCheck) >= 10000)
	{
		using namespace std::chrono;
		auto now = system_clock::now();
		auto tt = system_clock::to_time_t(now);
		tm tmv = {};
		localtime_s(&tmv, &tt);

		std::wstring todayPrefix = BuildTodayPrefixFromTimePoint(now);
		if (g_todayPrefix != todayPrefix)
		{
			g_todayPrefix = todayPrefix;
			ScanBackupFolder();
		}

		lastTodayPrefixCheck = GetTickCount64();
	}

	return false;
}

bool AppDraw()
{
	const float barHeight = 38.0f;
	const float buttonHeight = 30.0f;
	const float barSpacing = 6.0f;
	float contentHeight = ImGui::GetContentRegionAvail().y - barHeight - barSpacing;
	if (contentHeight < 0.0f)
	{
		contentHeight = 0.0f;
	}

	if (ImGui::BeginChild("main_content", ImVec2(0.0f, contentHeight), false))
	{
		if (ImGui::BeginTabBar("tabs"))
		{
			if (ImGui::BeginTabItem(" Watched Folders "))
			{
				UI_WatchedFolders();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(" Backed Up Files "))
			{
				UI_BackedUpFiles();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("       Log       "))
			{
				UI_History();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("    Settings     "))
			{
				UI_Settings();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::EndChild();

	ImGui::Spacing();
	ImGui::Separator();

	if (ImGui::BeginChild("pause_bar", ImVec2(0.0f, barHeight), false, ImGuiWindowFlags_NoScrollbar))
	{
		auto CenterNext = [&](float totalWidth)
		{
			float startX = (ImGui::GetContentRegionAvail().x - totalWidth) * 0.5f;
			if (startX < 0.0f)
			{
				startX = 0.0f;
			}
			ImGui::SetCursorPosX(startX);
		};

		float centerY = (barHeight - buttonHeight) * 0.5f;
		if (centerY > 0.0f)
		{
			ImGui::SetCursorPosY(centerY);
		}

		if (!IsPaused())
		{
			const float buttonSpacing = ImGui::GetStyle().ItemSpacing.x;
			const float pauseWidth = 140.0f;
			const float pauseForWidth = 220.0f;
			const float totalWidth = pauseWidth + buttonSpacing + pauseForWidth;
			CenterNext(totalWidth);

			if (ImGui::Button("Pause", ImVec2(140, buttonHeight)))
			{
				g_pauseUntilTick.store(0, std::memory_order_relaxed);
				g_isPaused.store(true, std::memory_order_relaxed);
			}

			ImGui::SameLine();
			if (ImGui::Button(fmt::format("Pause For {} Minutes", g_settings.pauseMinutes).c_str(), ImVec2(220, buttonHeight)))
			{
				uint64_t durationMs = (uint64_t)g_settings.pauseMinutes * 60ull * 1000ull;
				g_pauseUntilTick.store(GetTickCount64() + durationMs, std::memory_order_relaxed);
				g_isPaused.store(true, std::memory_order_relaxed);
			}
		}
		else
		{
			const float buttonSpacing = ImGui::GetStyle().ItemSpacing.x;
			const float pauseWidth = 140.0f;
			const float pauseForWidth = 220.0f;
			const float totalWidth = pauseWidth + buttonSpacing + pauseForWidth;
			CenterNext(totalWidth);

			if (ImGui::Button("Resume", ImVec2(pauseWidth, buttonHeight)))
			{
				g_pauseUntilTick.store(0, std::memory_order_relaxed);
				g_isPaused.store(false, std::memory_order_relaxed);
			}

			ImGui::SameLine();
			if (ImGui::BeginChild("pause_resume_timer", ImVec2(pauseForWidth, buttonHeight), false, ImGuiWindowFlags_NoScrollbar))
			{
				uint64_t pauseUntil = g_pauseUntilTick.load(std::memory_order_relaxed);
				if (pauseUntil != 0)
				{
					uint64_t nowTick = GetTickCount64();
					uint64_t remainingMs = (pauseUntil > nowTick) ? (pauseUntil - nowTick) : 0;
					uint64_t remainingSeconds = remainingMs / 1000ull;
					uint64_t remainingMinutes = remainingSeconds / 60ull;
					uint64_t remainingSecondsOnly = remainingSeconds % 60ull;
					std::string message = fmt::format("Resuming in {:02d}:{:02d}", (int)remainingMinutes, (int)remainingSecondsOnly);
					ImVec2 textSize = ImGui::CalcTextSize(message.c_str());
					ImVec2 boxSize = ImGui::GetContentRegionAvail();
					float textX = (boxSize.x - textSize.x) * 0.5f;
					float textY = (boxSize.y - textSize.y) * 0.5f;
					if (textX < 0.0f)
					{
						textX = 0.0f;
					}
					if (textY < 0.0f)
					{
						textY = 0.0f;
					}
					ImGui::SetCursorPos(ImVec2(textX, textY));
					ImGui::TextUnformatted(message.c_str());
				}
			}
			ImGui::EndChild();
		}
	}
	ImGui::EndChild();

	return false;
}

void AppShutdown()
{
	StopWatchers();
}
