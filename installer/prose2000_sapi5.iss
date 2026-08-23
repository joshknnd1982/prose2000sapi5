; Prose 2000 SAPI 5 - Windows installer
;
; Built by tools\build_all.ps1, which passes the paths in as /D defines. To compile it by
; hand:
;
;   ISCC.exe /DStageDir=..\output /DVersion=1.0.0 installer\prose2000_sapi5.iss
;
; Accessibility notes, since this installer is meant to be usable by the people most likely
; to want these voices:
;   * Every page is a standard Inno Setup page built from real Win32 controls, which screen
;     readers read natively. Nothing is owner-drawn and there is no splash screen.
;   * The components tree and type combo are given accessible names in InitializeWizard;
;     without that a screen reader announces them as nothing but their control type.
;   * Nothing steals focus, and no page auto-advances.
;   * Every outcome that matters - which interfaces registered, how many voices are visible,
;     where the logs are - is stated in text on the final page and written to the log, not
;     signalled by a colour or an icon.
;   * SetupLogging is on, so a failed install always leaves a full log behind.

#ifndef StageDir
  #define StageDir "..\output"
#endif
#ifndef Version
  #define Version "1.0.0"
#endif

#define AppName        "Prose 2000 SAPI 5"
#define AppPublisher   "Prose 2000 SAPI 5 project"
#define EngineDllName  "Prose2000SAPI5.dll"
#define ConfigExeName  "Prose2000Config.exe"
#define HostExeName    "ProseHost.exe"

