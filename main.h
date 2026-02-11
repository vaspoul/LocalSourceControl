#ifndef MAIN_H
#define MAIN_H

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

namespace fs = std::filesystem;


struct WatchedFolder
{
	std::wstring	path;
	bool			includeSubfolders = true;
	std::wstring	includeFiltersCSV;
	std::wstring	excludeFiltersCSV;
};

#endif // MAIN_H
