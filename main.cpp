#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <fstream>
#include <vector>
#include <string>
#include <regex>
#include <filesystem>
#include <sstream>

namespace std
{
	namespace fs = filesystem;
}

struct BackupProject 
{
    std::wstring				title;
    std::wstring				rootPath;
	std::wstring				backupPath;
	std::vector<std::wstring>	includePaths;
	std::vector<std::wstring>	excludePaths;

	std::vector<std::wregex>	includeRegex;
	std::vector<std::wregex>	excludeRegex;

	void SetIncludeFilter(const std::wstring& filter);
	void SetExcludeFilter(const std::wstring& filter);
	void UpdateRegex();
	bool MatchesFilters(const std::wstring& filename) const;
};

HINSTANCE					g_hInst;
HWND						g_hWND;
HWND						g_hTreeView;
HWND						g_hPropertyViewLabels;
HWND						g_hPropertyViewValues;
HWND						g_hFileView;
HWND						g_hHorizontalSplitter;
HWND						g_hVerticalSplitter;
std::vector<BackupProject>	g_backupProjects;
float						g_ProjectViewWidthF = 0.5f;
float						g_ProjectViewHeightF = 0.75f;
bool						g_dragSplitterX = false;
bool						g_dragSplitterY = false;
int							g_splitterHalfWidth = 5;
BackupProject*				g_currentProject = nullptr;
bool						g_pauseTreeViewSelections = false;

enum UILabels
{
	UI_Title,
	UI_RootPath,
	UI_BackupPath,
	UI_Include,
	UI_Exclude,
};

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void InitUI();
void UpdateLayout();
void PopulateProjectsTreeView();
void PopulateTreeNode(HTREEITEM parent, const std::wstring& path, const BackupProject& project);
void ShowProjectDetails();
void LoadSettings();
void SaveSettings();
std::wstring JoinPaths(const std::vector<std::wstring>& paths);
void SplitPaths(const std::wstring& str, std::vector<std::wstring>& paths);
bool LoadBackupProject(const std::wstring& line, BackupProject& project);
std::wstring SaveBackupProject(const BackupProject& project);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    g_hInst = hInstance;
    
	WNDCLASS wc = {};
	wc.lpfnWndProc		= WndProc;
	wc.hInstance		= g_hInst;
	wc.lpszClassName	= L"BackupBrowser";
	wc.hbrBackground	= (HBRUSH)GetStockObject(DKGRAY_BRUSH);
	RegisterClass(&wc);
    
	g_hWND = CreateWindow(	L"BackupBrowser", 
							L"Backup Browser", 
							WS_OVERLAPPEDWINDOW,
							CW_USEDEFAULT, CW_USEDEFAULT, 
							800, 600, 
							nullptr, 
							nullptr, 
							g_hInst, 
							nullptr);
    
	::ShowWindow(g_hWND, nCmdShow);
	::UpdateWindow(g_hWND);
    
	MSG msg;
    
	while (GetMessage(&msg, nullptr, 0, 0)) 
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return (int)msg.wParam;
}

