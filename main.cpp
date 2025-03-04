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

namespace std
{
	namespace fs = filesystem;
}

struct BackupProject 
{
    std::wstring	title;
    std::wstring	rootPath;
	std::wstring	backupPath;
	std::wstring	includePaths;
	std::wstring	excludePaths;
    bool			recurseSubfolders;
};

HINSTANCE					g_hInst;
HWND						g_hWND;
HWND						g_hTreeView;
HWND						g_hPropertyView;
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

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

void InitUI();
void UpdateLayout();
void PopulateProjectsTreeView();
void PopulateTreeNode(HTREEITEM parent, const std::wstring& path);
void ShowProjectDetails();
void LoadSettings();
void SaveSettings();

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
    
	g_hPropertyView = CreateWindowEx(0, WC_LISTVIEW, L"", WS_VISIBLE | WS_CHILD | WS_BORDER | LVS_REPORT, 300, 0, 500, 600, g_hWND, (HMENU)2, g_hInst, nullptr);
	{
		ListView_SetExtendedListViewStyle(g_hPropertyView, LVS_EX_FULLROWSELECT);

		LVCOLUMN lvc = { LVCF_TEXT | LVCF_WIDTH, 0, 0, L"" };

		lvc.cx = 150;
		lvc.pszText = L"Property";
		ListView_InsertColumn(g_hPropertyView, 0, &lvc);

		lvc.cx = 350;
		lvc.pszText = L"Value";
		ListView_InsertColumn(g_hPropertyView, 1, &lvc);
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
	MoveWindow(g_hPropertyView, 0, (int)(windowHeight * g_ProjectViewHeightF + g_splitterHalfWidth), (int)(windowWidth * g_ProjectViewWidthF - g_splitterHalfWidth), (int)(windowHeight * (1.0f - g_ProjectViewHeightF) - g_splitterHalfWidth), FALSE);
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
			LPNMHDR pNMHDR = (LPNMHDR)lParam;

			if (pNMHDR->hwndFrom == g_hTreeView && pNMHDR->code == TVN_SELCHANGED)
			{
				LPNMTREEVIEW pNMTV = (LPNMTREEVIEW)lParam;

				uint32_t projectIndex = (int)pNMTV->itemNew.lParam;

				if (projectIndex < g_backupProjects.size())
				{
					g_currentProject = &g_backupProjects[projectIndex];
					ShowProjectDetails();
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

		PopulateTreeNode(hTreeItem, project.rootPath);
	}
}

void PopulateTreeNode(HTREEITEM parent, const std::wstring& path)
{
	if (!std::fs::exists(path))
	{
		TVINSERTSTRUCT tvis = {};
		tvis.hParent		= TVI_ROOT;
		tvis.hInsertAfter	= TVI_LAST;
		tvis.item.mask		= TVIF_TEXT | TVIF_PARAM;
		tvis.item.pszText	= L"Missing Path!";
		HTREEITEM hTreeItem = TreeView_InsertItem(g_hTreeView, &tvis);

		return;
	}

	for (const std::fs::directory_entry& entry : std::fs::directory_iterator(path))
	{
		std::fs::path relativePath = std::fs::relative(entry.path(), path);

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
	ListView_DeleteAllItems(g_hPropertyView);
    
	if (g_currentProject == nullptr)
		return;

	LVITEM lvi = {};
	lvi.mask = LVIF_TEXT;
    
	lvi.pszText = L"Title";
	lvi.iItem = 0;
	ListView_InsertItem(g_hPropertyView, &lvi);
	lvi.pszText = (LPWSTR)g_currentProject->title.c_str();
	lvi.iSubItem = 1;
	ListView_SetItem(g_hPropertyView, &lvi);
    
	lvi.pszText = L"Root Path";
	lvi.iItem = 1;
	lvi.iSubItem = 0;
	ListView_InsertItem(g_hPropertyView, &lvi);
	lvi.pszText = (LPWSTR)g_currentProject->rootPath.c_str();
	lvi.iSubItem = 1;
	ListView_SetItem(g_hPropertyView, &lvi);
    
	lvi.pszText = L"Backup Path";
	lvi.iItem = 1;
	lvi.iSubItem = 0;
	ListView_InsertItem(g_hPropertyView, &lvi);
	lvi.pszText = (LPWSTR)g_currentProject->backupPath.c_str();
	lvi.iSubItem = 1;
	ListView_SetItem(g_hPropertyView, &lvi);

	lvi.pszText = L"Include";
	lvi.iItem = 2;
	lvi.iSubItem = 0;
	ListView_InsertItem(g_hPropertyView, &lvi);
	lvi.pszText = (LPWSTR)g_currentProject->includePaths.c_str();
	lvi.iSubItem = 1;
	ListView_SetItem(g_hPropertyView, &lvi);

	lvi.pszText = L"Exclude";
	lvi.iItem = 3;
	lvi.iSubItem = 0;
	ListView_InsertItem(g_hPropertyView, &lvi);
	lvi.pszText = (LPWSTR)g_currentProject->excludePaths.c_str();
	lvi.iSubItem = 1;
	ListView_SetItem(g_hPropertyView, &lvi);

	lvi.pszText = L"Recurse Subfolders";
	lvi.iItem = 4;
	lvi.iSubItem = 0;
	ListView_InsertItem(g_hPropertyView, &lvi);
	lvi.pszText = g_currentProject->recurseSubfolders ? L"Yes" : L"No";
	lvi.iSubItem = 1;
	ListView_SetItem(g_hPropertyView, &lvi);
}

void LoadSettings() 
{
	std::wifstream file(L"backup_settings.txt");

	if (g_backupProjects.empty())
	{
		g_backupProjects.push_back({L"Test", L"C:\\temp", L"c:\\backup", L"*.txt", L"", true});
		g_backupProjects.push_back({L"Test2", L"C:\\temp1", L"c:\\backup", L"*.txt, *.jpg", L"temp*", true});
		g_currentProject = &g_backupProjects[0];
		ShowProjectDetails();
	}

	if (!file)
		return;
    
	g_backupProjects.clear();

	BackupProject project;
	std::wstring line;

	// Window placement
	{
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

	while (std::getline(file, line)) 
	{
		if (line.empty())
			continue;
        
		size_t pos1 = line.find('|');
		size_t pos2 = line.rfind('|');

		if (pos1 == std::wstring::npos || pos2 == pos1) 
			continue;
        
		project.title				= line.substr(0, pos1);
		project.rootPath			= line.substr(pos1 + 1, pos2 - pos1 - 1);
		project.recurseSubfolders	= (line.substr(pos2 + 1) == L"1");
        
		g_backupProjects.push_back(project);
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
		file << project.title << L"|" << project.rootPath << L"|" << (project.recurseSubfolders ? L"1" : L"0") << "\n";
	}

	file.close();
}
