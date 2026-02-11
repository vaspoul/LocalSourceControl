// main.cpp

#include "main.h"
#include "app.h"
#include "util.h"
#include "settings.h"

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
	std::atomic<bool>							stopRequested = false;

	std::mutex									debounceMutex;
	std::unordered_map<std::wstring, uint64_t>	lastEventTickByPath;
};

static std::mutex													g_operationsMutex;
static std::vector<BackupOperation>									g_operations;
static const uint32_t												kOperationHistoryMaxCount = 1024;

static std::shared_mutex											g_indexMutex;
static std::unordered_map<std::wstring, std::vector<std::wstring>>	g_backupIndex;

static std::mutex													g_watchersMutex;
static std::vector<std::unique_ptr<FolderWatcher>>					g_watchers;

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

static void PushOperation(const BackupOperation& backupOperation)
{
	std::lock_guard<std::mutex> lock(g_operationsMutex);

	g_operations.push_back(backupOperation);

	if (g_operations.size() > kOperationHistoryMaxCount)
	{
		g_operations.erase(g_operations.begin(), g_operations.begin() + (g_operations.size() - kOperationHistoryMaxCount));
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

static bool FilterMatchToken(const std::wstring& fileNameLower, const std::wstring& extWithDotLower, const std::wstring& tokenRaw)
{
	std::wstring token = ToLower(Trim(tokenRaw));
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

static bool PassesFilters(const WatchedFolder& watchedFolder, const std::wstring& fullPath)
{
	std::wstring fileNameLower = ToLower(std::fs::path(fullPath).filename().wstring());
	std::wstring extLower = ToLower(std::fs::path(fullPath).extension().wstring());

	std::vector<std::wstring> includeTokens = SplitCSV(watchedFolder.includeFiltersCSV);
	std::vector<std::wstring> excludeTokens = SplitCSV(watchedFolder.excludeFiltersCSV);

	for (const auto& excludeToken : excludeTokens)
	{
		if (FilterMatchToken(fileNameLower, extLower, excludeToken))
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
		if (FilterMatchToken(fileNameLower, extLower, includeToken))
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

		std::fs::path relativeDir = std::fs::relative( backupFilePath.parent_path(), backupRootPath, errorCode);

		if (errorCode)
		{
			errorCode.clear();
			continue;
		}

		std::fs::path originalPath = relativeDir / std::fs::path(originalStem + originalExt);

		{
			std::unique_lock<std::shared_mutex> lock(g_indexMutex);
			g_backupIndex[originalPath.wstring()].push_back(backupFilePath.wstring());
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

	HANDLE directoryHandle = CreateFileW(
		watchedFolder.path.c_str(),
		FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS,
		nullptr);

	if (directoryHandle == INVALID_HANDLE_VALUE)
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
			directoryHandle,
			notifyBuffer.data(),
			(DWORD)notifyBuffer.size(),
			watchedFolder.includeSubfolders ? TRUE : FALSE,
			notifyFlags,
			&bytesReturned,
			nullptr,
			nullptr);

		if (!readSucceeded)
		{
			DWORD win32Error = GetLastError();

			BackupOperation backupOperation = {};
			backupOperation.timeStamp = MakeTimestampStr();
			backupOperation.originalPath = watchedFolder.path;
			backupOperation.result = L"ReadDirectoryChangesW failed (Win32 error " + std::to_wstring(win32Error) + L")";
			PushOperation(backupOperation);
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
				if (PassesFilters(watchedFolder, fullPath))
				{
					if (!SkipBackup(*watcher, fullPath, nowTick))
					{
						CopyToBackupAndIndex(watchedFolder, fullPath);
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

	CloseHandle(directoryHandle);
}

static void StopWatchers()
{
	std::lock_guard<std::mutex> lock(g_watchersMutex);

	for (auto& watcher : g_watchers)
	{
		watcher->stopRequested.store(true);
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

static void UI_WatchedFolders()
{
	ImGui::TextUnformatted("Watched folders");
	ImGui::HelpTooltip(
		"Tokens are comma-separated.\n"
		"Supported token types:\n"
		"  - Extension: .png or png\n"
		"  - Wildcards: *.tmp, foo*.txt (matches filename)\n"
		"  - Substring: foo (matches filename substring)\n"
		"Exclude wins over include. If include list is empty, everything is included (subject to exclude)."
	);

	if (ImGui::Button("Add Folder"))
	{
		std::wstring selectedPath = BrowseForFolder(L"Select folder to watch");
		if (!selectedPath.empty())
		{
			WatchedFolder newWatchedFolder = {};
			newWatchedFolder.path = selectedPath;
			newWatchedFolder.includeSubfolders = true;

			g_settings.watched.push_back(newWatchedFolder);
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

	for (int folderIndex = 0; folderIndex < (int)g_settings.watched.size(); ++folderIndex)
	{
		WatchedFolder& watchedFolder = g_settings.watched[folderIndex];
		ImGui::PushID(folderIndex);

		bool isOpen = ImGui::CollapsingHeader(WToUTF8(watchedFolder.path).c_str(), ImGuiTreeNodeFlags_DefaultOpen);
		if (isOpen)
		{
			if (ImGui::Checkbox("Include sub-folders", &watchedFolder.includeSubfolders))
			{
				MarkSettingsDirty();
			}

			std::string includeText = WToUTF8(watchedFolder.includeFiltersCSV);
			if (includeText.capacity() < 512)
			{
				includeText.reserve(512);
			}

			ImGui::TextUnformatted("Include filters (CSV)");
			ImGui::HelpTooltip("Examples:\n  .png, .jpg\n  png, jpg\n  *.tmp\n  foo*, *bar*\n  report\nIf empty: include everything (unless excluded).");
			ImGui::SetNextItemWidth(-1.0f);

			if (ImGui::InputTextStdString("##include", includeText))
			{
				watchedFolder.includeFiltersCSV = UTF8ToW(includeText);
				MarkSettingsDirty();
			}

			std::string excludeText = WToUTF8(watchedFolder.excludeFiltersCSV);
			if (excludeText.capacity() < 512)
			{
				excludeText.reserve(512);
			}

			ImGui::TextUnformatted("Exclude filters (CSV)");
			ImGui::HelpTooltip("Examples:\n  .tmp, .bak\n  *autosave*\nExclude is checked first and always wins.");
			ImGui::SetNextItemWidth(-1.0f);

			if (ImGui::InputTextStdString("##exclude", excludeText))
			{
				watchedFolder.excludeFiltersCSV = UTF8ToW(excludeText);
				MarkSettingsDirty();
			}

			if (ImGui::Button("Change Path"))
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

			ImGui::SameLine();

			if (ImGui::Button("Remove"))
			{
				g_settings.watched.erase(g_settings.watched.begin() + folderIndex);
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
	static std::string searchText;

	ImGui::TextUnformatted("Search");
	ImGui::HelpTooltip("Search is split into keywords (whitespace, comma, semicolon).\nAll keywords must appear as substrings.\nExample: \"foo bar\" matches \"foontarbartastic\".");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputTextStdString("##search", searchText);

	ImGui::Separator();

	if (ImGui::BeginTable("backups", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
	{
		ImGui::TableSetupColumn("File");
		ImGui::TableSetupColumn("Backups", ImGuiTableColumnFlags_WidthFixed, 90.0f);
		ImGui::TableSetupColumn("Last backup", ImGuiTableColumnFlags_WidthFixed, 220.0f);
		ImGui::TableHeadersRow();

		std::shared_lock<std::shared_mutex> lock(g_indexMutex);

		for (const auto& kv : g_backupIndex)
		{
			const std::wstring& originalPath = kv.first;
			const auto& backupPaths = kv.second;

			std::string originalPathUtf8 = WToUTF8(originalPath);

			if (!searchText.empty())
			{
				// ContainsAllKeywords lowercases internally now
				if (!ContainsAllKeywords(originalPathUtf8, searchText))
				{
					continue;
				}
			}

			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(originalPathUtf8.c_str());

			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%d", (int)backupPaths.size());

			ImGui::TableSetColumnIndex(2);
			if (!backupPaths.empty())
			{
				const std::wstring& lastBackupPath = backupPaths.back();
				std::wstring backupFileName = std::fs::path(lastBackupPath).filename().wstring();

				size_t markerPos = backupFileName.rfind(L"_backup_");
				if (markerPos != std::wstring::npos)
				{
					std::wstring stampPlus = backupFileName.substr(markerPos + 8);
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

static void UI_History()
{
	ImGui::Text("Last %d operations", kOperationHistoryMaxCount);
	ImGui::HelpTooltip("Shows copy successes/failures and reasons for skips.");
	ImGui::Separator();

	if (ImGui::BeginTable("ops", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
	{
		ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 160.0f);
		ImGui::TableSetupColumn("Original");
		ImGui::TableSetupColumn("Backup");
		ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, 220.0f);
		ImGui::TableHeadersRow();

		std::lock_guard<std::mutex> lock(g_operationsMutex);

		for (const auto& backupOperation : g_operations)
		{
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(WToUTF8(backupOperation.timeStamp).c_str());

			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(WToUTF8(backupOperation.originalPath).c_str());

			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(WToUTF8(backupOperation.backupPath).c_str());

			ImGui::TableSetColumnIndex(3);
			ImGui::TextUnformatted(WToUTF8(backupOperation.result).c_str());
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
		std::wstring selectedPath = BrowseForFolder(L"Select backup folder");

		if (!selectedPath.empty())
		{
			g_settings.backupRoot = selectedPath;

			// Requirement: button updates the local root variable
			backupRootUtf8 = WToUTF8(g_settings.backupRoot);

			MarkSettingsDirty();
			SaveSettings();
			ScanBackupFolder();
		}
	}

	ImGui::HelpTooltip("Backups are nested by original path under this root.");

	ImGui::SetNextItemWidth(-1.0f);
	if (ImGui::InputTextStdString("##backupRoot", backupRootUtf8))
	{
		g_settings.backupRoot = UTF8ToW(backupRootUtf8);
		MarkSettingsDirty();
	}

	ImGui::Separator();

	int maxFolderSizeMB = (int)g_settings.maxBackupSizeMB;
	ImGui::TextUnformatted("Max backup folder size (MB)");
	ImGui::HelpTooltip("When exceeded, oldest backups across all files are deleted until within the limit.");
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

	int maxPerFileBackups = (int)g_settings.maxBackupsPerFile;
	ImGui::TextUnformatted("Max backups per file");
	ImGui::HelpTooltip("Per original file, keep at most this many backups. Oldest backups are deleted first.");
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

	ImGui::Separator();

	if (ImGui::Button("Apply"))
	{
		MarkSettingsDirty();
		SaveSettings();
		ScanBackupFolder();
		EnforceGlobalSizeLimit(std::fs::path(g_settings.backupRoot), g_settings.maxBackupSizeMB);
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
		if (ImGui::BeginTabItem("Backup History"))
		{
			UI_History();
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
