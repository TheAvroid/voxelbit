; ---------------------------------------------------------------------------
; voxelbit.iss -- the shipped installer.
;
; Built by:  python tools/package.py --installer
; Needs:     dist/stage populated (package.py does that first)
; Produces:  voxelbit-setup.exe at the repository root
;
; ---------------------------------------------------------------------------
; WHY THIS EXISTS AT ALL
; ---------------------------------------------------------------------------
;
; voxelbit.exe was a launcher stub with the whole game appended after its PE
; image, unpacked at runtime and executed. That is, structurally, what a
; dropper does -- Defender's ML classifier scored it Trojan:Win32/Sabsik.FL.A!ml
; and DELETED it on download rather than warning about it. The game's own
; binary was never the problem: the same files inside a plain zip pass a clean
; Defender scan. It was the hand-rolled self-extractor around them.
;
; An Inno installer is a recognised, extremely common package format rather
; than an opaque blob welded to a stub, so there is far less for a classifier
; to object to. It is NOT a substitute for code signing: the installer this
; produces is unsigned, so SmartScreen will still say "unrecognized app" on
; first run. That is the click-through warning, not the silent deletion.
;
; ---------------------------------------------------------------------------
; THE LAYOUT IS LOAD-BEARING
; ---------------------------------------------------------------------------
;
; Staging puts the engine in app/ and the content in data/ as siblings, and
; the old launcher bridged them by setting VOXELBIT_DATA before starting the
; game. An installer sets no environment, so the layout has to satisfy the
; engine on its own: engine/src/core/assetroot.h resolves its root by looking
; for `data/game/assets` BESIDE the executable first. So app/ is flattened
; into {app} and data/ lands directly beneath it:
;
;     {app}\voxelbit.exe      <- app\v1.exe, renamed
;     {app}\Falcor.dll, shaders\, plugins\, pythondist\ ...
;     {app}\data\game\assets\ ...
;
; Change that and the game starts, finds nothing, and falls back to the
; hard-coded "C:/voxelbit" -- which works on this machine and on no other.
;
; v1.exe IS RENAMED because nothing at runtime reads its own filename: Falcor
; locates shaders and the DLSS blobs through getRuntimeDirectory, which is a
; path and not a name. "v1" is an internal CMake target that no player should
; have to recognise as the game.
; ---------------------------------------------------------------------------

#define AppName      "voxelbit"
#define AppVersion   "1.4"
#define AppPublisher "voxelbit"
#define AppExe       "voxelbit.exe"

[Setup]
; A FIXED GUID. It is what makes a second install an UPGRADE of the first
; rather than a second copy beside it -- generated once, never regenerated.
AppId={{8F3A1C42-7B6E-4D95-9E2F-3C5A81D0B7E4}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\voxelbit
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=no

; -- NO ADMIN, AND THAT IS DELIBERATE ---------------------------------------
;
; %LOCALAPPDATA% is where the old launcher unpacked to, so this is the same
; place the game has always lived. Installing under Program Files would mean a
; UAC prompt on top of the SmartScreen one, and the whole point of this file is
; to REMOVE scary dialogs rather than collect them.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

; 64-bit only. The engine is D3D12 + DLSS on an RTX card; there is no 32-bit
; build and never was.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

OutputDir=..
; THE PLAIN NAME (user 2026-09-21: "rename voxelbit-setup.exe to just
; voxelbit.exe"). This IS the download now -- the self-extractor it replaced
; is retired, and a player fetching a game should not have to know which of
; two exes is the one they want.
OutputBaseFilename=voxelbit
SetupIconFile=..\game\logo.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName} {#AppVersion}

; -- COMPRESSION -------------------------------------------------------------
;
; 1.3 GB of payload across ~5,800 files. Solid compression matters here
; precisely because so much of it is small and similar -- pythondist alone is
; 126 MB of stdlib. lzma2/max is slow to BUILD (tens of minutes) and is paid
; once per release, against a download every player pays.
Compression=lzma2/max
SolidCompression=yes
LZMANumBlockThreads=4
LZMAUseSeparateProcess=yes

; The player needs room for the download AND the install.
ExtraDiskSpaceRequired=0
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

; ---------------------------------------------------------------------------
; THE TITLE IS LOWERCASE (user 2026-09-21: "replace the Setup text with setup").
;
; Default.isl ships `SetupAppTitle=Setup` and `SetupWindowTitle=Setup - %1`, so
; the window reads "Setup - voxelbit" -- one capitalised word beside a wordmark
; that has never had one. Everything the player sees in this game is lowercase:
; the mark itself, the button labels, the settings rows, the watermark.
;
; BOTH, NOT JUST THE WINDOW. SetupWindowTitle is the title bar; SetupAppTitle is
; what the taskbar button and the Alt-Tab entry say. Changing one leaves the
; other capitalised in the place it is most likely to be noticed next.
;
; %1 IS THE APP NAME and has to stay -- Inno substitutes AppVerName into it.
; ---------------------------------------------------------------------------
[Messages]
SetupAppTitle=setup
SetupWindowTitle=setup - %1

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; THE GAME BINARY, RENAMED. Listed FIRST and excluded from the wildcard below,
; or it would be installed twice -- once as voxelbit.exe and once as v1.exe.
Source: "..\dist\stage\app\v1.exe"; DestDir: "{app}"; DestName: "{#AppExe}"; Flags: ignoreversion

; ...THE REST OF THE ENGINE, FLATTENED OUT OF app\ INTO {app}.
Source: "..\dist\stage\app\*"; DestDir: "{app}"; Excludes: "v1.exe"; \
    Flags: ignoreversion recursesubdirs createallsubdirs

; ...AND THE CONTENT, BESIDE IT. This is the path assetroot.h looks for.
Source: "..\dist\stage\data\*"; DestDir: "{app}\data"; \
    Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
; WorkingDir IS {app} AND HAS TO BE. The engine writes screenshots and
; recordings relative to where it is started from.
Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; \
    Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; THE GAME WRITES BESIDE ITSELF and none of it is in [Files], so without these
; an uninstall leaves the directory standing with whatever it produced in it.
;
; MEASURED, NOT GUESSED. A silent install/uninstall cycle left exactly one
; file -- voxelbit.exe.0.log, the crash reporter's own log (see
; engine/src/platform/crashlog.h, which names it after the executable). The
; shader cache was already handled; the log was not, and a rule written from
; imagination would have missed it the same way.
Type: filesandordirs; Name: "{app}\.shadercache"
Type: files;          Name: "{app}\*.log"
Type: filesandordirs; Name: "{app}\screenshots"
Type: filesandordirs; Name: "{app}\recordings"
Type: dirifempty;     Name: "{app}"
