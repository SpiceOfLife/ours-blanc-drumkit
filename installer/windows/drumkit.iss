; ours-blanc Drum Kit — Windows installer (Inno Setup 6)
;   iscc /DVersion=0.1.0 /DArtefacts=<build>\OursBlancDrumKit_artefacts\Release /DOutDir=<out> drumkit.iss
#ifndef Version
  #define Version "0.1.0"
#endif

[Setup]
AppId={{B3F0C2D4-8E61-4A27-9D5C-2F7A1E6B0C93}
AppName=ours-blanc Drum Kit
AppVersion={#Version}
AppPublisher=ours-blanc
AppPublisherURL=https://ours-blanc.app/lab/drumkit/
DefaultDirName={autopf}\ours-blanc\Drum Kit
DefaultGroupName=ours-blanc
DisableProgramGroupPage=yes
OutputDir={#OutDir}
OutputBaseFilename=ours-blanc-DrumKit-{#Version}-windows
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "すべて（スタンドアローン + VST3）"
Name: "custom"; Description: "選んでインストール"; Flags: iscustom

[Components]
Name: "app"; Description: "ours-blanc Drum Kit（スタンドアローン）"; Types: full custom
Name: "vst3"; Description: "VST3 プラグイン"; Types: full custom

[Files]
Source: "{#Artefacts}\Standalone\ours-blanc Drum Kit.exe"; DestDir: "{app}"; Components: app; Flags: ignoreversion
Source: "{#Artefacts}\VST3\ours-blanc Drum Kit.vst3\*"; DestDir: "{commoncf64}\VST3\ours-blanc Drum Kit.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\ours-blanc Drum Kit"; Filename: "{app}\ours-blanc Drum Kit.exe"; Components: app
Name: "{autodesktop}\ours-blanc Drum Kit"; Filename: "{app}\ours-blanc Drum Kit.exe"; Components: app; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "デスクトップにアイコンを作る"; Components: app

[Run]
Filename: "{app}\ours-blanc Drum Kit.exe"; Description: "ours-blanc Drum Kit を起動"; Flags: nowait postinstall skipifsilent; Components: app
