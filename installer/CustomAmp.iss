; Installer for Custom Amp, made with Inno Setup 6 (https://jrsoftware.org/isinfo.php).
; CI builds it after the plugin:
;   ISCC /DAppVersion=0.1.0 installer\CustomAmp.iss
; It installs the VST3 into the standard VST3 folder, the standalone app into Program Files (with a
; Start menu entry), and the presets as editable files into Public Documents\Custom Amp\Presets.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

#ifndef BuildDir
  #define BuildDir "..\build\CustomAmp_artefacts\Release"
#endif

[Setup]
; Never change AppId: Windows uses it to recognise updates of the same program.
AppId={{42251C65-E078-4E35-A11F-D050236631F0}
AppName=Custom Amp
AppVersion={#AppVersion}
AppPublisher=Kursat
DefaultDirName={autopf}\Custom Amp
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\build\installer
OutputBaseFilename=Custom-Amp-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
UninstallDisplayName=Custom Amp
UninstallDisplayIcon={app}\Custom Amp.exe

[Files]
Source: "{#BuildDir}\VST3\Custom Amp.vst3\*"; DestDir: "{commoncf64}\VST3\Custom Amp.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\Standalone\Custom Amp.exe"; DestDir: "{app}"; Flags: ignoreversion
; Your own copies to open and edit (the same presets are also built into the plugin's menu).
; An update never overwrites them and uninstalling keeps them, so your changes are safe.
Source: "..\presets\*.json"; DestDir: "{commondocs}\Custom Amp\Presets"; Flags: onlyifdoesntexist uninsneveruninstall

[Icons]
Name: "{autoprograms}\Custom Amp"; Filename: "{app}\Custom Amp.exe"

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Custom Amp.vst3"

[Run]
Filename: "{app}\Custom Amp.exe"; Description: "Start the Custom Amp app now"; Flags: postinstall nowait skipifsilent unchecked