void InitUI() 
{
	INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES };

	InitCommonControlsEx(&icex);
    
	g_hTreeView = CreateWindowEx(0, WC_TREEVIEW, L"", WS_VISIBLE | WS_CHILD | WS_BORDER | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS, 0, 0, 300, 600, g_hWND, (HMENU)1, g_hInst, nullptr);
    
	g_hPropertyViewLabels = CreateWindowEx(0, WC_LISTVIEW, L"", WS_VISIBLE | WS_CHILD | WS_BORDER | LVS_REPORT, 300, 0, 500, 600, g_hWND, (HMENU)2, g_hInst, nullptr);
	g_hPropertyViewValues = CreateWindowEx(0, WC_LISTVIEW, L"", WS_VISIBLE | WS_CHILD | WS_BORDER | LVS_REPORT | LVS_EDITLABELS, 300, 0, 500, 600, g_hWND, (HMENU)2, g_hInst, nullptr);
	{
		ListView_SetExtendedListViewStyle(g_hPropertyViewLabels, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
		ListView_SetExtendedListViewStyle(g_hPropertyViewValues, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

		LVCOLUMN lvc = { LVCF_TEXT | LVCF_WIDTH, 0, 0, L"" };

		lvc.cx = 120;
		lvc.pszText = L"Property";
		ListView_InsertColumn(g_hPropertyViewLabels, 0, &lvc);

		lvc.cx = 350;
		lvc.pszText = L"Value";
		ListView_InsertColumn(g_hPropertyViewValues, 0, &lvc);
	}

	g_hFileView = CreateWindowEx(0, WC_LISTVIEW, L"", WS_VISIBLE | WS_CHILD | WS_BORDER | LVS_REPORT, 300, 0, 500, 600, g_hWND, (HMENU)3, g_hInst, nullptr);
	{
		ListView_SetExtendedListViewStyle(g_hFileView, LVS_EX_FULLROWSELECT);

		LVCOLUMN lvc = { LVCF_TEXT | LVCF_WIDTH, 0, 0, L"" };

		lvc.cx = 150;
		lvc.pszText = L"Backup Date";
		ListView_InsertColumn(g_hFileView, 0, &lvc);

		lvc.cx = 350;
		lvc.pszText = L"Changes";
		ListView_InsertColumn(g_hFileView, 1, &lvc);
	}

	g_hVerticalSplitter = CreateWindowEx(0, WC_STATIC, L"", WS_VISIBLE | WS_CHILD, 300, 0, 500, 600, g_hWND, (HMENU)4, g_hInst, nullptr);
	g_hHorizontalSplitter = CreateWindowEx(0, WC_STATIC, L"", WS_VISIBLE | WS_CHILD, 300, 0, 500, 600, g_hWND, (HMENU)4, g_hInst, nullptr);

	UpdateLayout();
}

void UpdateLayout()
{
	RECT clientRect;
	::GetClientRect(g_hWND, &clientRect);

	uint32_t windowWidth = clientRect.right - clientRect.left;
	uint32_t windowHeight = clientRect.bottom - clientRect.top;

	MoveWindow(g_hTreeView,		0, 0, (int)(windowWidth * g_ProjectViewWidthF-g_splitterHalfWidth), (int)(windowHeight * g_ProjectViewHeightF - g_splitterHalfWidth), FALSE);
	MoveWindow(g_hPropertyViewLabels, 0, (int)(windowHeight * g_ProjectViewHeightF + g_splitterHalfWidth), 120, (int)(windowHeight * (1.0f - g_ProjectViewHeightF) - g_splitterHalfWidth), FALSE);
	MoveWindow(g_hPropertyViewValues, 120, (int)(windowHeight * g_ProjectViewHeightF + g_splitterHalfWidth), (int)((windowWidth * g_ProjectViewWidthF - g_splitterHalfWidth) - 120), (int)(windowHeight * (1.0f - g_ProjectViewHeightF) - g_splitterHalfWidth), FALSE);
	MoveWindow(g_hFileView, (int)(windowWidth * g_ProjectViewWidthF + g_splitterHalfWidth), 0, (int)(windowWidth * (1.0f - g_ProjectViewWidthF) - g_splitterHalfWidth), windowHeight, FALSE);
	MoveWindow(g_hHorizontalSplitter, 0, (int)(windowHeight * g_ProjectViewHeightF - g_splitterHalfWidth), (int)(windowWidth * g_ProjectViewWidthF - g_splitterHalfWidth), g_splitterHalfWidth*2, FALSE);
	MoveWindow(g_hVerticalSplitter, (int)(windowWidth * g_ProjectViewWidthF - g_splitterHalfWidth), 0, g_splitterHalfWidth*2, windowHeight, FALSE);

	InvalidateRect(g_hWND, NULL, FALSE);

	UpdateWindow(g_hWND);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) 
{
	switch (message) 
	{
		case WM_CREATE:
			g_hWND = hWnd;
			InitUI();
			LoadSettings();
			PopulateProjectsTreeView();
			ShowProjectDetails();
			break;

		case WM_CLOSE:
			SaveSettings();
			DestroyWindow(hWnd);
			break;

		case WM_DESTROY:
			PostQuitMessage(0);
			break;

		case WM_SIZE:
		{
			UpdateLayout();
			break;
		}

		case WM_NCHITTEST:
		{
			int mouseX = LOWORD(lParam);
			int mouseY = HIWORD(lParam);

			RECT clientRect;
			::GetClientRect(g_hWND, &clientRect);

			uint32_t windowWidth = clientRect.right - clientRect.left;
			uint32_t windowHeight = clientRect.bottom - clientRect.top;

			if (mouseX >= (int)(windowWidth * g_ProjectViewWidthF - g_splitterHalfWidth) && mouseX <= (int)(windowWidth * g_ProjectViewWidthF + g_splitterHalfWidth) )
			{
				return HTTRANSPARENT;
			}
			
			if (mouseX <= (int)(windowWidth * g_ProjectViewWidthF + g_splitterHalfWidth) && mouseY >= (int)(windowHeight * g_ProjectViewHeightF - g_splitterHalfWidth) && mouseY <= (int)(windowHeight * g_ProjectViewHeightF + g_splitterHalfWidth) )
			{
				return HTTRANSPARENT;
			}

			return DefWindowProc(hWnd, message, wParam, lParam);
		}

		case WM_LBUTTONDOWN:
		{
			int mouseX = LOWORD(lParam);
			int mouseY = HIWORD(lParam);

			RECT clientRect;
			::GetClientRect(g_hWND, &clientRect);

			uint32_t windowWidth = clientRect.right - clientRect.left;
			uint32_t windowHeight = clientRect.bottom - clientRect.top;

			if (mouseX >= (int)(windowWidth * g_ProjectViewWidthF - g_splitterHalfWidth) && mouseX <= (int)(windowWidth * g_ProjectViewWidthF + g_splitterHalfWidth) )
			{
				g_dragSplitterX = true;
				SetCapture(g_hWND);
			}
			
			if (mouseX <= (int)(windowWidth * g_ProjectViewWidthF + g_splitterHalfWidth) && mouseY >= (int)(windowHeight * g_ProjectViewHeightF - g_splitterHalfWidth) && mouseY <= (int)(windowHeight * g_ProjectViewHeightF + g_splitterHalfWidth) )
			{
				g_dragSplitterY = true;
				SetCapture(g_hWND);
			}

			return 0;
		}

		case WM_MOUSEMOVE:
		{
			int mouseX = LOWORD(lParam);
			int mouseY = HIWORD(lParam);

			RECT clientRect;
			::GetClientRect(g_hWND, &clientRect);

			uint32_t windowWidth = clientRect.right - clientRect.left;
			uint32_t windowHeight = clientRect.bottom - clientRect.top;

			if (g_dragSplitterX)
			{
				g_ProjectViewWidthF = (float)(mouseX - clientRect.left) / float(clientRect.right - clientRect.left);

				g_ProjectViewWidthF = std::max(0.1f, std::min(0.9f, g_ProjectViewWidthF));

				UpdateLayout();
			}
			
			if (g_dragSplitterY)
			{
				g_ProjectViewHeightF = (float)(mouseY - clientRect.top) / float(clientRect.bottom - clientRect.top);

				g_ProjectViewHeightF = std::max(0.1f, std::min(0.9f, g_ProjectViewHeightF));

				UpdateLayout();
			}

			bool overX = (mouseX >= (int)(windowWidth * g_ProjectViewWidthF - g_splitterHalfWidth) && mouseX <= (int)(windowWidth * g_ProjectViewWidthF + g_splitterHalfWidth) );
			bool overY = (mouseX <= (int)(windowWidth * g_ProjectViewWidthF + g_splitterHalfWidth) && mouseY >= (int)(windowHeight * g_ProjectViewHeightF - g_splitterHalfWidth) && mouseY <= (int)(windowHeight * g_ProjectViewHeightF + g_splitterHalfWidth) );

			if (overX && overY)
			{
				SetCursor(LoadCursor(NULL, IDC_SIZEALL));
			}
			else if (overX)
			{
				SetCursor(LoadCursor(NULL, IDC_SIZEWE));
			}
			else if (overY)
			{
				SetCursor(LoadCursor(NULL, IDC_SIZENS));
			}
			else
			{
				SetCursor(LoadCursor(NULL, IDC_ARROW));
			}

			return 0;
		}

		case WM_LBUTTONUP:
		{
			int mouseX = LOWORD(lParam);
			int mouseY = HIWORD(lParam);

			if (g_dragSplitterX || g_dragSplitterY)
			{
				g_dragSplitterX = false;
				g_dragSplitterY = false;
				ReleaseCapture();
			}

			return 0;
		}

		case WM_NOTIFY:
		{
			LPNMHDR pNotifyHeader = (LPNMHDR)lParam;

			if (pNotifyHeader->hwndFrom == g_hTreeView && pNotifyHeader->code == TVN_SELCHANGED)
			{
				if (!g_pauseTreeViewSelections)
				{
					LPNMTREEVIEW pNMTV = (LPNMTREEVIEW)lParam;

					uint32_t projectIndex = (int)pNMTV->itemNew.lParam;

					if (projectIndex < g_backupProjects.size())
					{
						g_currentProject = &g_backupProjects[projectIndex];
						ShowProjectDetails();
					}
				}
			}
			else if (pNotifyHeader->hwndFrom == g_hPropertyViewValues)
			{
				if (pNotifyHeader->code == NM_DBLCLK)
				{
					NMLISTVIEW* pnmv = (NMLISTVIEW*)lParam;
					ListView_EditLabel(g_hPropertyViewValues, pnmv->iItem);
				}
				else if (pNotifyHeader->code == LVN_ENDLABELEDIT)
				{
					NMLVDISPINFO* pDispInfo = (NMLVDISPINFO*)lParam;

					if (pDispInfo->item.pszText)
					{
						ListView_SetItemText(g_hPropertyViewValues, pDispInfo->item.iItem, 0, pDispInfo->item.pszText);

						switch (pDispInfo->item.iItem)
						{
							case UI_Title:
							{ 
								g_currentProject->title = pDispInfo->item.pszText;
								ListView_SetItemText(g_hPropertyViewValues, pDispInfo->item.iItem, 0, (LPWSTR)g_currentProject->title.c_str());
								break;
							}

							case UI_RootPath:
							{
								g_currentProject->rootPath = pDispInfo->item.pszText;
								ListView_SetItemText(g_hPropertyViewValues, pDispInfo->item.iItem, 0, (LPWSTR)g_currentProject->rootPath.c_str());		
								break;
							}

							case UI_BackupPath:
							{
								g_currentProject->backupPath = pDispInfo->item.pszText;
								ListView_SetItemText(g_hPropertyViewValues, pDispInfo->item.iItem, 0, (LPWSTR)g_currentProject->backupPath.c_str());
								break;
							}

							case UI_Include:
							{ 
								g_currentProject->SetIncludeFilter(pDispInfo->item.pszText); 
								std::wstring temp = JoinPaths(g_currentProject->includePaths); 
								ListView_SetItemText(g_hPropertyViewValues, pDispInfo->item.iItem, 0, (LPWSTR)temp.c_str()); 
								PopulateProjectsTreeView();
								break;
							}

							case UI_Exclude:
							{
								g_currentProject->SetExcludeFilter(pDispInfo->item.pszText);
								std::wstring temp = JoinPaths(g_currentProject->excludePaths);
								ListView_SetItemText(g_hPropertyViewValues, pDispInfo->item.iItem, 0, (LPWSTR)temp.c_str());
								PopulateProjectsTreeView();
								break;
							}
						}
					}
				}
			}

			break;
		}

		case WM_KEYUP:
		{
			switch (wParam)
			{
				case VK_F5:
				{
					PopulateProjectsTreeView();
					ShowProjectDetails();
					break;
				}
			}

			break;
		}

		default:
			return DefWindowProc(hWnd, message, wParam, lParam);
	}

	return 0;
}

void PopulateProjectsTreeView()
{
	SendMessage(g_hTreeView, WM_SETREDRAW, FALSE, 0);
	g_pauseTreeViewSelections = true;

	TreeView_DeleteAllItems(g_hTreeView);

	HTREEITEM currentProjectTreeItem = NULL;

	for (unsigned i=0; i!=g_backupProjects.size(); ++i) 
	{
		const BackupProject& project = g_backupProjects[i];

		WCHAR buffer[256];
		swprintf_s(buffer, 256, L"%s [%s]", project.title.c_str(), project.rootPath.c_str());

		TVINSERTSTRUCT tvis = {};
		tvis.hParent		= TVI_ROOT;
		tvis.hInsertAfter	= TVI_LAST;
		tvis.item.mask		= TVIF_TEXT | TVIF_PARAM;
		tvis.item.pszText	= buffer;
		tvis.item.lParam	= i;

		HTREEITEM hTreeItem = TreeView_InsertItem(g_hTreeView, &tvis);

		PopulateTreeNode(hTreeItem, project.rootPath, project);

		if (g_currentProject == &project)
		{
			currentProjectTreeItem = hTreeItem;
		}
	}

	TreeView_Expand(g_hTreeView, currentProjectTreeItem, TVE_EXPAND);
	TreeView_EnsureVisible(g_hTreeView, currentProjectTreeItem);
	SendMessage(g_hTreeView, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(g_hTreeView, NULL, TRUE);

	g_pauseTreeViewSelections = false;

	TreeView_SelectItem(g_hTreeView, currentProjectTreeItem);
}

void PopulateTreeNode(HTREEITEM parent, const std::wstring& path, const BackupProject& project)
{
	if (!std::fs::exists(path))
	{
		TVINSERTSTRUCT tvis = {};
		tvis.hParent		= parent;
		tvis.hInsertAfter	= TVI_LAST;
		tvis.item.mask		= TVIF_TEXT | TVIF_PARAM;
		tvis.item.pszText	= L"Missing Path!";
		HTREEITEM hTreeItem = TreeView_InsertItem(g_hTreeView, &tvis);

		return;
	}

	for (const std::fs::directory_entry& entry : std::fs::directory_iterator(path))
	{
		std::fs::path relativePath = std::fs::relative(entry.path(), path);

		if (!project.MatchesFilters(relativePath))
			continue;

		WCHAR buffer[256];
		swprintf_s(buffer, 256, L"%s", relativePath.c_str());

		TVINSERTSTRUCT tvis = {};
		tvis.hParent		= parent;
		tvis.hInsertAfter	= TVI_LAST;
		tvis.item.mask		= TVIF_TEXT | TVIF_PARAM;
		tvis.item.pszText	= buffer;

		HTREEITEM hTreeItem = TreeView_InsertItem(g_hTreeView, &tvis);

		//DirEntry dirEntry;

		//dirEntry.path			= FromAbsolutePath(entry.path());
		//dirEntry.isDirectory	= entry.is_directory();
		//dirEntry.fileSize		= entry.file_size();
		//dirEntry.lastWriteTime	= entry.last_write_time();

		//m_currentFolderContents.push_back(dirEntry);
	}
}

void ShowProjectDetails()
{
	ListView_DeleteAllItems(g_hPropertyViewLabels);
	ListView_DeleteAllItems(g_hPropertyViewValues);
    
	if (g_currentProject == nullptr)
		return;

	LVITEM lvi = {};
	lvi.mask = LVIF_TEXT;
    
	lvi.iItem = UI_Title;
	lvi.iSubItem = 0;
	{
		lvi.pszText = L"Title";
		ListView_InsertItem(g_hPropertyViewLabels, &lvi);

		lvi.pszText = (LPWSTR)g_currentProject->title.c_str();
		ListView_InsertItem(g_hPropertyViewValues, &lvi);
	}
    
	lvi.iItem = UI_RootPath;
	{
		lvi.pszText = L"Root Path";
		ListView_InsertItem(g_hPropertyViewLabels, &lvi);
	
		lvi.pszText = (LPWSTR)g_currentProject->rootPath.c_str();
		ListView_InsertItem(g_hPropertyViewValues, &lvi);
	}
    
	lvi.iItem = UI_BackupPath;
	{
		lvi.pszText = L"Backup Path";
		ListView_InsertItem(g_hPropertyViewLabels, &lvi);

		lvi.pszText = (LPWSTR)g_currentProject->backupPath.c_str();
		ListView_InsertItem(g_hPropertyViewValues, &lvi);
	}

	lvi.iItem = UI_Include;
	{
		lvi.pszText = L"Include";
		ListView_InsertItem(g_hPropertyViewLabels, &lvi);

		std::wstring temp = JoinPaths(g_currentProject->includePaths);
		lvi.pszText = (LPWSTR)temp.c_str();
		ListView_InsertItem(g_hPropertyViewValues, &lvi);
	}

	lvi.iItem = UI_Exclude;
	{
		lvi.pszText = L"Exclude";
		ListView_InsertItem(g_hPropertyViewLabels, &lvi);

		std::wstring temp = JoinPaths(g_currentProject->excludePaths);
		lvi.pszText = (LPWSTR)temp.c_str();
		ListView_InsertItem(g_hPropertyViewValues, &lvi);
	}
}

void BackupProject::SetIncludeFilter(const std::wstring& filter)
{
	includePaths.clear();

	SplitPaths(filter, includePaths);

	UpdateRegex(); 
}

void BackupProject::SetExcludeFilter(const std::wstring& filter)
{
	excludePaths.clear();
	
	SplitPaths(filter, excludePaths);

	UpdateRegex(); 
}

void BackupProject::UpdateRegex()
{
	auto WildcardToRegex = [](const std::wstring& wildcard)->std::wregex 
	{
		std::wstring regexPattern;

		for (wchar_t c : wildcard)
		{
			switch (c)
			{
				case L'*':	regexPattern += L".*";		break;	// Convert * to .*
				case L'?':	regexPattern += L".";		break;	// Convert ? to .
				case L'\\': regexPattern += L"\\\\\\";	break;	// Escape backslashes
				case L'.':	regexPattern += L"\\.";		break;	// Escape dots
				default:	regexPattern += c;			break;
			}
		}

		//// Match end of string
		//regexPattern += L"$";

		return std::wregex(regexPattern, std::regex_constants::icase);
	};

	includeRegex.clear();
	for (const std::wstring& str : includePaths)
	{
		includeRegex.push_back( WildcardToRegex(str) );
	}

	excludeRegex.clear();
	for (const std::wstring& str : excludePaths)
	{
		excludeRegex.push_back( WildcardToRegex(str) );
	}
}

bool BackupProject::MatchesFilters(const std::wstring& filename) const
{
	bool included = false;

	for (const auto& filter : includeRegex)
	{
		if (std::regex_match(filename, filter))
		{
			included = true;
			break;
		}
	}

	if (included)
	{
		for (const auto& filter : excludeRegex)
		{
			if (std::regex_match(filename, filter))
			{
				included = false;
				break;
			}
		}
	}

	return included;
}

std::wstring JoinPaths(const std::vector<std::wstring>& paths) 
{
	std::wstringstream ss;

	for (size_t i = 0; i < paths.size(); ++i) 
	{
		if (i > 0) ss << L";";
		ss << paths[i];
	}

	return ss.str();
}

void SplitPaths(const std::wstring& str, std::vector<std::wstring>& paths) 
{
	const std::wstring delimiters = L";, ";

	auto AddToken = [&](const std::wstring& token)
	{
		if (token.empty())
			return;

		size_t start = 0;
		size_t end = token.size();

		while (iswspace(token[start]) && start < end)
		{
			++start;
		}

		while (iswspace(token[end-1]) && end>start)
		{
			--end;
		}

		if (end>start) 
		{
			paths.push_back( token.substr(start, end - start) );
		}
	};

	size_t start = 0;
	size_t end = str.find_first_of(delimiters, start);

	while (end != std::string::npos)
	{
		// Avoid empty tokens
		if (end > start) 
		{
			AddToken(str.substr(start, end - start));
		}

		start = end + 1;
		end = str.find_first_of(delimiters, start);
	}

	if (start < str.size()) 
	{
		AddToken(str.substr(start));
	}
}

void LoadSettings() 
{
	std::wifstream file(L"backup_settings.txt");

	if (g_backupProjects.empty())
	{
		g_backupProjects.push_back({L"Test", L"C:\\temp", L"c:\\backup", {L"*.txt"}, {L""}});
		g_backupProjects.back().UpdateRegex();
		g_backupProjects.push_back({L"Test2", L"C:\\temp1", L"c:\\backup", {L"*.txt, *.jpg"}, {L"temp*"}});
		g_backupProjects.back().UpdateRegex();
		g_currentProject = &g_backupProjects[0];
		ShowProjectDetails();
	}

	if (!file)
		return;
    
	g_backupProjects.clear();

	// Window placement
	{
		std::wstring line;
		std::getline(file, line);

		std::wregex pattern(L"\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*([-+]?\\d*\\.?\\d+)\\s*,\\s*([-+]?\\d*\\.?\\d+)\\s*");
		std::wsmatch match;

		if (std::regex_match(line, match, pattern) && match.size() == 8)
		{
			int left = std::stoi(match[1].str());
			int right = std::stoi(match[2].str());
			int top = std::stoi(match[3].str());
			int bottom = std::stoi(match[4].str());
			int showCmd = std::stoi(match[5].str());
			g_ProjectViewWidthF = std::stof(match[6].str());
			g_ProjectViewHeightF = std::stof(match[7].str());

			::SetWindowPos(g_hWND, NULL, left, top, right - left, bottom - top, SWP_NOZORDER);
			::ShowWindow(g_hWND, showCmd);
		}
	}

	// Load projects
	{
		BackupProject project;

		std::wstring line;
		while (std::getline(file, line)) 
		{
			if (line.empty())
				continue;
        
			if (LoadBackupProject(line, project))
			{
				g_backupProjects.push_back(project);
			}
		}
	}

	file.close();

	if (!g_backupProjects.empty())
	{
		g_currentProject = &g_backupProjects[0];
		ShowProjectDetails();
	}
}

void SaveSettings() 
{
	std::wofstream file(L"backup_settings.txt");

	WINDOWPLACEMENT wp;
	wp.length = sizeof(WINDOWPLACEMENT);

	::GetWindowPlacement(g_hWND, &wp);
	RECT windowRect = wp.rcNormalPosition;

	file << windowRect.left << L", " << windowRect.right << L", " << windowRect.top << L", " << windowRect.bottom << L", " << wp.showCmd << L", " << g_ProjectViewWidthF << L", " << g_ProjectViewHeightF << L"\n";

	for (const BackupProject& project : g_backupProjects) 
	{
		file << SaveBackupProject(project) << "\n";
	}

	file.close();
}

std::wstring SaveBackupProject(const BackupProject& project)
{
	std::wostringstream os;

	os	<< project.title << L"|" 
		<< project.rootPath << L"|"
		<< project.backupPath << L"|"
		<< JoinPaths(project.includePaths) << L"|"
		<< JoinPaths(project.excludePaths) << L"|";

	return os.str();
}

bool LoadBackupProject(const std::wstring& line, BackupProject& project) 
{
	std::wstringstream ss(line);
	std::wstring token;
	std::vector<std::wstring> tokens;

	// Split the line by '|'
	while (std::getline(ss, token, L'|'))
	{
		tokens.push_back(token);
	}

	if (tokens.size() < 5) return false; // Ensure all fields exist

	project.title = tokens[0];
	project.rootPath = tokens[1];
	project.backupPath = tokens[2];
	project.SetIncludeFilter(tokens[3]);
	project.SetExcludeFilter(tokens[4]);

	project.UpdateRegex();

	return true;
}
