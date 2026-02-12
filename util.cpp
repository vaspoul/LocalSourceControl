#include "main.h"
#include "util.h"
#include "imgui/imgui_internal.h"

std::wstring UTF8ToW(const std::string& s)
{
	if (s.empty())
	{
		return L"";
	}
	int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring out;
	out.resize((size_t)len);
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
	return out;
}

std::string WToUTF8(const std::wstring& s)
{
	if (s.empty())
	{
		return "";
	}
	int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
	std::string out;
	out.resize((size_t)len);
	WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len, nullptr, nullptr);
	return out;
}

std::wstring Trim(const std::wstring& s)
{
	size_t a = 0;
	while (a < s.size() && iswspace(s[a]))
	{
		++a;
	}
	size_t b = s.size();
	while (b > a && iswspace(s[b - 1]))
	{
		--b;
	}
	return s.substr(a, b - a);
}

std::wstring ToLower(const std::wstring& s)
{
	std::wstring out = s;
	std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c)
	{
		return (wchar_t)towlower(c);
	});
	return out;
}

std::string ToLower(const std::string& s)
{
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c)
	{
		return (char)tolower(c);
	});
	return out;
}

std::vector<std::wstring> SplitCSV(const std::wstring& csv)
{
	std::vector<std::wstring> out;
	std::wstring cur;

	for (wchar_t c : csv)
	{
		if (c == L',' || c == L';' || iswspace(c))
		{
			std::wstring t = Trim(cur);
			if (!t.empty())
			{
				out.push_back(t);
			}
			cur.clear();
		}
		else
		{
			cur.push_back(c);
		}
	}

	std::wstring t = Trim(cur);
	if (!t.empty())
	{
		out.push_back(t);
	}

	return out;
}

bool ContainsAllKeywords(const std::wstring& phrase, const std::wstring& keywords)
{
	if (keywords.empty())
	{
		return true;
	}

	std::wstring haystackLower = ToLower(phrase);

	std::vector<std::wstring> tokens = SplitCSV(keywords);

	for (const std::wstring& keywordRaw : tokens)
	{
		std::wstring keyword = ToLower(Trim(keywordRaw));
		if (keyword.empty())
		{
			continue;
		}

		if (haystackLower.find(keyword) == std::wstring::npos)
		{
			return false;
		}
	}

	return true;
}



std::wstring MakeTimestampStr()
{
	using namespace std::chrono;
	auto now = system_clock::now();
	auto tt = system_clock::to_time_t(now);
	tm tmv = {};
	localtime_s(&tmv, &tt);

	wchar_t buf[64] = {};
	swprintf_s(buf, L"%04d_%02d_%02d__%02d_%02d_%02d",
		tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
		tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

	return buf;
}

bool IsPathUnderRoot(const std::wstring& candidatePath, const std::wstring& rootPath)
{
	if (rootPath.empty())
	{
		return false;
	}

	std::error_code errorCode;

	std::fs::path candidateCanonical = std::fs::weakly_canonical(std::fs::path(candidatePath), errorCode);
	if (errorCode)
	{
		errorCode.clear();
		candidateCanonical = std::fs::path(candidatePath);
	}

	std::fs::path rootCanonical = std::fs::weakly_canonical(std::fs::path(rootPath), errorCode);
	if (errorCode)
	{
		errorCode.clear();
		rootCanonical = std::fs::path(rootPath);
	}

	std::wstring candidateLower = ToLower(candidateCanonical.wstring());
	std::wstring rootLower = ToLower(rootCanonical.wstring());

	// Ensure root ends with backslash for proper prefix match
	if (!rootLower.empty() && rootLower.back() != L'\\')
	{
		rootLower.push_back(L'\\');
	}

	return candidateLower.rfind(rootLower, 0) == 0;
}

namespace ImGui
{
bool InputTextStdString(const char* label, std::string& s, ImGuiInputTextFlags flags)
{
	if (s.capacity() < 256)
	{
		s.reserve(256);
	}

	flags |= ImGuiInputTextFlags_CallbackResize;
	return ImGui::InputText(label, s.data(), s.capacity() + 1, flags,
		[](ImGuiInputTextCallbackData* data)
		{
			if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
			{
				auto* str = (std::string*)data->UserData;
				str->resize((size_t)data->BufTextLen);
				data->Buf = str->data();
			}
			return 0;
		}, (void*)&s);
}

bool InputTextStdString(const char* label, std::wstring& value, ImGuiInputTextFlags flags)
{
	std::string utf8Value = WToUTF8(value);

	bool changed = ImGui::InputTextStdString(label, utf8Value, flags);
	if (changed)
	{
		value = UTF8ToW(utf8Value);
	}

	return changed;
}

void HelpTooltip(const char* text)
{
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
	{
		ImGui::SetTooltip("%s", text);
	}
}

// See https://github.com/ocornut/imgui/issues/5280
bool TextClickable(const char* fmt, ...)
{
	ImU32 hoverColor = ImGui::GetColorU32(ImGuiCol_ButtonHovered);

	ImGuiContext& g = *GImGui;

	ImGuiWindow* window = ImGui::GetCurrentWindow();
	
	if (window->SkipItems)
		return false;

	// Format text
	char txtBuffer[1024*3+1];
	va_list args;
	va_start(args, fmt);
	const char* text_begin = txtBuffer;
	const char* text_end = txtBuffer + ImFormatStringV(txtBuffer, IM_ARRAYSIZE(txtBuffer), fmt, args);
	va_end(args);

	// Layout
	const ImVec2 text_pos(window->DC.CursorPos.x, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
	const ImVec2 text_size = ImGui::CalcTextSize(text_begin, text_end);

	ImRect bb(text_pos.x, text_pos.y, text_pos.x + text_size.x, text_pos.y + text_size.y);
	
	ImGui::ItemSize(text_size, 0.0f);
	
	if (!ImGui::ItemAdd(bb, 0))
		return false;

	// Render
	bool hovered = ImGui::IsItemHovered();
	
	if (hovered)
		ImGui::PushStyleColor(ImGuiCol_Text, hoverColor);

	ImGui::RenderText(bb.Min, text_begin, text_end, false);
	
	if (hovered)
		ImGui::PopStyleColor();

	return ImGui::IsItemClicked();
}

bool TextClickable(const std::wstring& text)
{
	std::string utf8Text = WToUTF8(text);
	return TextClickable("%s", utf8Text.c_str());
}

} // namespace ImGui