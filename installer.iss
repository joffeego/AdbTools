; AdbTools installer script (Inno Setup 6)
;
; Build the portable files first (adb_browser.exe + assets/ + scrcpy/), then run:
;   iscc installer.iss
; or override the version from CI:
;   iscc installer.iss /DMyAppVersion=X.Y.Z   (the CI passes the tag version here)

#ifndef MyAppVersion
  #define MyAppVersion "0.10.12"
#endif

; VERSIONINFO wants four components; the release tag only has three.
#define MyAppVersionQuad MyAppVersion + ".0"

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

; The setup exe is unsigned, so its version resource is the only identity Windows
; (and a heuristic AV engine's "who wrote this?" pass) can read off the file: with
; no resource at all, Explorer shows an empty "File version" and the sample looks
; like a hand-rolled dropper. Inno writes the block below into the setup exe, and
; release.yml asserts the result, because nothing else can be verified about an
; unsigned installer.
;
; The Chinese product name is written out literally instead of as {#MyAppName}:
; ISPP's inline substitution goes through the build machine's ANSI code page, and
; on the CI runner (CP1252, Inno Setup 6.7.1) {#MyAppName} came out as
; "ADB ae ae...a" mojibake in the version resource of the v0.10.12 build, while the
; same text used literally (AppName, which Inno copied into ProductName in every
; earlier release) was correct. The file also carries a UTF-8 BOM now, so its
; encoding no longer depends on the code page of whichever machine compiles it.
VersionInfoVersion={#MyAppVersionQuad}
VersionInfoProductVersion={#MyAppVersionQuad}
VersionInfoCompany={#MyAppPublisher}
VersionInfoProductName=ADB 文件浏览器
VersionInfoDescription=ADB 文件浏览器 安装程序
VersionInfoCopyright=Copyright (C) 2025 joffeego. Licensed under the Apache License 2.0.

; Windows 10 or newer (see README): the program is built against the UCRT and needs
; OpenGL 3.3, so on Windows 7/8 the install would succeed and the program would then
; not start at all - one of the "it just won't open on that computer" reports.
; Refusing up front, with Inno's own message, is the clearer answer.
MinVersion=10.0

[Languages]
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; No adb / scrcpy here on purpose. They are unsigned third-party binaries, and
; bundling them is what made antivirus products flag the whole download (Defender
; names the archive, so it looked like our program was the problem). The app
; downloads both from the official sources on demand and verifies the published
; checksums - see 维护与发版指南.md and the release notes for the optional tools
; package.
Source: "build\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\assets\*";       DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "LICENSE";              DestDir: "{app}"; Flags: ignoreversion
Source: "THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#MyAppName}";  Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
