// main.cpp

#include "main.h"
#include "app.h"
#include "util.h"
#include "settings.h"
#include "imgui/imgui_internal.h"

struct BackupOperation
{
	std::wstring	timeStamp;
	std::wstring	originalPath;
	std::wstring	backupPath;
	std::wstring	result;
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

static std::mutex											g_operationsMutex;
static std::vector<BackupOperation>							g_operations;
static const uint32_t										kOperationHistoryMaxCount = 1024;

static std::shared_mutex									g_indexMutex;
static std::map<std::wstring, std::vector<std::wstring>>	g_backupIndex;

static std::mutex											g_watchersMutex;
static std::vector<std::unique_ptr<FolderWatcher>>			g_watchers;

static std::atomic<uint32_t>								g_backupsToday;
static std::wstring											g_todayPrefix;
		
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

static bool ParseBackupTimestamp(const std::wstring& backupFileName, std::wstring& outDate, std::wstring& outTime)
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

	wchar_t dateBuffer[64] = {};
	wchar_t timeBuffer[64] = {};

	static const wchar_t* monthNames[12] =
	{
		L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
		L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"
	};

	swprintf_s(
		dateBuffer,
		L"%02d %s %04d",
		day,
		monthNames[month - 1],
		year);

	swprintf_s(
		timeBuffer,
		L"%02d:%02d:%02d",
		hour,
		minute,
		second);

	outDate = dateBuffer;
	outTime = timeBuffer;

	return true;
}

static std::wstring FormatOperationTimestampForDisplay(const std::wstring& rawTimestamp)
{
	int year = 0, month = 0, day = 0;
	int hour = 0, minute = 0, second = 0;

	if (swscanf_s(
		rawTimestamp.c_str(),
		L"%4d_%2d_%2d__%2d_%2d_%2d",
		&year, &month, &day,
		&hour, &minute, &second) != 6)
	{
		return rawTimestamp;
	}

	if (month < 1 || month > 12)
	{
		return rawTimestamp;
	}

	static const wchar_t* monthNames[12] =
	{
		L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
		L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"
	};

	wchar_t buffer[64] = {};
	swprintf_s(
		buffer,
		L"%02d %s %04d %02d:%02d:%02d",
		day,
		monthNames[month - 1],
		year,
		hour,
		minute,
		second);

	return buffer;
}

