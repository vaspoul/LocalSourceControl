
#include "imgui/imgui.h"

void AppInit();
bool AppLoop();
bool AppDraw();
void AppShutdown();

std::wstring	BrowseForFolder(const std::wstring& title);
std::wstring	BrowseForExeFile();
void			OpenFileWithShell(const std::wstring& filePath);
void			OpenExplorerSelectPath(const std::wstring& filePath);
bool			FileExists(const std::wstring& filePath);
