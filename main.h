#ifndef MAIN_H
#define MAIN_H

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <ShlObj_core.h>	// BROWSEINFOW (requested)
#include <shobjidl.h>		// IFileDialog (new-style folder picker)
#include <shlwapi.h>
#include <shellapi.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <list>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

#define FMT_HEADER_ONLY
#define FMT_UNICODE 1
#include "fmt/format.h"
#include "fmt/xchar.h"

namespace std
{
	namespace fs = filesystem;

	template<class Key, class Value>
	using umap = unordered_map<Key, Value>;

	template<class Value>
	using uset = unordered_set<Value>;
}

struct WatchedFolder
{
	std::wstring	path;
	bool			includeSubfolders = true;
	std::wstring	includeFiltersCSV;
	std::wstring	excludeFiltersCSV;
};

#endif // MAIN_H