static void PushOperation(const BackupOperation& backupOperation)
{
	std::lock_guard<std::mutex> lock(g_operationsMutex);

	g_operations.push_back(backupOperation);

	if (g_operations.size() > kOperationHistoryMaxCount)
	{
		g_operations.erase(g_operations.begin(), g_operations.begin() + (g_operations.size() - kOperationHistoryMaxCount));
	}

	if (backupOperation.result == L"OK")
	{
		using namespace std::chrono;
		auto now = system_clock::now();
		auto tt = system_clock::to_time_t(now);
		tm tmv = {};
		localtime_s(&tmv, &tt);

		wchar_t todayPrefix[64] = {};
		swprintf_s(todayPrefix, L"_backup_%04d_%02d_%02d__", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
		if (g_todayPrefix != todayPrefix)
		{
			g_todayPrefix = todayPrefix;
			g_backupsToday = 0;
		}

		if (backupOperation.backupPath.find(g_todayPrefix) != std::wstring::npos)
		{
			++g_backupsToday;

			TrayUpdateBackupCount(g_backupsToday);
		}
	}
}

static uint64_t FileTimeToU64(const FILETIME& fileTime)
{
	ULARGE_INTEGER fileTime64 = {};
	fileTime64.LowPart = fileTime.dwLowDateTime;
	fileTime64.HighPart = fileTime.dwHighDateTime;
	return fileTime64.QuadPart;
}

static std::optional<uint64_t> TryGetFileWriteTimeU64(const std::wstring& filePath)
{
	WIN32_FILE_ATTRIBUTE_DATA fileAttributeData = {};
	if (!GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &fileAttributeData))
	{
		return std::nullopt;
	}

	return FileTimeToU64(fileAttributeData.ftLastWriteTime);
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

static void EnforcePerFileLimit_Locked(std::vector<std::wstring>& backupPaths, uint32_t maxBackupsPerFile)
{
	while (backupPaths.size() > maxBackupsPerFile)
	{
		std::wstring oldestBackupPath = backupPaths.front();
		backupPaths.erase(backupPaths.begin());

		std::error_code removeError;
		std::fs::remove(std::fs::path(oldestBackupPath), removeError);
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
		std::wstring backupPath;
	};

	std::vector<GlobalBackupItem> allBackups;

	{
		std::shared_lock<std::shared_mutex> lock(g_indexMutex);

		for (const auto& kv : g_backupIndex)
		{
			const std::wstring& originalPath = kv.first;
			const std::vector<std::wstring>& backupPaths = kv.second;

			for (const std::wstring& backupPath : backupPaths)
			{
				allBackups.push_back(GlobalBackupItem{ originalPath, backupPath });
			}
		}
	}

	std::sort(allBackups.begin(), allBackups.end(), [](const GlobalBackupItem& left, const GlobalBackupItem& right)
	{
		return left.backupPath < right.backupPath;
	});


	size_t globalIndex = 0;

	while (currentBytes > maxBytes && globalIndex < allBackups.size())
	{
		std::error_code errorCode;
		uint64_t removedFileSize = 0;

		if (std::fs::exists(allBackups[globalIndex].backupPath, errorCode))
		{
			removedFileSize = (uint64_t)std::fs::file_size(allBackups[globalIndex].backupPath, errorCode);
		}

		std::fs::remove(allBackups[globalIndex].backupPath, errorCode);

		{
			std::unique_lock<std::shared_mutex> lock(g_indexMutex);

			auto foundItem = g_backupIndex.find(allBackups[globalIndex].originalPath);
			if (foundItem != g_backupIndex.end())
			{
				auto& backupPaths = foundItem->second;

				backupPaths.erase( std::remove( backupPaths.begin(), backupPaths.end(), allBackups[globalIndex].backupPath), backupPaths.end() );
			}
		}

		if (removedFileSize > 0 && currentBytes >= removedFileSize)
		{
			currentBytes -= removedFileSize;
		}

		++globalIndex;
	}
}

static std::wstring MakeBackupPath(const std::wstring& backupRoot, const std::wstring& originalFullPath)
{
	std::fs::path originalPath(originalFullPath);
	std::fs::path originalDir = originalPath.parent_path();
	std::fs::path originalStem = originalPath.stem();
	std::fs::path originalExt = originalPath.extension();

	std::wstring sanitizedDir = SanitizePathForBackup(originalDir.wstring());
	std::fs::path destinationDir = std::fs::path(backupRoot) / std::fs::path(sanitizedDir);

	std::wstring timestamp = MakeTimestampStr();
	std::wstring destinationFileName = originalStem.wstring() + L"_backup_" + timestamp + originalExt.wstring();

	return (destinationDir / destinationFileName).wstring();
}

static bool CopyToBackupAndIndex(const WatchedFolder& watchedFolder, const std::wstring& filePath)
{
	(void)watchedFolder;

	if (g_settings.backupRoot.empty())
	{
		BackupOperation backupOperation = {};
		backupOperation.timeStamp = MakeTimestampStr();
		backupOperation.originalPath = filePath;
		backupOperation.result = L"Skipped (backup folder not set)";
		PushOperation(backupOperation);
		return false;
	}

	std::error_code errorCode;
	if (!std::fs::exists(filePath, errorCode) || !std::fs::is_regular_file(filePath, errorCode))
	{
		return false;
	}

	std::wstring destinationPath = MakeBackupPath(g_settings.backupRoot, filePath);
	EnsureDirExists(std::fs::path(destinationPath).parent_path());

	BOOL copySucceeded = CopyFileW(filePath.c_str(), destinationPath.c_str(), FALSE);
	if (!copySucceeded)
	{
		DWORD win32Error = GetLastError();

		BackupOperation backupOperation = {};
		backupOperation.timeStamp = MakeTimestampStr();
		backupOperation.originalPath = filePath;
		backupOperation.backupPath = destinationPath;
		backupOperation.result = L"Copy failed (Win32 error " + std::to_wstring(win32Error) + L")";
		PushOperation(backupOperation);
		return false;
	}

	uint64_t destinationWriteTime = 0;
	if (auto destinationWriteTimeOpt = TryGetFileWriteTimeU64(destinationPath); destinationWriteTimeOpt.has_value())
	{
		destinationWriteTime = destinationWriteTimeOpt.value();
	}

	{
		std::unique_lock<std::shared_mutex> lock(g_indexMutex);

		std::vector<std::wstring>& backupPaths = g_backupIndex[filePath];
		backupPaths.push_back(destinationPath);

		std::sort(backupPaths.begin(), backupPaths.end(), [](const std::wstring& leftPath, const std::wstring& rightPath)
		{
			return leftPath < rightPath;
		});

		EnforcePerFileLimit_Locked(backupPaths, g_settings.maxBackupsPerFile);
	}

	{
		BackupOperation backupOperation = {};
		backupOperation.timeStamp = MakeTimestampStr();
		backupOperation.originalPath = filePath;
		backupOperation.backupPath = destinationPath;
		backupOperation.result = L"OK";
		PushOperation(backupOperation);
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

		{
			std::unique_lock<std::shared_mutex> lock(g_indexMutex);
			g_backupIndex[originalFullPath].push_back(backupFilePath.wstring());
		}
	}

	{
		std::unique_lock<std::shared_mutex> lock(g_indexMutex);

		for (auto& kv : g_backupIndex)
		{
			std::vector<std::wstring>& backupPaths = kv.second;

			std::sort(backupPaths.begin(), backupPaths.end(), [](const std::wstring& leftPath, const std::wstring& rightPath)
			{
				return leftPath < rightPath;
			});

			EnforcePerFileLimit_Locked(backupPaths, g_settings.maxBackupsPerFile);
		}
	}

	EnforceGlobalSizeLimit(std::fs::path(g_settings.backupRoot), g_settings.maxBackupSizeMB);

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
		BackupOperation backupOperation = {};
		backupOperation.timeStamp = MakeTimestampStr();
		backupOperation.originalPath = watchedFolder.path;
		backupOperation.result = L"Watcher failed to open folder";
		PushOperation(backupOperation);
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
				DWORD win32Error = GetLastError();

				BackupOperation backupOperation = {};
				backupOperation.timeStamp = MakeTimestampStr();
				backupOperation.originalPath = watchedFolder.path;
				backupOperation.result = L"ReadDirectoryChangesW failed (Win32 error " + std::to_wstring(win32Error) + L")";
				PushOperation(backupOperation);
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
		BackupOperation backupOperation = {};
		backupOperation.timeStamp = MakeTimestampStr();
		backupOperation.originalPath = originalFilePath;
		backupOperation.backupPath = backupFilePath;
		backupOperation.result = L"Diff tool not set";
		PushOperation(backupOperation);
		return;
	}

	if (!FileExists(diffToolPath))
	{
		BackupOperation backupOperation = {};
		backupOperation.timeStamp = MakeTimestampStr();
		backupOperation.originalPath = originalFilePath;
		backupOperation.backupPath = backupFilePath;
		backupOperation.result = L"Diff tool path does not exist";
		PushOperation(backupOperation);
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
		BackupOperation backupOperation = {};
		backupOperation.timeStamp = MakeTimestampStr();
		backupOperation.originalPath = originalFilePath;
		backupOperation.backupPath = backupFilePath;
		backupOperation.result = L"Failed to launch diff tool";
		PushOperation(backupOperation);
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
						if (ImGui::InputTextStdString("##include_filters", watchedFolder.includeFiltersCSV))
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
						if (ImGui::InputTextStdString("##exclude_filters", watchedFolder.excludeFiltersCSV))
						{
							MarkSettingsDirty();
						}
					}

					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted("Actions");
					ImGui::TableNextColumn();
					if (ImGui::Button("Apply All"))
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

static std::wstring BackupTimestampFromBackupPath(const std::wstring& backupPath)
{
	std::wstring backupFileName = std::fs::path(backupPath).filename().wstring();
	std::wstring outDate;
	std::wstring outTime;
	if (!ParseBackupTimestamp(backupFileName, outDate, outTime))
	{
		return L"-";
	}

	return outDate + L" " + outTime;
}

static void UI_BackedUpFiles()
{
	ImGui::Dummy(ImVec2(0,4));
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0,4));

	static std::wstring searchText;
	static std::wstring selectedOriginalPath;
	static std::wstring selectedBackupPath;
	std::wstring latestBackupPath;
	static float leftPaneWidth = 520.0f;

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
	bool isDiffPressed = isCtrlDown && ImGui::IsKeyPressed(ImGuiKey_D, false);
	bool refreshRequested = ImGui::IsKeyPressed(ImGuiKey_F5, false);

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

	{
		std::shared_lock<std::shared_mutex> indexLock(g_indexMutex);
	
		std::vector<std::wstring> visibleOriginalPaths;
	
		visibleOriginalPaths.reserve(g_backupIndex.size());

		for (const auto& indexPair : g_backupIndex)
		{
			if (indexPair.second.empty())
			{
				continue;
			}

			if (!ContainsAllKeywords(indexPair.first, searchText))
			{
				continue;
			}

			visibleOriginalPaths.push_back(indexPair.first);
		}

		if (visibleOriginalPaths.empty())
		{
			selectedOriginalPath.clear();
			selectedBackupPath.clear();
		}
		else
		{
			bool selectedIsVisible = (std::find(visibleOriginalPaths.begin(), visibleOriginalPaths.end(), selectedOriginalPath) != visibleOriginalPaths.end());

			if (!selectedIsVisible)
			{
				selectedOriginalPath = visibleOriginalPaths.front();
				selectedBackupPath.clear();
			}
		}

		if (ImGui::BeginChild("backed_up_files_left", ImVec2(leftPaneWidth, paneHeight), false))
		{
			if (ImGui::BeginTable("backed_up_files_left_table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
			{
				ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 1.0f);
				ImGui::TableSetupColumn("Filename", ImGuiTableColumnFlags_WidthStretch, 0.6f);
				ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 60.0f);
				ImGui::TableSetupColumn("Latest Backup", ImGuiTableColumnFlags_WidthFixed, 170.0f);
				ImGui::TableHeadersRow();

				for (const std::wstring& originalPath : visibleOriginalPaths)
				{
					auto it = g_backupIndex.find(originalPath);
					if (it == g_backupIndex.end())
					{
						continue;
					}
					const std::vector<std::wstring>& backupPaths = it->second;
					if (backupPaths.empty())
					{
						continue;
					}

					std::fs::path originalFsPath(originalPath);
					std::wstring originalNameWide = originalFsPath.filename().wstring();
					std::wstring originalFolderWide = originalFsPath.parent_path().wstring();
					std::string originalNameUtf8 = WToUTF8(originalNameWide);
					std::string originalFolderUtf8 = WToUTF8(originalFolderWide);
					std::string originalPathUtf8 = WToUTF8(originalPath);

					ImGui::PushID(originalPath.data());
					ImGui::TableNextRow();
					float rowMinY = ImGui::GetCursorScreenPos().y;

					ImGui::TableNextColumn();
					ImGui::TextClickable("%s", originalFolderUtf8.c_str());
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("%s", originalFolderUtf8.c_str());
					}
					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						OpenExplorerSelectPath(originalFolderWide);
					}
					if (ImGui::BeginPopupContextItem("folder_context"))
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
					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						selectedOriginalPath = originalPath;
						selectedBackupPath.clear();
						OpenFileWithShell(originalPath);
					}
					if (ImGui::BeginPopupContextItem("original_context"))
					{
						if (ImGui::MenuItem("Show in Explorer"))
						{
							selectedOriginalPath = originalPath;
							selectedBackupPath.clear();
							OpenExplorerSelectPath(originalPath);
						}
						ImGui::EndPopup();
					}

					ImGui::TableNextColumn();
					ImGui::Text("%d", (int)backupPaths.size());

					ImGui::TableNextColumn();
					{
						const std::wstring& latestBackupForRow = backupPaths.back();
						std::wstring latestTimestamp = BackupTimestampFromBackupPath(latestBackupForRow);
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

						bool isHovered = ImGui::IsMouseHoveringRect(rowMin, rowMax, false);
						bool isSelected = (selectedOriginalPath == originalPath);
						if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
						{
							selectedOriginalPath = originalPath;
							selectedBackupPath.clear();
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

				ImGui::EndTable();
			}
		}
		ImGui::EndChild();

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
			if (selectedOriginalPath.empty())
			{
				ImGui::TextDisabled("No backed up file selected.");
			}
			else
			{
				auto selectedIt = g_backupIndex.find(selectedOriginalPath);
				if (selectedIt == g_backupIndex.end() || selectedIt->second.empty())
				{
					ImGui::TextDisabled("No backups available for selected file.");
				}
				else
				{
					const std::vector<std::wstring>& selectedBackups = selectedIt->second;
					latestBackupPath = selectedBackups.back();

					if (ImGui::BeginTable("selected_file_backups_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY))
					{
						ImGui::TableSetupColumn("Date & Time", ImGuiTableColumnFlags_WidthFixed, 190.0f);
						ImGui::TableSetupColumn("Backup File", ImGuiTableColumnFlags_WidthStretch, 1.0f);
						ImGui::TableSetupColumn("##Actions", ImGuiTableColumnFlags_WidthFixed, 250.0f);
						ImGui::TableHeadersRow();

						for (int backupIndex = (int)selectedBackups.size() - 1; backupIndex >= 0; --backupIndex)
						{
							const std::wstring& backupPath = selectedBackups[backupIndex];
							std::wstring backupFileName = std::fs::path(backupPath).filename().wstring();
							std::string backupFileNameUtf8 = WToUTF8(backupFileName);

							ImGui::PushID(backupIndex);

							ImGui::TableNextRow();
							float backupRowMinY = ImGui::GetCursorScreenPos().y;

							ImGui::TableNextColumn();
							{
								std::wstring timestamp = BackupTimestampFromBackupPath(backupPath);
						
								ImGui::TextUnformatted(WToUTF8(timestamp).c_str());

								if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
								{
									selectedBackupPath = backupPath;
								}
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
									const std::wstring& previousBackupPath = selectedBackups[backupIndex - 1];
									LaunchDiffTool(g_settings.diffToolPath, previousBackupPath, backupPath);
								}
								if (!hasPreviousBackup)
								{
									ImGui::EndDisabled();
								}

								ImGui::SameLine();
								if (ImGui::Button("Diff Current"))
								{
									LaunchDiffTool(g_settings.diffToolPath, backupPath, selectedOriginalPath);
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
											const std::wstring& previousBackupPath = selectedBackups[backupIndex - 1];
											LaunchDiffTool(g_settings.diffToolPath, previousBackupPath, backupPath);
										}
									}

									if (ImGui::MenuItem("Diff Current"))
									{
										LaunchDiffTool(g_settings.diffToolPath, backupPath, selectedOriginalPath);
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
		ImGui::EndChild();

		ImGui::PopStyleVar();
	}

	if (isDiffPressed)
	{
		if (selectedBackupPath.empty())
			selectedBackupPath = latestBackupPath;

		if (!selectedOriginalPath.empty() && !selectedBackupPath.empty())
		{
			LaunchDiffTool(g_settings.diffToolPath, selectedBackupPath, selectedOriginalPath);
		}
	}

	if (refreshRequested)
	{
		ScanBackupFolder();
	}
}

static void UI_History()
{
	ImGui::Dummy(ImVec2(0,4));
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0,4));

	static int selectedOperationIndex = -1;

	ImGui::Text("Last %d operations", kOperationHistoryMaxCount);

	ImGui::Separator();

	bool isCtrlDown = ImGui::GetIO().KeyCtrl;
	bool isDiffPressed = isCtrlDown && ImGui::IsKeyPressed(ImGuiKey_D, false);

	BackupOperation selectedOperationCopy = {};
	bool hasSelectedOperation = false;

	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));

	if (ImGui::BeginTable("ops", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
	{
		ImGui::TableSetupColumn("Path");
		ImGui::TableSetupColumn("Backup Time", ImGuiTableColumnFlags_WidthFixed, 160.0f);
		ImGui::TableSetupColumn("Backup Path");
		ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, 220.0f);
		ImGui::TableHeadersRow();

		std::lock_guard<std::mutex> lock(g_operationsMutex);

		for (int operationIndex = (int)g_operations.size() - 1; operationIndex >= 0; --operationIndex)
		{
			const BackupOperation& backupOperation = g_operations[operationIndex];

			ImGui::PushID(operationIndex);

			ImGui::TableNextRow();

			float rowMinY = ImGui::GetCursorScreenPos().y;

			ImGui::TableNextColumn();
			{
				std::string originalUtf8 = WToUTF8(backupOperation.originalPath);

				ImGui::TextClickable("%s", originalUtf8.c_str());

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
			std::wstring formattedTimeStamp = FormatOperationTimestampForDisplay(backupOperation.timeStamp);
			ImGui::TextUnformatted(WToUTF8(formattedTimeStamp).c_str());

			ImGui::TableNextColumn();
			{
				std::string backupUtf8 = WToUTF8(backupOperation.backupPath);

				ImGui::TextClickable("%s", backupUtf8.c_str());

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
			ImGui::TextUnformatted(WToUTF8(backupOperation.result).c_str());

			float rowMaxY = ImGui::GetCursorScreenPos().y;

			HandleRowSelectAndHighlight(operationIndex, selectedOperationIndex, rowMinY, rowMaxY);

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

	if (isDiffPressed && hasSelectedOperation)
	{
		LaunchDiffTool(g_settings.diffToolPath, selectedOperationCopy.backupPath, selectedOperationCopy.originalPath);
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

	wchar_t todayPrefix[64] = {};
	swprintf_s(todayPrefix, L"_backup_%04d_%02d_%02d__", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);

	g_todayPrefix = todayPrefix;

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

		wchar_t todayPrefix[64] = {};
		swprintf_s(todayPrefix, L"_backup_%04d_%02d_%02d__", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);

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

	return false;
}

void AppShutdown()
{
	StopWatchers();
}
