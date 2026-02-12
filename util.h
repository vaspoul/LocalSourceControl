#ifndef UTIL_H
#define UTIL_H

#include "imgui/imgui.h"

std::wstring				UTF8ToW(const std::string& s);
std::string					WToUTF8(const std::wstring& s);
std::wstring				Trim(const std::wstring& s);
std::wstring				ToLower(const std::wstring& s);
std::string					ToLower(const std::string& s);
std::vector<std::wstring>	SplitCSV(const std::wstring& csv);
bool						ContainsAllKeywords(const std::wstring& phrase, const std::wstring& keywords);
std::wstring				MakeTimestampStr();
bool						IsPathUnderRoot(const std::wstring& candidatePath, const std::wstring& rootPath);

namespace ImGui
{
bool						InputTextStdString(const char* label, std::string& s, ImGuiInputTextFlags flags = 0);
bool						InputTextStdString(const char* label, std::wstring& value, ImGuiInputTextFlags flags = 0);
void						HelpTooltip(const char* text);
bool						TextClickable(const char* fmt, ...);
bool						TextClickable(const std::wstring& text);
}

#endif // UTIL_H
