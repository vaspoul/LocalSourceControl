#ifndef SETTINGS_H
#define SETTINGS_H

struct Settings
{
	int				winX = CW_USEDEFAULT;
	int				winY = CW_USEDEFAULT;
	int				winW = 1280;
	int				winH = 720;

	std::wstring	backupRoot = L"";
	uint32_t		maxBackupSizeMB = 10240;
	uint32_t		maxBackupsPerFile = 50;

	std::vector<WatchedFolder> watched;
};

extern Settings	g_settings;

void			LoadSettings();
void			SaveSettings();

void			MarkSettingsDirty();
void			MaybeSaveSettingsThrottled();

#endif // SETTINGS_H

