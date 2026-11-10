; ==============================================================================
; EyeCam Windows Studio & DirectShow Virtual Camera Inno Setup Script
; ==============================================================================

#define MyAppName "EyeCam Studio"
#define MyAppVersion "1.2.3"
#define MyAppPublisher "EyeCam Team"
#define MyAppURL "https://github.com/Gladiator666Satan/EyeCam"
#define MyAppExeName "EyeCamStudio.exe"

[Setup]
AppId={{5E4A3B2C-1D0F-4E9A-8B7C-6D5E4F3A2B1C}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\EyeCam
DefaultGroupName=EyeCam
DisableProgramGroupPage=yes
LicenseFile=
OutputDir=..\build\installer
OutputBaseFilename=EyeCam-Setup-x64
SetupIconFile=..\src\windows\gui\eyecam_pc.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=admin

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "ukrainian"; MessagesFile: "compiler:Languages\Ukrainian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "startupicon"; Description: "Запускати EyeCam Studio автоматично при старті Windows"; GroupDescription: "Автозавантаження:"; Flags: unchecked

[Files]
; Виконуваний файл десктопного Studio додатка
Source: "..\build\bin\Release\EyeCamStudio.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\Release\EyeCamFilter.dll"; DestDir: "{app}"; Flags: ignoreversion regserver
Source: "..\build\bin\Release\windows_test.exe"; DestDir: "{app}"; Flags: ignoreversion

; Вбудований мобільний міст ADB
Source: "..\build\adb\adb.exe"; DestDir: "{app}\adb"; Flags: ignoreversion
Source: "..\build\adb\AdbWinApi.dll"; DestDir: "{app}\adb"; Flags: ignoreversion
Source: "..\build\adb\AdbWinUsbApi.dll"; DestDir: "{app}\adb"; Flags: ignoreversion

; Скрипти реєстрації DirectShow драйвера
Source: "..\scripts\windows\register_filter.bat"; DestDir: "{app}\scripts"; Flags: ignoreversion
Source: "..\scripts\windows\unregister_filter.bat"; DestDir: "{app}\scripts"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\{#MyAppExeName}"
Name: "{group}\Перереєструвати віртуальну камеру"; Filename: "{app}\scripts\register_filter.bat"; IconFilename: "{sys}\shell32.dll"; IconIndex: 167
Name: "{group}\Видалити реєстрацію камери"; Filename: "{app}\scripts\unregister_filter.bat"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
Name: "{userstartup}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: startupicon

[Run]
; Автоматична реєстрація DirectShow DLL як віртуальної вебкамери Windows
Filename: "regsvr32.exe"; Parameters: "/s ""{app}\EyeCamFilter.dll"""; StatusMsg: "Реєстрація системного DirectShow драйвера віртуальної камери EyeCam..."; Flags: runhidden
; Запуск Studio після встановлення
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Дереєстрація DirectShow фільтра перед видаленням файлів
Filename: "regsvr32.exe"; Parameters: "/u /s ""{app}\EyeCamFilter.dll"""; Flags: runhidden

[UninstallDelete]
Type: filesandordirs; Name: "{app}\adb"
Type: filesandordirs; Name: "{app}\scripts"
Type: filesandordirs; Name: "{app}"
