; AdbTools installer script (Inno Setup 6)
;
; Build the portable files first (adb_browser.exe + assets/ + scrcpy/), then run:
;   iscc installer.iss
; or override the version from CI:
;   iscc installer.iss /DMyAppVersion=X.Y.Z   (the CI passes the tag version here)

#ifndef MyAppVersion
  #define MyAppVersion "0.10.0"
#endif

#define MyAppName "ADB 文件浏览器"
#define MyAppPublisher "joffeego"
#define MyAppExeName "adb_browser.exe"

[Setup]
; A stable GUID: keep it unchanged across releases so upgrades replace cleanly.
AppId={{B7A2C9E4-1D5F-4A3B-8C6E-0F9D2A5B7C1E}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
; Per-user install (no admin needed); the app self-updates its own files, so it
; must live in a user-writable directory rather than Program Files.
DefaultDirName={localappdata}\Programs\AdbTools
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=dist
OutputBaseFilename=AdbTools-Setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}
; The installer can be Authenticode-signed after compilation (SignPath in CI,
; see .github/workflows/release.yml). Signing is currently NOT enabled, so
; released installers are unsigned.
;
; The uninstaller is deliberately NOT signed: Inno Setup can only embed a
; signature into unins000.exe by running a local SignTool, or by embedding a
; pre-signed uninst***.exe from SignedUninstallerDir - a cloud signing service
; cannot do either during the build. Leaving SignedUninstaller at its default
; (yes whenever a SignTool is set) would make iscc abort and ask for a manual
; signature, so it is disabled explicitly. Consequence: unins000.exe stays
; unsigned and shows "unknown publisher" when a user runs the uninstaller.
SignedUninstaller=no

[Languages]
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "build\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\assets\*";       DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "build\scrcpy\*";       DestDir: "{app}\scrcpy"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "LICENSE";              DestDir: "{app}"; Flags: ignoreversion
Source: "THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#MyAppName}";  Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
