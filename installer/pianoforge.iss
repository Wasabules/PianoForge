; PianoForge's installer (Inno Setup 6), compiled by the `installer` build target (make_installer.cmake), which
; defines AppVersion, StageDir, OutputDir, IconFile and WithObs.
;
; - For the current user by default (no administrator rights; %LOCALAPPDATA%\Programs\PianoForge), or for every
;   user if chosen; an update keeps the choice made the first time.
; - The application and the Visual C++ runtime next to it; the VST3 plugin (ticked by default), in the VST3 folder
;   of every user (all-users install: Common Files\VST3) or of the current user (%LOCALAPPDATA%\Programs\Common\VST3);
;   optionally the OBS Studio plugin, where OBS looks for plugins of every user. A plugin a host or OBS is using
;   moves aside, and the new one loads when it is loaded again.
; - Never the corpus nor a profile: the application asks for its corpus at first start.
; - Settings, takes and recordings are the user's: neither an update nor an uninstall touches them.
; - Updates (Help > Check for updates) run it with /SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS
;   /relaunch=1: the application closes, the installer replaces it and opens it again.

#ifndef AppVersion
  #error AppVersion must be defined (the installer build target does)
#endif
#ifndef WithObs
  #define WithObs "0"
#endif
#ifndef WithVst3
  #define WithVst3 "0"
#endif

[Setup]
AppId={{6E2B7C1A-3F4D-4B8E-9A61-5C0D2E7F8B34}
AppName=PianoForge
AppVersion={#AppVersion}
AppVerName=PianoForge {#AppVersion}
AppPublisher=PianoForge
VersionInfoVersion={#AppVersion}
VersionInfoDescription=PianoForge installer
DefaultDirName={autopf}\PianoForge
DefaultGroupName=PianoForge
DisableProgramGroupPage=yes
AllowNoIcons=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
UsePreviousPrivileges=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=PianoForge-{#AppVersion}-setup
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\PianoForge.exe
UninstallDisplayName=PianoForge
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
#if WithVst3 == "1"
Name: "vst3"; Description: "The VST3 plugin (PianoForge in a host: REAPER, FL Studio, Cubase...)"; GroupDescription: "Plugins:"
#endif
#if WithObs == "1"
Name: "obsplugin"; Description: "The OBS Studio plugin (the PianoForge audio source, for streaming)"; GroupDescription: "OBS Studio:"; Check: ObsInstalled
#endif

[Files]
Source: "{#StageDir}\PianoForge.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\msvcp140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\vcruntime140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion
#if WithVst3 == "1"
Source: "{#StageDir}\vst3\PianoForge.vst3\Contents\x86_64-win\PianoForge.vst3"; DestDir: "{autocf64}\VST3\PianoForge.vst3\Contents\x86_64-win"; Flags: ignoreversion; Tasks: vst3; BeforeInstall: MoveVst3Aside
Source: "{#StageDir}\vst3\PianoForge.vst3\Contents\Resources\moduleinfo.json"; DestDir: "{autocf64}\VST3\PianoForge.vst3\Contents\Resources"; Flags: ignoreversion; Tasks: vst3
Source: "{#StageDir}\vst3\PianoForge.vst3\desktop.ini"; DestDir: "{autocf64}\VST3\PianoForge.vst3"; Flags: ignoreversion; Tasks: vst3
Source: "{#StageDir}\vst3\PianoForge.vst3\Plugin.ico"; DestDir: "{autocf64}\VST3\PianoForge.vst3"; Flags: ignoreversion; Tasks: vst3
#endif
#if WithObs == "1"
Source: "{#StageDir}\obs\pianoforge-obs.dll"; DestDir: "{commonappdata}\obs-studio\plugins\pianoforge-obs\bin\64bit"; Flags: ignoreversion; Tasks: obsplugin; BeforeInstall: MovePluginAside
Source: "{#StageDir}\obs\en-US.ini"; DestDir: "{commonappdata}\obs-studio\plugins\pianoforge-obs\data\locale"; Flags: ignoreversion; Tasks: obsplugin
#endif

[Icons]
Name: "{group}\PianoForge"; Filename: "{app}\PianoForge.exe"
Name: "{autodesktop}\PianoForge"; Filename: "{app}\PianoForge.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\PianoForge.exe"; Description: "{cm:LaunchProgram,PianoForge}"; Flags: nowait postinstall skipifsilent
Filename: "{app}\PianoForge.exe"; Flags: nowait; Check: RelaunchAfterUpdate

[UninstallDelete]
Type: files; Name: "{commonappdata}\obs-studio\plugins\pianoforge-obs\bin\64bit\*.old"
Type: files; Name: "{autocf64}\VST3\PianoForge.vst3\Contents\x86_64-win\*.old"

[Code]
function ObsInstalled: Boolean;
begin
  Result := FileExists(ExpandConstant('{commonpf64}\obs-studio\bin\64bit\obs64.exe'));
end;

{ An update started by the application (/relaunch=1): it opens it again once installed. }
function RelaunchAfterUpdate: Boolean;
begin
  Result := WizardSilent and (ExpandConstant('{param:relaunch|0}') = '1');
end;

{ OBS keeps the plugin it uses from being overwritten, not from being renamed: that one moves aside (removed by a
  later install, once OBS has let go of it), and the new one loads at OBS's next start. }
procedure MovePluginAside;
var
  Dir, Target: String;
  Found: TFindRec;
begin
  Dir := ExpandConstant('{commonappdata}\obs-studio\plugins\pianoforge-obs\bin\64bit');
  if FindFirst(Dir + '\*.old', Found) then
  try
    repeat
      DeleteFile(Dir + '\' + Found.Name);
    until not FindNext(Found);
  finally
    FindClose(Found);
  end;
  Target := Dir + '\pianoforge-obs.dll';
  if FileExists(Target) and not DeleteFile(Target) then
    RenameFile(Target, Target + '.' + GetDateTimeString('yyyymmdd-hhnnss', #0, #0) + '.old');
end;

{ The same for the VST3 plugin a host has loaded. }
procedure MoveVst3Aside;
var
  Dir, Target: String;
  Found: TFindRec;
begin
  Dir := ExpandConstant('{autocf64}\VST3\PianoForge.vst3\Contents\x86_64-win');
  if FindFirst(Dir + '\*.old', Found) then
  try
    repeat
      DeleteFile(Dir + '\' + Found.Name);
    until not FindNext(Found);
  finally
    FindClose(Found);
  end;
  Target := Dir + '\PianoForge.vst3';
  if FileExists(Target) and not DeleteFile(Target) then
    RenameFile(Target, Target + '.' + GetDateTimeString('yyyymmdd-hhnnss', #0, #0) + '.old');
end;
