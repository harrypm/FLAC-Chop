; FLAC-Chop Windows installer (Inno Setup) — x86_64 + arm64.
;
; CI compiles this file in the "Create Windows installer EXE (Inno Setup)"
; steps of .github/workflows/build.yml, passing everything as /D defines:
;   /DFLAC_CHOP_APP_VERSION=<version>     full version (v1.0.8, v1.0.8-3-g…,
;                                         dev-<sha>): shown in Add/Remove
;                                         Programs + the install wizard
;   /DFLAC_CHOP_NUMERIC_VERSION=<x.y.z>   numeric semver for the PE
;                                         VersionInfoVersion resource
;                                         (must be numeric; CI derives it)
;   /DFLAC_CHOP_ARCH_SUFFIX=x86_64|arm64  output name + AppId (separate
;                                         uninstall entries per arch)
;   /DFLAC_CHOP_DIST_DIR=<abs>\dist       the windeployqt bundle to install
;   /DFLAC_CHOP_ICON_FILE=<abs>\…ico     setup + shortcut icon
;   /DFLAC_CHOP_OUTPUT_DIR=<abs repo root>  where the installer EXE is written
; The defaults below are relative to this script's folder (assets/installer)
; so a manual `iscc assets\installer\flac-chop.iss` from the repo root works
; too.
;
; The workflow then verifies the produced installer end to end: silent
; install -> the installed flac-chop.exe --version must report
; FLAC_CHOP_APP_VERSION (never "dev-…") -> silent uninstall. The installer is
; per-user (PrivilegesRequired=lowest, {localappdata}\Programs): no admin.

#ifndef FLAC_CHOP_APP_VERSION
#define FLAC_CHOP_APP_VERSION "dev"
#endif
#ifndef FLAC_CHOP_NUMERIC_VERSION
#define FLAC_CHOP_NUMERIC_VERSION "0.0.0.1"
#endif
#ifndef FLAC_CHOP_ARCH_SUFFIX
#define FLAC_CHOP_ARCH_SUFFIX "x86_64"
#endif
#ifndef FLAC_CHOP_DIST_DIR
#define FLAC_CHOP_DIST_DIR "..\..\dist"
#endif
#ifndef FLAC_CHOP_ICON_FILE
#define FLAC_CHOP_ICON_FILE "..\icons\flac-chop-icon.ico"
#endif
#ifndef FLAC_CHOP_OUTPUT_DIR
#define FLAC_CHOP_OUTPUT_DIR "..\.."
#endif

[Setup]
; AppId must be stable across releases (it keys the uninstall entry); one
; entry per arch so an x86_64 and an arm64 install never clobber each other.
AppId=FLACChop_{#FLAC_CHOP_ARCH_SUFFIX}
AppName=FLAC-Chop
AppVersion={#FLAC_CHOP_APP_VERSION}
AppVerName=FLAC-Chop {#FLAC_CHOP_APP_VERSION}
AppPublisher=harrypm
AppPublisherURL=https://github.com/harrypm/FLAC-Chop
AppSupportURL=https://github.com/harrypm/FLAC-Chop/issues
AppUpdatesURL=https://github.com/harrypm/FLAC-Chop/releases
DefaultDirName={localappdata}\Programs\FLAC-Chop
DisableProgramGroupPage=yes
OutputDir={#FLAC_CHOP_OUTPUT_DIR}
OutputBaseFilename=windows_FLAC-Chop_{#FLAC_CHOP_APP_VERSION}_{#FLAC_CHOP_ARCH_SUFFIX}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
SetupIconFile={#FLAC_CHOP_ICON_FILE}
UninstallDisplayIcon={app}\flac-chop.exe
VersionInfoVersion={#FLAC_CHOP_NUMERIC_VERSION}

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; \
    GroupDescription: "Additional icons:"

[Files]
; dist/ is the windeployqt + sox + runtime-DLL bundle; install it whole
; (recursing into platforms/, styles/, tls/, imageformats/, …).
Source: "{#FLAC_CHOP_DIST_DIR}\*"; DestDir: "{app}"; \
    Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\FLAC-Chop"; Filename: "{app}\flac-chop.exe"
Name: "{autodesktop}\FLAC-Chop"; Filename: "{app}\flac-chop.exe"; \
    Tasks: desktopicon

[Run]
Filename: "{app}\flac-chop.exe"; Description: "Launch FLAC-Chop"; \
    Flags: nowait postinstall skipifsilent
