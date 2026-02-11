#include "main.h"
#include "util.h"

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

bool ContainsAllKeywords(const std::string& phrase, const std::string& keywords)
{
	if (keywords.empty())
	{
		return true;
	}

	// Lowercase phrase internally
	std::string phraseLower = ToLower(phrase);

	// Convert keywords to wide and split using same token logic
	std::wstring keywordsW = UTF8ToW(keywords);
	std::vector<std::wstring> tokensW = SplitCSV(keywordsW);

	if (tokensW.empty())
	{
		return true;
	}

	for (const std::wstring& tokenW : tokensW)
	{
		std::string token = ToLower(WToUTF8(tokenW));

		if (token.empty())
		{
			continue;
		}

		if (phraseLower.find(token) == std::string::npos)
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

void HelpTooltip(const char* text)
{
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
	{
		ImGui::SetTooltip("%s", text);
	}
}

} // namespace ImGui