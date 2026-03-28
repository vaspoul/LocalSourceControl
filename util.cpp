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

bool InputTextMultilineStdString(const char* label, std::string& s, const ImVec2& size, ImGuiInputTextFlags flags)
{
	if (s.capacity() < 256)
	{
		s.reserve(256);
	}

	flags |= ImGuiInputTextFlags_CallbackResize;
	return ImGui::InputTextMultiline(label, s.data(), s.capacity() + 1, size, flags,
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

bool InputTextMultilineStdString(const char* label, std::wstring& value, const ImVec2& size, ImGuiInputTextFlags flags)
{
	std::string utf8Value = WToUTF8(value);

	bool changed = ImGui::InputTextMultilineStdString(label, utf8Value, size, flags);
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

bool DateTimePopup(const char* id, std::chrono::system_clock::time_point& timePoint, bool setToEndOfDay)
{
	bool changed = false;
	if (!ImGui::BeginPopup(id))
	{
		return false;
	}

	const auto clamp = [](int value, const int minValue, const int maxValue) -> int
	{
			 if (value <= minValue)		return minValue;
		else if (value >= maxValue)		return maxValue;
		else							return value;
	};

	const auto daysInMonth = [&](int month, int year)
	{
		static const int daysPerMonth[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };

		month = clamp(month, 1, 12);

		if (month != 2)
		{
			return daysPerMonth[month - 1];
		}

		bool isLeap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);

		return isLeap ? 29 : 28;
	};

	const auto clampDateTime = [&](int* year, int* month, int* day, int* hour, int* minute, int* second)
	{
		*year	= clamp(*year, 1970, 2100);
		*month	= clamp(*month, 1, 12);
		*day	= clamp(*day, 1, daysInMonth(*month, *year));
		*hour	= clamp(*hour, 0, 23);
		*minute = clamp(*minute, 0, 59);
		*second = clamp(*second, 0, 59);
	};

	ImGui::PushID(id);
	int year;
	int month;
	int day;
	int hour;
	int minute;
	int second;

	{
		auto tt = std::chrono::system_clock::to_time_t(timePoint);
		tm tmv = {};
		localtime_s(&tmv, &tt);
		year = tmv.tm_year + 1900;
		month = tmv.tm_mon + 1;
		day = tmv.tm_mday;
		hour = tmv.tm_hour;
		minute = tmv.tm_min;
		second = tmv.tm_sec;
	}

	static const char* monthNames[12] =	{ "January","February","March","April","May","June", "July","August","September","October","November","December" };

	int monthIndex = clamp(month, 1, 12) - 1;
	int yearValue = clamp(year, 1970, 2100);

	ImGui::TextUnformatted("Date");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(140.0f);
	if (ImGui::Combo("##month", &monthIndex, monthNames, 12))
	{
		month = monthIndex + 1;
		changed = true;
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(90.0f);
	if (ImGui::BeginCombo("##year", std::to_string(yearValue).c_str()))
	{
		for (int y = 1970; y <= 2100; ++y)
		{
			bool isSelected = (y == yearValue);
			if (ImGui::Selectable(std::to_string(y).c_str(), isSelected))
			{
				year = y;
				changed = true;
			}
			if (isSelected)
			{
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::EndCombo();
	}

	ImGui::Separator();

	int dayCount = daysInMonth(month, year);

	int weekdaySundayBased;
	int weekdayMondayBased;

	{
		tm tmv = {};
		tmv.tm_year = year - 1900;
		tmv.tm_mon = month - 1;
		tmv.tm_mday = 1;
		tmv.tm_isdst = -1;
		mktime(&tmv);

		weekdaySundayBased = tmv.tm_wday; // 0=Sun
		weekdayMondayBased = (weekdaySundayBased + 6) % 7; // 0=Mon
	}

	if (ImGui::BeginTable("##calendar", 7, ImGuiTableFlags_SizingStretchSame))
	{
		ImGui::TableSetupColumn("Mon", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Tue", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Wed", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Thu", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Fri", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Sat", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Sun", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();

		int dayNum = 1 - weekdayMondayBased;
		for (int row = 0; row < 6; ++row)
		{
			ImGui::TableNextRow();
			for (int col = 0; col < 7; ++col, ++dayNum)
			{
				ImGui::TableNextColumn();
				if (dayNum < 1 || dayNum > dayCount)
				{
					ImGui::TextUnformatted(" ");
					continue;
				}

				std::string label = std::to_string(dayNum);
				float colWidth = ImGui::GetColumnWidth();
				float textWidth = ImGui::CalcTextSize(label.c_str()).x;
				float cursorX = ImGui::GetCursorPosX();
				ImGui::SetCursorPosX(cursorX + (colWidth - textWidth) * 0.5f);

				bool isSelected = (dayNum == day);
				ImGui::PushID(dayNum);
				if (ImGui::Selectable(label.c_str(), isSelected, ImGuiSelectableFlags_DontClosePopups, ImVec2(0.0f, 0.0f)))
				{
					day = dayNum;
					changed = true;
				}
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}

	ImGui::Separator();
	ImGui::TextUnformatted("Time (24h)");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(40.0f);
	if (ImGui::InputScalar("##hour", ImGuiDataType_S32, &hour, nullptr, nullptr, "%02d"))
	{
		changed = true;
	}
	ImGui::SameLine();
	ImGui::TextUnformatted(":");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(40.0f);
	if (ImGui::InputScalar("##minute", ImGuiDataType_S32, &minute, nullptr, nullptr, "%02d"))
	{
		changed = true;
	}
	ImGui::SameLine();
	ImGui::TextUnformatted(":");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(40.0f);
	if (ImGui::InputScalar("##second", ImGuiDataType_S32, &second, nullptr, nullptr, "%02d"))
	{
		changed = true;
	}

	ImGui::Separator();

	if (ImGui::Button("<", ImVec2(40.0f, 0.0f)))
	{
		auto tt = std::chrono::system_clock::to_time_t(timePoint);
		tm tmv = {};
		localtime_s(&tmv, &tt);
		tmv.tm_mday -= 1;
		tmv.tm_isdst = -1;
		time_t adjusted = mktime(&tmv);
		if (adjusted != (time_t)-1)
		{
			timePoint = std::chrono::system_clock::from_time_t(adjusted);
			auto adjustTt = std::chrono::system_clock::to_time_t(timePoint);
			tm adjustTm = {};
			localtime_s(&adjustTm, &adjustTt);
			year = adjustTm.tm_year + 1900;
			month = adjustTm.tm_mon + 1;
			day = adjustTm.tm_mday;
			hour = adjustTm.tm_hour;
			minute = adjustTm.tm_min;
			second = adjustTm.tm_sec;
			changed = true;
		}
	}

	ImGui::SameLine();

	if (ImGui::Button("Today", ImVec2(80.0f, 0.0f)))
	{
		time_t now = time(nullptr);
		tm tmvNow = {};
		localtime_s(&tmvNow, &now);
		year = tmvNow.tm_year + 1900;
		month = tmvNow.tm_mon + 1;
		day = tmvNow.tm_mday;
		if (setToEndOfDay)
		{
			hour = 23;
			minute = 59;
			second = 59;
		}
		else
		{
			hour = 0;
			minute = 0;
			second = 0;
		}
		changed = true;
	}

	ImGui::SameLine();

	if (ImGui::Button("Now", ImVec2(80.0f, 0.0f)))
	{
		time_t now = time(nullptr);
		tm tmvNow = {};
		localtime_s(&tmvNow, &now);
		year = tmvNow.tm_year + 1900;
		month = tmvNow.tm_mon + 1;
		day = tmvNow.tm_mday;
		hour = tmvNow.tm_hour;
		minute = tmvNow.tm_min;
		second = tmvNow.tm_sec;
		changed = true;
	}

	ImGui::SameLine();

	if (ImGui::Button(">", ImVec2(40.0f, 0.0f)))
	{
		auto tt = std::chrono::system_clock::to_time_t(timePoint);
		tm tmv = {};
		localtime_s(&tmv, &tt);
		tmv.tm_mday += 1;
		tmv.tm_isdst = -1;
		time_t adjusted = mktime(&tmv);
		if (adjusted != (time_t)-1)
		{
			timePoint = std::chrono::system_clock::from_time_t(adjusted);
			auto adjustTt = std::chrono::system_clock::to_time_t(timePoint);
			tm adjustTm = {};
			localtime_s(&adjustTm, &adjustTt);
			year = adjustTm.tm_year + 1900;
			month = adjustTm.tm_mon + 1;
			day = adjustTm.tm_mday;
			hour = adjustTm.tm_hour;
			minute = adjustTm.tm_min;
			second = adjustTm.tm_sec;
			changed = true;
		}
	}

	clampDateTime(&year, &month, &day, &hour, &minute, &second);
	ImGui::PopID();
	ImGui::EndPopup();

	if (changed)
	{
		tm tmv = {};
		tmv.tm_year = year - 1900;
		tmv.tm_mon = month - 1;
		tmv.tm_mday = day;
		tmv.tm_hour = hour;
		tmv.tm_min = minute;
		tmv.tm_sec = second;
		tmv.tm_isdst = -1;
		time_t tt = mktime(&tmv);
		if (tt != (time_t)-1)
		{
			timePoint = std::chrono::system_clock::from_time_t(tt);
		}
	}

	return changed;
}

} // namespace ImGui
