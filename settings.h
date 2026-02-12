#ifndef SETTINGS_H
#define SETTINGS_H

struct Settings
{
	int				winX = CW_USEDEFAULT;
	int				winY = CW_USEDEFAULT;
	int				winW = 1280;
	int				winH = 720;

	std::wstring	backupRoot = L"";
	uint32_t		maxBackupSizeMB = 1024*10;
	uint32_t		maxBackupsPerFile = 256;
	std::wstring	diffToolPath;
	bool			minimizeOnClose = true;

	std::vector<WatchedFolder> watched;
};

extern Settings	g_settings;

void			LoadSettings();
void			SaveSettings();

void			MarkSettingsDirty();
void			MaybeSaveSettingsThrottled();

#endif // SETTINGS_H

