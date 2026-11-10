; ==============================================================================
; EyeCam Studio — Automated Windows 11 x64 Installer (Inno Setup 6)
; Automatically installs EyeCam Studio GUI, registers DirectShow Virtual Camera
; filter (EyeCamFilter.dll), and configures Windows Defender Firewall rules.
; ==============================================================================

#define MyAppName "EyeCam Studio"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "EyeCam Open-Source Project"
#define MyAppURL "https://github.com/Gladiator666Satan/EyeCam"
#define MyAppExeName "EyeCamStudio.exe"
#define MyFilterDllName "EyeCamFilter.dll"

[Setup]
AppId={{5C9D017A-C081-4FE7-99E4-500D06474F29}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\EyeCam
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\build\installer
OutputBaseFilename=EyeCam-Setup-x64
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
DisableWelcomePage=no
UninstallDisplayIcon={app}\{#MyAppExeName}
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "ukrainian"; MessagesFile: "compiler:Languages\Ukrainian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "firewall"; Description: "Configure Windows Firewall rules for Wi-Fi camera streaming (UDP 8555 / TCP 8554)"; GroupDescription: "System Integration:"

[Files]
Source: "..\build\bin\Release\EyeCamStudio.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\Release\EyeCamFilter.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\Release\windows_test.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\scripts\windows\*"; DestDir: "{app}\scripts"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "Launch EyeCam Studio (Virtual Camera & Wireless Receiver)"
Name: "{group}\Re-register Virtual Camera Driver"; Filename: "{sys}\regsvr32.exe"; Parameters: """{app}\{#MyFilterDllName}"""; Comment: "Register DirectShow Virtual Camera Filter"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon; Comment: "EyeCam Studio — Turn your smartphone into a pro PC webcam"

[Run]
; 1. Automatically register DirectShow Virtual Camera Filter (EyeCamFilter.dll)
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\{#MyFilterDllName}"""; StatusMsg: "Registering EyeCam DirectShow Virtual Camera Driver..."; Flags: runhidden waituntilterminated

; 2. Configure Windows Defender Firewall rules for UDP 8555 (Video Stream) and TCP 8554 (ISP Control)
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""EyeCam Studio UDP 8555"""; Flags: runhidden waituntilterminated; Tasks: firewall
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""EyeCam Studio UDP 8555"" dir=in action=allow protocol=UDP localport=8555"; StatusMsg: "Configuring Windows Firewall for UDP video streaming..."; Flags: runhidden waituntilterminated; Tasks: firewall
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""EyeCam Studio TCP 8554"""; Flags: runhidden waituntilterminated; Tasks: firewall
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""EyeCam Studio TCP 8554"" dir=out action=allow protocol=TCP remoteport=8554"; Flags: runhidden waituntilterminated; Tasks: firewall

; 3. Offer to launch EyeCam Studio immediately after installation
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Cleanly unregister DirectShow Virtual Camera Filter & remove Firewall rules on uninstall
Filename: "{sys}\regsvr32.exe"; Parameters: "/u /s ""{app}\{#MyFilterDllName}"""; RunOnceId: "UnregisterEyeCamFilter"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""EyeCam Studio UDP 8555"""; RunOnceId: "DeleteFirewallUDP"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""EyeCam Studio TCP 8554"""; RunOnceId: "DeleteFirewallTCP"; Flags: runhidden waituntilterminated