[Setup]
#ifdef Probe
; A separate identity so an accessibility probe run can never be mistaken for, or clash
; with, a real installation.
AppId={{9D4E2B71-33A8-4C16-B5F0-1E7A62D9C084}
#else
AppId={{4A81F5C2-7E63-4D09-9C21-58B3E0A7D461}
#endif
AppName={#AppName}
AppVersion={#Version}
AppVerName={#AppName} {#Version}
AppPublisher={#AppPublisher}
AppComments=The emulated Telesensory Systems Prose 2000, with three English voices, available to any SAPI 5 application.
UninstallDisplayName={#AppName}
DefaultDirName={autopf}\Prose2000SAPI5
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir={#StageDir}\..\dist
#ifdef Probe
OutputBaseFilename=Prose2000SAPI5_AccessibilityProbe
#else
OutputBaseFilename=Prose2000SAPI5_Setup_{#Version}
#endif
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; Both the CLSID registrations and the speech TokenEnums key live under HKLM.
; Compiling with /DProbe builds the same wizard without elevation and without payload,
; which is how the pages are checked against a screen reader.
#ifdef Probe
PrivilegesRequired=lowest
#else
PrivilegesRequired=admin
#endif

; ProseHost.exe - the emulator that runs the original firmware - is a 64-bit program and
; there is no 32-bit build of it, so 64-bit Windows is a hard requirement. The 32-bit SAPI
; interface is still installed and still works: it is there for 32-bit *applications*, and
; it spawns the same 64-bit emulator under WOW64.
ArchitecturesAllowed=x64compatible
; Installing in 64-bit mode is what makes {sys} mean the 64-bit System32 and {syswow64} the
; 32-bit one, which the registration steps below depend on.
ArchitecturesInstallIn64BitMode=x64compatible

; A full log is written to the temp folder and copied beside the program at the end.
SetupLogging=yes

AlwaysShowComponentsList=yes
ShowComponentSizes=yes
InfoBeforeFile={#StageDir}\..\installer\before_install.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full";   Description: "Everything - both interfaces, all three voices, and the tools"
Name: "custom"; Description: "Choose what to install"; Flags: iscustom

[Components]
Name: "engine";  Description: "Speech engine, firmware and SAPI 5 interfaces (required)"; Types: full custom; Flags: fixed
Name: "config";  Description: "Prose 2000 configuration utility"; Types: full custom
Name: "tools";   Description: "Diagnostic tools (sample renderer and test harness)"; Types: full

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut to the configuration utility"; GroupDescription: "Shortcuts:"; Components: config

[Files]
#ifdef Probe
Source: "{#StageDir}\..\installer\before_install.txt"; DestDir: "{app}"; Components: engine
#else
; ---- the SAPI 5 interfaces -------------------------------------------------------------
; The 32-bit DLL serves 32-bit applications, the 64-bit DLL serves 64-bit ones. Both spawn
; the same 64-bit emulator, so unlike most dual-architecture speech engines there is no
; helper process and no surrogate anywhere in this package.
Source: "{#StageDir}\{#EngineDllName}";       DestDir: "{app}";     Components: engine; Flags: ignoreversion
Source: "{#StageDir}\prose_speak.exe";        DestDir: "{app}";     Components: engine; Flags: ignoreversion
Source: "{#StageDir}\x64\{#EngineDllName}";   DestDir: "{app}\x64"; Components: engine; Flags: ignoreversion
Source: "{#StageDir}\x64\prose_speak.exe";    DestDir: "{app}\x64"; Components: engine; Flags: ignoreversion

; ---- the emulator and the original firmware --------------------------------------------
; All six ROM images are required; the engine refuses to start if any is missing.
Source: "{#StageDir}\{#HostExeName}";         DestDir: "{app}";     Components: engine; Flags: ignoreversion
Source: "{#StageDir}\roms\*";                 DestDir: "{app}\roms"; Components: engine; Flags: ignoreversion
Source: "{#StageDir}\ProseHost-LICENSE.txt";  DestDir: "{app}";     Components: engine; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StageDir}\open_logs.cmd";          DestDir: "{app}";     Components: engine; Flags: ignoreversion

; ---- the configuration utility ----------------------------------------------------------
Source: "{#StageDir}\{#ConfigExeName}";       DestDir: "{app}";     Components: config; Flags: ignoreversion

; ---- diagnostics ------------------------------------------------------------------------
Source: "{#StageDir}\prose_render.exe";       DestDir: "{app}";     Components: tools; Flags: ignoreversion
Source: "{#StageDir}\prose_sapitest.exe";     DestDir: "{app}";     Components: tools; Flags: ignoreversion
Source: "{#StageDir}\x64\prose_render.exe";   DestDir: "{app}\x64"; Components: tools; Flags: ignoreversion
Source: "{#StageDir}\x64\prose_sapitest.exe"; DestDir: "{app}\x64"; Components: tools; Flags: ignoreversion
#endif

[Icons]
Name: "{group}\Prose 2000 Speech Settings"; Filename: "{app}\{#ConfigExeName}"; Components: config; Comment: "Adjust the rate, pitch and volume of the Prose 2000 voices"
Name: "{commondesktop}\Prose 2000 Speech Settings"; Filename: "{app}\{#ConfigExeName}"; Components: config; Tasks: desktopicon; Comment: "Adjust the rate, pitch and volume of the Prose 2000 voices"
Name: "{group}\Speak a test sentence";      Filename: "{app}\prose_speak.exe"; Comment: "Speaks one sentence using a Prose 2000 voice"
Name: "{group}\List installed voices";      Filename: "{app}\prose_speak.exe"; Parameters: "--list"; Comment: "Lists every SAPI 5 voice Windows can see"
Name: "{group}\Open the log folder";        Filename: "{app}\open_logs.cmd"; Comment: "Opens the folder the speech engine writes its logs to"

[Run]
Filename: "{app}\prose_speak.exe"; Description: "Speak a test sentence now"; Flags: postinstall nowait skipifsilent unchecked
Filename: "{app}\{#ConfigExeName}"; Description: "Open the Prose 2000 speech settings"; Components: config; Flags: postinstall nowait skipifsilent unchecked

[UninstallDelete]
Type: filesandordirs; Name: "{app}\roms"
Type: filesandordirs; Name: "{app}\x64"
Type: files;          Name: "{app}\install.log"
Type: files;          Name: "{app}\open_logs.cmd"

[Code]
{ Used to give the components tree an accessible name. Inno exposes no property for it, and
  without one a screen reader announces the control as nothing but its type. }
procedure SetWindowTextW(Wnd: HWND; Text: String);
  external 'SetWindowTextW@user32.dll stdcall';

var
  RegisteredX86: Boolean;
  RegisteredX64: Boolean;
  RegistrationNotes: String;

procedure Note(const S: String);
begin
  Log('[prose2000] ' + S);
  if RegistrationNotes <> '' then
    RegistrationNotes := RegistrationNotes + #13#10;
  RegistrationNotes := RegistrationNotes + S;
end;

{ A running emulator holds ProseHost.exe open, so it has to go before files are replaced or
  removed. The next client starts a fresh one on demand. }
procedure StopEngine;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM {#HostExeName} /F', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM {#ConfigExeName} /F', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

{ regsvr32 is bitness-specific: the copy in System32 registers 64-bit DLLs, the copy in
  SysWOW64 registers 32-bit ones. Getting these the wrong way round is the classic way to
  end up with a voice that is registered but never enumerated. }
function RunRegsvr(const Regsvr32, DllPath, Args: String): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(Regsvr32, Args + ' "' + DllPath + '"', '', SW_HIDE,
                 ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
  Log(Format('[prose2000] %s %s "%s" -> %d', [Regsvr32, Args, DllPath, ResultCode]));
end;

{ The 32-bit and 64-bit registrations write to different views of the registry, so both are
  checked. Either one being present means Windows can enumerate the voices for that
  bitness of application. }
function VoicesRegistered: Boolean;
begin
  Result := RegKeyExists(HKEY_LOCAL_MACHINE,
    'SOFTWARE\Microsoft\Speech\Voices\TokenEnums\Prose2000');
  if not Result then
    Result := RegKeyExists(HKEY_LOCAL_MACHINE,
      'SOFTWARE\WOW6432Node\Microsoft\Speech\Voices\TokenEnums\Prose2000');
end;

function FirmwarePresent: Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\{#HostExeName}')) and
            FileExists(ExpandConstant('{app}\roms\v3.4.1__2000__0.u21')) and
            FileExists(ExpandConstant('{app}\roms\v3.4.1__2000__1.u44')) and
            FileExists(ExpandConstant('{app}\roms\v3.4.1__2000__2.u22')) and
            FileExists(ExpandConstant('{app}\roms\v3.4.1__2000__3.u45')) and
            FileExists(ExpandConstant('{app}\roms\v3.12__8-9-88__dsp_prog.u29')) and
            FileExists(ExpandConstant('{app}\roms\v3.12__8-9-88__dsp_data.u29'));
end;

procedure InitializeWizard;
begin
  { Verified with tools\check_installer_a11y.ps1, which reads every control through MSAA
    the way NVDA does. Without these the components tree reports no name at all. }
  SetWindowTextW(WizardForm.ComponentsList.Handle, 'Components to install');
  SetWindowTextW(WizardForm.TypesCombo.Handle, 'Installation type');
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  LogPath: String;
begin
  if CurStep = ssInstall then
  begin
    StopEngine;
  end
  else if CurStep = ssPostInstall then
  begin
    RegistrationNotes := '';

    { Both interfaces are registered on every machine this installer will run on, because
      64-bit Windows is a hard requirement - the emulator has no 32-bit build. }
    RegisteredX64 := RunRegsvr(ExpandConstant('{sys}\regsvr32.exe'),
                               ExpandConstant('{app}\x64\{#EngineDllName}'), '/s');
    RegisteredX86 := RunRegsvr(ExpandConstant('{syswow64}\regsvr32.exe'),
                               ExpandConstant('{app}\{#EngineDllName}'), '/s');

    if RegisteredX64 then
      Note('The 64-bit SAPI 5 interface was registered.')
    else
      Note('The 64-bit SAPI 5 interface could NOT be registered.');

    if RegisteredX86 then
      Note('The 32-bit SAPI 5 interface was registered.')
    else
      Note('The 32-bit SAPI 5 interface could NOT be registered.');

    if FirmwarePresent then
      Note('The emulator and all six firmware images are installed.')
    else
      Note('Warning: part of the firmware is missing; the voices will not speak.');

    if VoicesRegistered then
      Note('Windows speech settings can now see all three Prose 2000 voices: ' +
           'Prose 2000, Prose 2000 Deep and Prose 2000 High.')
    else
      Note('Warning: the speech voice list was not updated. See the log named below.');

    { Keep the installer's own log with the program, where a bug report can find it. }
    LogPath := ExpandConstant('{log}');
    if LogPath <> '' then
    begin
      CopyFile(LogPath, ExpandConstant('{app}\install.log'), False);
      Note('A full installation log was saved as ' + ExpandConstant('{app}\install.log') + '.');
    end;
    { Written as an unexpanded environment variable on purpose: under an administrative
      install the localappdata constant resolves to the administrator's folder, not the
      folder the person who actually uses the voices will find their logs in. }
    Note('The speech engine writes its own logs to '
         + '%LOCALAPPDATA%\Prose2000 SAPI5\Logs'
         + ' - there is a shortcut to it in the Start menu.');
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    StopEngine;
    RunRegsvr(ExpandConstant('{sys}\regsvr32.exe'),
              ExpandConstant('{app}\x64\{#EngineDllName}'), '/s /u');
    RunRegsvr(ExpandConstant('{syswow64}\regsvr32.exe'),
              ExpandConstant('{app}\{#EngineDllName}'), '/s /u');
    { Give the loader a moment to let go of the DLLs before the files are deleted. }
    Sleep(1500);
  end;
end;

{ The finish page normally shows one fixed line. Replacing it with the notes above means a
  screen reader reads the actual outcome - which interfaces registered, whether the firmware
  is there, where the logs are - instead of a generic success message. }
procedure CurPageChanged(CurPageID: Integer);
var
  Blank: String;
  Summary: String;
begin
  if (CurPageID = wpFinished) and (RegistrationNotes <> '') then
  begin
    Blank := #13#10 + #13#10;
    Summary := '{#AppName} has been installed.' + Blank + RegistrationNotes + Blank +
      'Choose a voice in your screen reader or in Windows speech settings. Rate, pitch and ' +
      'volume can be trimmed in the Prose 2000 configuration utility, and any change there ' +
      'takes effect on the next thing spoken.';
    WizardForm.FinishedLabel.AutoSize := False;
    WizardForm.FinishedLabel.Height := WizardForm.FinishedLabel.Parent.ClientHeight - 8;
    WizardForm.FinishedLabel.Caption := Summary;
  end;
end;
