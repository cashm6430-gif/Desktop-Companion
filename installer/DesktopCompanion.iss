; Desktop-Companion installer (Inno Setup 6).
;
; Built by tools/package.ps1, which passes /DAppVersion, /DStageDir and /DOutDir.
; It packages the same staged folder the portable zip ships, so both flavours
; are byte-for-byte the same runtime.
;
; Per-user install (no admin): {localappdata}\Programs\DesktopCompanion. The
; "agent hooks" task wires Codex / WorkBuddy / Claude to the bundled hook helper
; at uninstall-safe absolute paths, and uninstall removes those entries again.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef StageDir
  #define StageDir "..\build\package\DesktopCompanion"
#endif
#ifndef OutDir
  #define OutDir "..\build\package"
#endif

#define AppName "Desktop Companion"
#define AppPublisher "Desktop Companion"
#define AppExe "DesktopCompanion.exe"

[Setup]
AppId={{5F2C9A14-7E3B-4D6A-9C81-2B4E6F0A9D31}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\Programs\DesktopCompanion
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#OutDir}
OutputBaseFilename=DesktopCompanion-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#AppExe}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "agenthooks"; Description: "Install hooks for AI coding agents (Codex / WorkBuddy / Claude)"; GroupDescription: "Additional:"; Flags: checkedonce
Name: "autostart"; Description: "Start {#AppName} when I sign in"; GroupDescription: "Additional:"; Flags: unchecked
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional:"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon
Name: "{userstartup}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: autostart

[Run]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\tools\install-hooks.ps1"" -HelperPath ""{app}\DesktopCompanionHook.exe"""; StatusMsg: "Installing AI-agent hooks..."; Flags: runhidden; Tasks: agenthooks
Filename: "{app}\{#AppExe}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\tools\install-hooks.ps1"" -HelperPath ""{app}\DesktopCompanionHook.exe"" -Remove"; Flags: runhidden; RunOnceId: "RemovePetHooks"
; The pet seeds the Claude-Desktop per-session configs itself, so those entries have
; to be taken back out here too -- otherwise uninstalling would leave hooks pointing
; at a helper that no longer exists. The script is still on disk at this point.
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\tools\seed-cowork-hooks.ps1"" -Remove"; Flags: runhidden; RunOnceId: "RemoveCoworkHooks"
