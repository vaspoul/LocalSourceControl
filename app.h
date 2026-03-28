
#include "imgui/imgui.h"
#include <cstdint>

void AppInit();
bool AppLoop();
bool AppDraw();
void AppShutdown();

std::wstring	BrowseForFolder(const std::wstring& title);
std::wstring	BrowseForExeFile();
void			OpenFileWithShell(const std::wstring& filePath);
void			OpenExplorerSelectPath(const std::wstring& filePath);
bool			FileExists(const std::wstring& filePath);

void			TrayUpdateStatus(uint32_t count, bool isPaused);
