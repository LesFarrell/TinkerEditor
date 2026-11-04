; Inno Setup script for Tinker Editor.
; Build first (build.bat, or any premake5.lua toolchain), then compile this
; with Inno Setup 6 (ISCC.exe tinkeredit.iss) from the repo root.
; build.bat stages the complete release tree in dist/; everything installed
; comes from there. User settings, the WebView2 browser profile, debug
; symbols, import libraries, and compiler objects stay out.

#define MyAppName "Tinker Editor"
#define MyAppExeName "tinkeredit.exe"
#define MyAppExePath AddBackslash(SourcePath) + "dist\" + MyAppExeName

#if !FileExists(MyAppExePath)
  #error dist\tinkeredit.exe is missing - run build.bat before compiling the installer.
#endif

; Version, publisher, and copyright are read from tinkeredit.exe's own
; VERSIONINFO (tinkeredit.rc) - the same source Help > About shows - so the
; installer can never disagree with the program it installs.
#define MyAppVersion GetStringFileInfo(MyAppExePath, "Tinker Editor")
#define MyAppPublisher GetStringFileInfo(MyAppExePath, "Les Farrell")
#define MyAppCopyright GetStringFileInfo(MyAppExePath, "(c) 2026 Les Farrell")

[Setup]
AppId={{8C1FAAE2-ED8A-4F09-BF45-2CD28C46BD0F}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppCopyright={#MyAppCopyright}
VersionInfoVersion={#GetVersionNumbersString(MyAppExePath)}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
OutputBaseFilename=TinkerEditorSetup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
WizardStyle=modern
; The association tasks write under Software\Classes; tell Explorer so new
; "Open with" entries show up without signing out.
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
Name: "assocmd"; Description: "Associate Markdown files (.md, .markdown, ...) with {#MyAppName}"; GroupDescription: "File associations:"
Name: "openwith"; Description: "Add {#MyAppName} to ""Open with"" for text, HTML, and source code files"; GroupDescription: "File associations:"; Flags: unchecked

[Files]
; Application runtime.
Source: "dist\tinkeredit.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\Scintilla.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\WebView2Loader.dll"; DestDir: "{app}"; Flags: ignoreversion

; Offline preview assets.
Source: "dist\preview_shell.html"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\github-markdown.css"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\mermaid.min.js"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\highlight.min.js"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\highlight-powershell.min.js"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\highlight-github.min.css"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\highlight-github-dark.min.css"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\katex\*"; DestDir: "{app}\katex"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "dist\plugins\*"; DestDir: "{app}\plugins"; Flags: ignoreversion recursesubdirs createallsubdirs

; Application and redistributed-component licences.
Source: "dist\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\third_party\cmark-gfm\COPYING"; DestDir: "{app}\third_party\cmark-gfm"; Flags: ignoreversion
Source: "dist\third_party\cmark-gfm\VERSION.txt"; DestDir: "{app}\third_party\cmark-gfm"; Flags: ignoreversion
Source: "dist\third_party\gdb\bin\gdb.exe"; DestDir: "{app}\third_party\gdb\bin"; Flags: ignoreversion
Source: "dist\third_party\gdb\COPYING.txt"; DestDir: "{app}\third_party\gdb"; Flags: ignoreversion
Source: "dist\third_party\gdb\COPYING.MinGW-w64-runtime.txt"; DestDir: "{app}\third_party\gdb"; Flags: ignoreversion
Source: "dist\third_party\gdb\VERSION.txt"; DestDir: "{app}\third_party\gdb"; Flags: ignoreversion
Source: "dist\third_party\gdb\SOURCE.md"; DestDir: "{app}\third_party\gdb"; Flags: ignoreversion
Source: "dist\third_party\gdb\SHA256SUMS.txt"; DestDir: "{app}\third_party\gdb"; Flags: ignoreversion
Source: "dist\third_party\github-markdown-css\LICENSE"; DestDir: "{app}\third_party\github-markdown-css"; Flags: ignoreversion
Source: "dist\third_party\github-markdown-css\VERSION.txt"; DestDir: "{app}\third_party\github-markdown-css"; Flags: ignoreversion
Source: "dist\third_party\highlightjs\LICENSE"; DestDir: "{app}\third_party\highlightjs"; Flags: ignoreversion
Source: "dist\third_party\highlightjs\VERSION.txt"; DestDir: "{app}\third_party\highlightjs"; Flags: ignoreversion
Source: "dist\third_party\katex\LICENSE"; DestDir: "{app}\third_party\katex"; Flags: ignoreversion
Source: "dist\third_party\katex\VERSION.txt"; DestDir: "{app}\third_party\katex"; Flags: ignoreversion
Source: "dist\third_party\lexilla\License.txt"; DestDir: "{app}\third_party\lexilla"; Flags: ignoreversion
Source: "dist\third_party\lexilla\version.txt"; DestDir: "{app}\third_party\lexilla"; Flags: ignoreversion
Source: "dist\third_party\lua\LICENSE"; DestDir: "{app}\third_party\lua"; Flags: ignoreversion
Source: "dist\third_party\lua\VERSION.txt"; DestDir: "{app}\third_party\lua"; Flags: ignoreversion
Source: "dist\third_party\mermaid\LICENSE"; DestDir: "{app}\third_party\mermaid"; Flags: ignoreversion
Source: "dist\third_party\mermaid\VERSION.txt"; DestDir: "{app}\third_party\mermaid"; Flags: ignoreversion
Source: "dist\third_party\scintilla\License.txt"; DestDir: "{app}\third_party\scintilla"; Flags: ignoreversion
Source: "dist\third_party\scintilla\version.txt"; DestDir: "{app}\third_party\scintilla"; Flags: ignoreversion
Source: "dist\third_party\scowl\COPYING"; DestDir: "{app}\third_party\scowl"; Flags: ignoreversion
Source: "dist\third_party\scowl\VERSION.txt"; DestDir: "{app}\third_party\scowl"; Flags: ignoreversion
Source: "dist\third_party\webview2\LICENSE.txt"; DestDir: "{app}\third_party\webview2"; Flags: ignoreversion
Source: "dist\third_party\webview2\VERSION.txt"; DestDir: "{app}\third_party\webview2"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Markdown: the extensions editor_language.c's LANGUAGES table treats as Markdown.
; Registered through OpenWithProgids, so Tinker Editor is offered for them
; without overriding a default the user already chose.
Root: HKA; Subkey: "Software\Classes\TinkerEditor.md"; ValueType: string; ValueName: ""; ValueData: "Markdown Document"; Flags: uninsdeletekey; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\TinkerEditor.md\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\TinkerEditor.md\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\.md\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.md"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\.markdown\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.md"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\.mdown\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.md"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\.mkd\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.md"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\.mdwn\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.md"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\.text\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.md"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocmd
; Every other language in the LANGUAGES table, plus HTML. These only add
; Tinker Editor to each type's "Open with" list. A non-Markdown file named on the
; command line opens in the editor rather than preview-only.
Root: HKA; Subkey: "Software\Classes\TinkerEditor.source"; ValueType: string; ValueName: ""; ValueData: "Text Document"; Flags: uninsdeletekey; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\TinkerEditor.source\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\TinkerEditor.source\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.txt\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.log\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.json\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.jsonc\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.jsonl\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.geojson\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.webmanifest\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.xml\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.xsd\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.xsl\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.xslt\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.svg\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.rss\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.atom\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.plist\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.csproj\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.props\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.targets\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.html\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.htm\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.xhtml\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.yml\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.yaml\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.c\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.h\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.cc\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.cpp\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.cxx\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.hpp\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.hxx\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.js\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.mjs\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.cjs\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.ts\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.tsx\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.jsx\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.cs\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.java\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.go\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.rs\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.php\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.py\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.pyw\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.pyi\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.sh\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.bash\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.zsh\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.ksh\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.bashrc\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.profile\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.ps1\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.psm1\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.psd1\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.lua\OpenWithProgids"; ValueType: string; ValueName: "TinkerEditor.source"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; The preview's WebView2 browser profile - a cache, safe to remove (see
; init_webview() in tinkeredit.c).
Type: filesandordirs; Name: "{localappdata}\TinkerEditor\WebView2"
; get_settings_path() (settings.c) writes here, under %APPDATA%, not {app} -
; the app is never installed with write access to Program Files, so settings
; were never going to end up next to the exe.
Type: files; Name: "{userappdata}\tinkeredit\settings.json"
; %LOCALAPPDATA%\TinkerEditor\Recovery (unsaved-document snapshots) is left in
; place deliberately: it can hold work that was never saved anywhere else.
