#ifndef UTIL_H
#define UTIL_H

#include "imgui/imgui.h"

std::wstring UTF8ToW(const std::string& s);
std::string WToUTF8(const std::wstring& s);
std::wstring TrimW(const std::wstring& s);
std::wstring ToLowerW(const std::wstring& s);
std::string ToLowerA(const std::string& s);
std::vector<std::wstring> SplitCSVW(const std::wstring& csv);
bool ContainsAllKeywords(const std::string& haystackLower, const std::string& query);
std::wstring NowStamp();
bool InputTextStdString(const char* label, std::string& s, ImGuiInputTextFlags flags = 0);
void HelpTooltip(const char* text);

#endif // UTIL_H
