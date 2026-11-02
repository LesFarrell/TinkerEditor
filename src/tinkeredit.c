/*
 * tinkeredit - a tiny live-preview Markdown editor for Win32.
 * Left pane: plain-text edit control for Markdown source.
 * Right pane: WebView2-hosted HTML preview (supports Mermaid diagrams,
 * with native browser zoom/pan/scroll), re-rendered on every edit.
 *
 * Build with Zig on PATH by running build.bat (see premake5.lua for the
 * alternative MinGW-w64 GCC build).
 */

#define COBJMACROS

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dlgs.h> /* cmb1: the Font common dialog's font-name combo control ID, see do_choose_font() */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <wctype.h>
#include <process.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <spellcheck.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <dwmapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
/* Windows builds before 20H1 (19041) shipped the same attribute under this
   ordinal instead; harmless to set on newer builds since only one of the two
   actually takes effect there. */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1
#define DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 19
#endif
#include "editor_commands.h"
#include "app_accelerators.h"
#include "text_search.h"
#include "editor_language.h"
#include "preview_fallback.h"
#include "file_explorer.h"
#include "file_io.h"
#include "recovery.h"
#include "markdown.h"
#include "menu_commands.h"
#include "outline_minimap.h"
#include "output_process.h"
#include "output_text.h"
#include "path_utils.h"
#include "terminal_grid.h"
#include "terminal_process.h"
#include "terminal_view.h"
#include "progress_dialog.h"
#include "scintilla_config.h"
#include "plugin_host.h"
#include "plugin_settings.h"
#include "settings.h"
#include "recent_list.h"
#include "text_codec.h"
#include "git_status.h"
#include "workspace_text.h"
#include "markdown_links.h"
#include "theme.h"
#include "themed_scrollbar.h"
#include "toolbar_icons.h"

#include <WebView2.h>
#include "../third_party/scintilla/include/Scintilla.h"
#include "../third_party/lexilla/include/Lexilla.h"
#include "../third_party/lexilla/include/SciLexer.h"

#include "tinkeredit.h"

typedef struct AppWindows {
    HWND edit, preview, tabs, status, outline, minimap, splitEdit, lastEditView;
    HWND debugStatus, debugVariables;
    HWND debugContinue, debugStepOver, debugStepInto, debugStepOut;
    HWND outputPanel, outputInput;
} AppWindows;
static AppWindows g_windows;
#define g_hEdit g_windows.edit
#define g_hPreview g_windows.preview
#define g_hTabs g_windows.tabs
#define g_hStatus g_windows.status
#define g_hOutline g_windows.outline
#define g_hMinimap g_windows.minimap
#define g_hSplitEdit g_windows.splitEdit
#define g_lastEditView g_windows.lastEditView
#define g_hDebugStatus g_windows.debugStatus
#define g_hDebugVariables g_windows.debugVariables
#define g_hDebugContinue g_windows.debugContinue
#define g_hDebugStepOver g_windows.debugStepOver
#define g_hDebugStepInto g_windows.debugStepInto
#define g_hDebugStepOut g_windows.debugStepOut
#define g_hOutputPanel g_windows.outputPanel
#define g_hOutputInput g_windows.outputInput
HWND g_hMainWnd;
/* The main window's accelerator table, forwarded into from the preview's
   AcceleratorKeyPressed handler - see AccelKey_Invoke() - since disabling
   WebView2's own browser accelerators (AreBrowserAcceleratorKeysEnabled)
   stops it acting on them but does not, by itself, deliver them to the
   host's message loop. Set once in WinMain. */
static HACCEL g_hAccel = NULL;
static int g_lastPluginLoadSucceeded = 1;
/* Set when plugin code changes save metadata without modifying Scintilla's
   text buffer, so dirty-state reconciliation preserves that change. */
static int g_pluginChangedSaveEncoding = 0;
static int g_pluginConfigChanged = 0;
/* Metadata context paired with the editor view bound into a running Lua
   command/predicate; -1 means the selected main tab. */
static int g_pluginCommandDocument = -1;
static HACCEL create_main_accelerators(void);
static void rebuild_main_accelerators(void);
/* Font face/weight/point-size: see settings.h for MAX_FONT_FACE_NAME's
   LF_FACESIZE-related commentary. Weight/italic are captured separately
   from the face name in do_choose_font() - a family that packages each
   weight as a same-named style (e.g. "Google Sans Code Monospaced" with a
   "Medium" style) needs the weight applied separately or the choice is
   silently lost to Regular on every reload. */
WCHAR g_fontFaceName[MAX_FONT_FACE_NAME] = L"Consolas";
int g_fontPointSize = 10;
int g_fontWeight = FW_NORMAL;
int g_fontItalic = 0;

static ICoreWebView2 *g_webview = NULL;
static ICoreWebView2Controller *g_webviewController = NULL;
static ICoreWebView2_3 *g_webview3 = NULL;
static ICoreWebView2ExecuteScriptCompletedHandler g_scriptHandler;
static int g_webviewReady = 0;
static int g_ignoreNextEditorScroll = 0;
static EventRegistrationToken g_webMessageToken;
static int g_webMessageRegistered = 0;
static HBRUSH g_previewBkBrush = NULL; /* matches the preview host's WM_CTLCOLORSTATIC background to the theme, so it doesn't flash white before WebView2 paints */
static HBRUSH g_mainBkBrush = NULL; /* fills the toolbar strip and splitter gap in the main window, matching the theme instead of the system COLOR_WINDOW default */
static HBRUSH g_menuBkBrush = NULL; /* owner-drawn top-level menu bar background */
static HFONT g_menuFont = NULL;
static int g_menuFontDpi = 0;
static char *g_pendingHtmlBody = NULL; /* set if update requested before webview ready */
static int g_previewShowingRawHtml = 0; /* true once show_html_file_preview() has navigated away from the shell page */
static WCHAR *g_expectedHtmlPreviewUrl = NULL; /* one-shot allowance for show_html_file_preview()'s own navigation; see NavStarting_Invoke */
static WCHAR *g_documentMappingDirectory = NULL; /* avoids remapping document.local when tabs share a folder */
static WCHAR *g_exportPath = NULL;
static unsigned long g_exportRequest = 0;
static WCHAR *g_pendingPdfPath = NULL;
typedef enum PrintAction {
    PRINT_ACTION_NONE = 0,
    PRINT_ACTION_DIALOG,
    PRINT_ACTION_EXPORT_PDF
} PrintAction;

static PrintAction g_pendingPrintAction = PRINT_ACTION_NONE;
int g_previewDarkMode = 0; /* the rendered Markdown preview's own theme, independent of the editor/app chrome below */
int g_editorDarkMode = 0; /* the Scintilla editor's colours, plus all surrounding app chrome (menus, dialogs, tabs, toolbar, title bar) */
/* CompilerBackendPreference is declared in settings.h; see load_compiler_settings()
   and find_native_compiler() further down. Auto keeps the existing GCC/Clang/
   Zig/MSVC priority search; the others pin Run/Debug to just one of them. */
CompilerBackendPreference g_compilerBackend = COMPILER_BACKEND_AUTO;
WCHAR *g_vcvarsallOverridePath = NULL; /* [Compiler] VcvarsallPath override; skips auto-detection */
WCHAR *g_gccBinOverride = NULL;   /* [Compiler] GccPath: folder to search for gcc.exe/g++.exe instead of PATH */
WCHAR *g_clangBinOverride = NULL; /* [Compiler] ClangPath: folder to search for clang.exe/clang++.exe instead of PATH */
WCHAR *g_zigBinOverride = NULL;   /* [Compiler] ZigPath: folder to search for zig.exe instead of PATH */
WCHAR *g_luaBinOverride = NULL;   /* [Compiler] LuaPath: folder to search for a Lua interpreter instead of PATH */
WCHAR *g_gdbBinOverride = NULL;   /* [Compiler] GdbPath: optional folder overriding bundled/PATH GDB */
WCHAR *g_compilerExtraFlags = NULL; /* [Compiler] ExtraFlags appended to Run/Debug C/C++ builds; set via Settings > Compiler */
WCHAR *g_customCssPath = NULL;
static WCHAR *g_debugLocationPath = NULL;
static WCHAR *g_debugWorkingDirectory = NULL;
static WCHAR *g_debugCommandPath = NULL;
static WCHAR *g_debugOutputPath = NULL;
static WCHAR *g_lastDebugSource = NULL;
static HANDLE g_debugProcess = NULL;
static HANDLE g_debugJob = NULL; /* owns the hidden GDB process tree so it cannot outlive Tinker Editor */
static int g_lastDebugLine = -1;
static int g_activeDebuggerKind = 0; /* 1 = pdb, 2 = GDB */
static char *g_lastDebugState = NULL;
static void apply_output_panel_font(const char *faceUtf8);
static HWND active_output_focus_target(void);
static void show_output_panel(int tab, int focusInput);
static int run_in_output_panel(const WCHAR *file, const WCHAR *parameters,
                               const WCHAR *directory, const WCHAR *title);
static void restore_persisted_breakpoints(const WCHAR *path);
static void save_breakpoints_for_active_document(const WCHAR *path);
static void save_all_document_breakpoints(void);
#define MAX_DOCUMENTS RECOVERY_MAX_DOCUMENTS
typedef struct Document {
    sptr_t sciDocument;
    WCHAR *path;
    int dirty;
    int hasBom; /* the file has a byte-order mark for its encoding (UTF-8 or UTF-16) */
    int encoding; /* the TextEncoding the file is stored in; the editor holds UTF-8 */
    int encodingForced; /* chosen with Reopen with Encoding, so reloads keep it */
    FILETIME fileTime;
    int hasFileTime;
    int externalChanged;
    int externalDeleted;
    Sci_Position caret;
    Sci_Position anchor;
    Sci_Position firstVisibleLine;
    int xOffset;
    unsigned untitledNumber;
    int closedIndex;
    int pinned; /* protected from ordinary close commands until explicitly unpinned */
    unsigned long long lastActivated; /* MRU order for the searchable tab switcher */
    int isReference; /* 0, or a ReferenceKind for a Help page so Help reuses its tab */
    int viewKind; /* DOCUMENT_VIEW_*: text editor or a read-only image preview */
    int largeFile; /* using large-file mode, either from open or after growing past the threshold */
    int autoReload; /* reload changes on disk without asking while there are no unsaved edits */
    /* Where the last load from disk ended, so growth can be appended rather
       than reloaded (see append_file_growth()); loadedTailLength 0 means unknown. */
    long long loadedBytes;
    Sci_Position loadedLength;
    unsigned char loadedTail[64];
    int loadedTailLength;
    TextCodecState appendCodec;
} Document;

enum { DOCUMENT_VIEW_TEXT = 0, DOCUMENT_VIEW_IMAGE };

/* The Help pages opened as unsaved Markdown tabs (see show_help_page()). */
typedef enum { REFERENCE_MARKDOWN = 1, REFERENCE_SHORTCUTS } ReferenceKind;

static const WCHAR *reference_title(int kind) {
    return kind == REFERENCE_SHORTCUTS ? L"Keyboard Shortcuts" : L"Markdown Reference";
}

typedef struct DocumentStore {
    Document documents[MAX_DOCUMENTS];
    int count;
    int active;
    unsigned nextUntitledNumber;
    int switching;
} DocumentStore;
static DocumentStore g_documentStore = { .nextUntitledNumber = 1 };
static unsigned long long g_tabActivationSerial = 0;

static int image_file_path(const WCHAR *path) {
    const WCHAR *extension = path ? PathFindExtensionW(path) : NULL;
    return extension && extension_in_list(extension,
        L".png;.jpg;.jpeg;.gif;.bmp;.webp;.svg;.ico;.avif");
}

static int active_document_is_image(void) {
    return g_documentStore.count > 0 &&
           g_documentStore.documents[g_documentStore.active].viewKind == DOCUMENT_VIEW_IMAGE;
}

static int active_document_is_raster_image(void) {
    const WCHAR *path = g_documentStore.count > 0
        ? g_documentStore.documents[g_documentStore.active].path : NULL;
    return active_document_is_image() && path &&
           _wcsicmp(PathFindExtensionW(path), L".svg") != 0;
}
static char g_tabTooltipAnsi[LONG_PATH_CAPACITY];
/* Set on every edit and every dirty-state change; cleared once
   write_recovery_snapshot() has brought the on-disk snapshot back in sync.
   Lets the 5s autosave timer skip its work entirely while idle instead of
   rewriting unchanged documents every tick. */
static int g_recoverySnapshotStale = 0;
/* This instance's own recovery folder, opened by the first snapshot that has
   documents to write (see recovery_host.inc). */
static RecoverySession g_recoverySession = { NULL, INVALID_HANDLE_VALUE };

#define MAX_CLOSED_DOCUMENTS 10
static Document g_closedDocuments[MAX_CLOSED_DOCUMENTS];
static int g_closedDocumentCount = 0;

/* Documents a [[wiki-link]] jump was made from, most recent last, so
   "Go Back" can return to wherever the last jump started - see
   open_wikilink_target() and go_wiki_back(). Only documents with a saved
   path can be recorded; jumping from an unsaved document simply isn't
   undoable this way, the same limitation the recent-files list has. */
#define MAX_WIKI_BACK 10
static WCHAR *g_wikiBackStack[MAX_WIKI_BACK];
static int g_wikiBackCount = 0;

/* Most editor operations naturally target the selected tab. These aliases
   keep that code direct while the storage itself remains per-document.
   Despite the g_ names they are not globals: each one reads or writes a
   field of whichever document is active at that moment, so code that
   switches tabs (or touches a background document) must go through
   g_documentStore.documents[i] instead. */
#define g_currentFile (g_documentStore.documents[g_documentStore.active].path)
#define g_dirty       (g_documentStore.documents[g_documentStore.active].dirty)
#define g_hasBom      (g_documentStore.documents[g_documentStore.active].hasBom)
#define g_encoding    (g_documentStore.documents[g_documentStore.active].encoding)
#define g_largeFile   (g_documentStore.documents[g_documentStore.active].largeFile)
/* LARGE_FILE_FEATURES: a file of at least this many MiB opens in large-file
   mode - streamed straight into a style-less, 64-bit-position Scintilla
   document (see read_large_file_document()). A smaller ordinary document
   that grows past the threshold also enters the same reduced-feature mode
   without destructively replacing its buffer or undo history. Every
   full-document feature that would otherwise rescan it on each pause in typing is switched off: lexing
   and folding, word wrap, preview, outline, word count, word completion,
   brace matching, smart Enter/Tab, and recovery snapshots. Plain editing, find/replace, and
   save still work. Persisted as [View] LargeFileThresholdMB. */
int g_largeFileThresholdMB = 64;

static long long large_file_threshold_bytes(void) {
    return (long long)g_largeFileThresholdMB * 1024 * 1024;
}
int g_previewVisible = 0;
int g_editorVisible = 1;
/* Image tabs temporarily occupy the preview area without changing the user's
   saved editor/preview layout. Switching back restores that layout. */
static int editor_pane_visible(void) {
    return g_editorVisible && !active_document_is_image();
}

static int preview_pane_visible(void) {
    return g_previewVisible || active_document_is_image();
}
int g_previewOnLeft = 0;
int g_splitterRatio = 500; /* left pane width as thousandths of the available split width */
int g_splitterPos = -1; /* pixel x of splitter, initialized from g_splitterRatio */
int g_splitEditorVisible = 0;
int g_splitEditorRatio = 500; /* main pane size as thousandths of the editor region's split axis */
int g_splitEditorVertical = 0; /* nonzero places the second pane to the right instead of below */
static int g_splitEditorPos = -1; /* pixel position along the split axis, local to the editor region */
static RECT g_splitEditorSplitterRect = {0};
/* The hidden split pane rests on its own empty document instead of observing
   every edit made to the active tab. This explicit reference keeps that
   document alive while the pane is temporarily attached elsewhere. */
static sptr_t g_splitScratchDocument = 0;
/* Optional document retained in the secondary editor while another tab is
   selected in the main pane. It is always one of DocumentStore's live
   Scintilla documents; closing that tab clears this pointer first. */
static sptr_t g_splitDocument = 0;

static HWND tracked_edit_view(void) {
    if (g_lastEditView == g_hSplitEdit && g_hSplitEdit && g_splitEditorVisible)
        return g_hSplitEdit;
    return g_hEdit;
}
static RECT g_splitterRect = {0}; /* the main editor/preview splitter's own bounds, for drawing its line */
static RECT g_explorerSplitterRect = {0}; /* the explorer panel's splitter, empty while the panel is hidden */
static POINT g_splitEditorRegionOrigin = {0}; /* window-relative editor origin, refreshed every layout() */
static int g_draggingSplitEditor = 0;
static int g_draggingExplorer = 0;
static RECT g_outputSplitterRect = {0}; /* above the output panel, empty while it is hidden */
static int g_draggingOutput = 0;
int g_outputPanelVisible = 0;
int g_outputPanelHeight = -1; /* pixels; -1 until the user drags the splitter */
int g_runInConsole = 0; /* Run in a separate console window instead of the output panel */
int g_commandLinePreviewMode = 0;
RECT g_savedWindowRect;
int g_haveSavedWindowRect = 0;
int g_startMaximized = 0;
/* File named on the command line, if any; handed off from WinMain to the
   deferred startup init that runs after the window is first shown (see
   run_deferred_startup_init()), and freed there. */
static WCHAR *g_pendingCliFile = NULL;
static int g_comInitialized = 0;
int g_focusMode = 0;
/* Markdown is prose: wrapping long paragraphs is the useful default. */
int g_wordWrap = 0;
int g_showWhitespace = 0;
int g_indentGuides = 0;
int g_markChangedLines = 1;
int g_indentUseSpaces = 0;
int g_tabWidth = 4;
int g_spellCheckEnabled = 1;
int g_outlineVisible = 0;
int g_minimapVisible = 1;
static int g_minimapDragging = 0;
static int g_minimapViewTop = 0; /* client-relative y (minimap coords) of the visible-region box */
static int g_minimapViewHeight = 0;
int g_editorZoom = 0; /* Scintilla SCI_SETZOOM units: +1/-1 per level, 0 is 100%. */
WCHAR g_spellLanguage[LOCALE_NAME_MAX_LENGTH] = L"";
static Sci_Position g_outlineLines[OUTLINE_MAX_HEADINGS];
static int g_outlineCount = 0;
static int g_tabDragIndex = -1;
static int g_tabDragged = 0;
static int g_activeLanguage = 0; /* Language of the active document; 0 is LANG_MARKDOWN. */
static ISpellChecker *g_spellChecker = NULL;
/* SCN_CHARADDED must return promptly. Remember the view/document that caused
   it so word completion can scan after a short pause instead of inside the
   keypress notification itself. */
static HWND g_pendingCompletionView = NULL;
static sptr_t g_pendingCompletionDocument = 0;

/* Long file operations report through progress_dialog.c, which wraps the
   shell's IProgressDialog. Its host table is filled in once the main window
   exists (see progress_dialog_init in WinMain). */
static ProgressDialogHost g_progressDialogHost;

/* scintilla_config.c configures each editor view through this host; the
   persisted settings it reads come from settings_values.h directly. The host
   is filled in before any view is created (see initialize_main_window). */
static int scintilla_config_large_file_active(HWND hEdit) {
    sptr_t document = hEdit ? (sptr_t)SendMessageA(hEdit, SCI_GETDOCPOINTER, 0, 0) : 0;
    for (int i = 0; document && i < g_documentStore.count; i++)
        if (g_documentStore.documents[i].sciDocument == document)
            return g_documentStore.documents[i].largeFile;
    return g_documentStore.count > 0 ? g_largeFile : 0;
}
static const ScintillaConfigHost g_scintillaConfigHost = {
    .splitEdit = &g_windows.splitEdit,
    .largeFileActive = scintilla_config_large_file_active,
};

#define MAX_RECENT_FILES 10
static WCHAR *g_recentFiles[MAX_RECENT_FILES];
static int g_recentFileCount = 0;

#define MAX_RECENT_FOLDERS 10
static WCHAR *g_recentFolders[MAX_RECENT_FOLDERS];
static int g_recentFolderCount = 0;
static WCHAR *g_currentWorkspaceFolder = NULL;
/* Updated by the hidden asynchronous Git status process. The status bar can
   paint this without waiting for git, and document tabs use the path table
   below for their one-character working-tree decorations. */
static WCHAR g_gitStatusText[128] = L"";
/* Kept at its full ref length because branch actions must never operate on a
   display-truncated name. The status bar truncates naturally when painted. */
static WCHAR *g_gitBranchName;
#define GIT_STATUS_FILE_MAX 4096
/* status is the one-character working-tree decoration tabs use (worktree
   over index); index/worktree keep the raw porcelain X/Y codes (' ' when
   absent) so the Git panel can show staged and unstaged changes apart. */
typedef struct { WCHAR *path; WCHAR status; WCHAR index; WCHAR worktree; } GitStatusFile;
static GitStatusFile g_gitStatusFiles[GIT_STATUS_FILE_MAX];
static int g_gitStatusFileCount = 0;

static WCHAR git_status_for_path(const WCHAR *path) {
    if (!path || !g_currentWorkspaceFolder) return 0;
    size_t rootLength = wcslen(g_currentWorkspaceFolder);
    if (_wcsnicmp(path, g_currentWorkspaceFolder, rootLength) != 0) return 0;
    const WCHAR *relative = path + rootLength;
    while (*relative == L'\\' || *relative == L'/') relative++;
    for (int i = 0; i < g_gitStatusFileCount; i++)
        if (_wcsicmp(relative, g_gitStatusFiles[i].path) == 0) return g_gitStatusFiles[i].status;
    return 0;
}

static HWND g_hFindDlg = NULL;
WCHAR g_findWhat[256] = L"";
WCHAR g_replaceWith[256] = L"";
int g_findOptions = 0;
int g_findInSelection = 0;

/* ---- recently opened files and workspace folders, most-recent-first.
   Both lists share one implementation; only the backing array, its cap,
   and the settings.json section/key names differ. ---- */
static const RecentList g_recentFileList = {g_recentFiles, &g_recentFileCount, MAX_RECENT_FILES,
                                            L"RecentFiles", L"File"};
static const RecentList g_recentFolderList = {g_recentFolders, &g_recentFolderCount, MAX_RECENT_FOLDERS,
                                              L"RecentFolders", L"Folder"};

/* Drop any recent-file entries whose file no longer exists on disk. Called
   right before the menu is (re)built so the list stays accurate without
   needing to stat every entry on every keystroke. */
static void prune_missing_recent_files(void) {
    int i = 0;
    while (i < g_recentFileCount) {
        WCHAR *ioPath = extended_path(g_recentFiles[i]);
        DWORD attrs = ioPath ? GetFileAttributesW(ioPath) : INVALID_FILE_ATTRIBUTES;
        free(ioPath);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            recent_list_remove(&g_recentFileList, g_recentFiles[i]);
        } else {
            i++;
        }
    }
}

static void load_workspace_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WCHAR *value = read_profile_string(L"Workspace", L"CurrentFolder", path);
    /* Give the file explorer a useful root on a fresh install without
       interrupting its first open with a folder-picker dialog. */
    replace_wstring(&g_currentWorkspaceFolder, value && value[0] ? value : L"C:\\");
    SetCurrentDirectoryW(g_currentWorkspaceFolder);
    free(value);
    free(path);
}

static void save_workspace_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    write_setting_string(L"Workspace", L"CurrentFolder",
                         g_currentWorkspaceFolder ? g_currentWorkspaceFolder : L"", path);
    free(path);
}

/* The standard error message box, owned by the main window. Messages about
   one feature (Run, Debug, Plugins) use their own title instead. */
static void show_error(const WCHAR *message) {
    MessageBoxW(g_hMainWnd, message, L"Tinker Editor", MB_OK | MB_ICONERROR);
}

#include "internal/editor_view.inc"

#include "internal/control_theme.inc"

#include "internal/preview.inc"

#include "internal/documents.inc"

#include "internal/recovery_host.inc"

#include "internal/spell_check.inc"

#include "internal/dialogs.inc"

#include "internal/help_and_menu.inc"

#include "internal/ui_shell.inc"
#include "internal/debugger.inc"
#include "internal/output_panel.inc"
#include "internal/git_panel.inc"

#include "internal/outline_host.inc"

#include "internal/workspace.inc"

#include "internal/settings_dialog.inc"
#include "internal/command_dispatch.inc"

#include "internal/plugin_bridge.inc"

#include "internal/editor_input.inc"
static void explorer_request_open(const WCHAR *path) {
    open_file_in_tab(path);
}

static void explorer_request_change_root(const WCHAR *path) {
    adopt_workspace_folder(g_hMainWnd, path);
}

static int window_dpi(HWND hwnd) {
    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(hwnd, hdc);
    return dpi;
}

static void create_navigation_controls(HWND hwnd) {
    g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd, (HMENU)ID_STATUS, GetModuleHandleW(NULL), NULL);
    SetWindowSubclass(g_hStatus, StatusSubclassProc, 1, 0);

    /* LBS_HASSTRINGS keeps the listbox owning a copy of each LB_ADDSTRING
       text (as before) so draw_themed_listbox_item() can fetch it back via
       LB_GETTEXT - without it, an owner-draw listbox instead treats the
       LB_ADDSTRING data as an opaque per-item value, which here is a
       heap pointer that update_outline() frees right after inserting it. */
    g_hOutline = CreateWindowExW(0, L"LISTBOX", L"",
        WS_CHILD | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
        LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        0, 0, 0, 0, hwnd, (HMENU)ID_OUTLINE, GetModuleHandleW(NULL), NULL);
    theme_list_frame(g_hOutline);
    themed_scrollbar_attach(g_hOutline, THEMED_SCROLL_LISTBOX);
}

static void create_document_tabs(HWND hwnd, int dpi) {
    g_hTabs = CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER | TCS_TOOLTIPS |
        TCS_FIXEDWIDTH | TCS_FORCELABELLEFT | TCS_RAGGEDRIGHT | TCS_OWNERDRAWFIXED,
        0, 0, 0, 0, hwnd, (HMENU)ID_TABS, GetModuleHandleA(NULL), NULL);
    /* A compact fixed width keeps more documents accessible without making
       short filenames consume excessive space. The tooltip below supplies
       the full path whenever the visible label is clipped. */
    SendMessageW(g_hTabs, TCM_SETITEMSIZE, 0,
        MAKELPARAM(MulDiv(TAB_ITEM_WIDTH, dpi, 96), MulDiv(TAB_ITEM_HEIGHT, dpi, 96)));
    HWND tabTooltips = TabCtrl_GetToolTips(g_hTabs);
    if (tabTooltips) {
        SendMessageW(tabTooltips, CCM_SETUNICODEFORMAT, TRUE, 0);
        SendMessageW(tabTooltips, TTM_SETMAXTIPWIDTH, 0, TAB_TOOLTIP_MAX_WIDTH);
    }
    SetWindowSubclass(g_hTabs, TabSubclassProc, 1, 0);
}

static void create_editor_views(HWND hwnd) {
    /* No WS_EX_CLIENTEDGE on either pane: its sunken border is drawn in fixed
       system greys, which reads as a light seam against the dark theme. */
    g_hEdit = CreateWindowExA(0, "Scintilla", "",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)ID_EDIT, GetModuleHandleA(NULL), NULL);
    SetWindowSubclass(g_hEdit, EditSubclassProc, 2, 0);
    configure_scintilla(g_hEdit);

    g_hMinimap = CreateWindowExA(0, "Scintilla", "",
        WS_CHILD | (g_minimapVisible ? WS_VISIBLE : 0),
        0, 0, 0, 0, hwnd, (HMENU)ID_MINIMAP, GetModuleHandleA(NULL), NULL);
    SetWindowSubclass(g_hMinimap, MinimapSubclassProc, 1, 0);
    configure_minimap(g_hMinimap);

    g_hSplitEdit = CreateWindowExA(0, "Scintilla", "",
        WS_CHILD | (g_splitEditorVisible ? WS_VISIBLE : 0),
        0, 0, 0, 0, hwnd, (HMENU)ID_SPLIT_EDIT, GetModuleHandleA(NULL), NULL);
    SetWindowSubclass(g_hSplitEdit, EditSubclassProc, 3, 0);
    configure_scintilla(g_hSplitEdit);
    g_splitScratchDocument = (sptr_t)SendMessageA(g_hSplitEdit, SCI_GETDOCPOINTER, 0, 0);
    SendMessageA(g_hSplitEdit, SCI_ADDREFDOCUMENT, 0, (LPARAM)g_splitScratchDocument);
    g_lastEditView = g_hEdit;

    apply_editor_font();
}

static void initialize_first_document(void) {
    g_documentStore.count = 1;
    g_documentStore.active = 0;
    memset(&g_documentStore.documents[0], 0, sizeof(g_documentStore.documents[0]));
    g_documentStore.documents[0].sciDocument = (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0);
    SendMessageA(g_hEdit, SCI_ADDREFDOCUMENT, 0, (LPARAM)g_documentStore.documents[0].sciDocument);
    sync_split_editor_document();
    /* The sole blank startup tab is the unnumbered canonical document. */
    g_documentStore.documents[0].untitledNumber = 0;
    g_documentStore.documents[0].lastActivated = ++g_tabActivationSerial;
    TCITEMW firstTab = {0};
    firstTab.mask = TCIF_TEXT;
    firstTab.pszText = L"";
    SendMessageW(g_hTabs, TCM_INSERTITEMW, 0, (LPARAM)&firstTab);
    update_tab_label(0);

    reset_undo_history(g_hEdit);
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    /* Establish a clean initial document state before the window is shown;
       New/Open perform the same reset after replacing text. */
    set_dirty(0);
    update_preview();
}

static void create_preview_control(HWND hwnd) {
    apply_preview_host_theme();
    apply_main_theme(hwnd);
    g_hPreview = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)ID_PREVIEW, GetModuleHandleA(NULL), NULL);
}

static void initialize_main_window(HWND hwnd) {
    g_hMainWnd = hwnd;

    /* Point outline_minimap.c at the shell state it drives before any child
       view (the minimap in particular) is created. */
    init_outline_minimap();

    /* Likewise point scintilla_config.c at the split view and large-file flag
       before the editor and split views are configured. */
    scintilla_config_init(&g_scintillaConfigHost);

    /* The process preference is selected before window creation; complete the
       per-window opt-in before attaching the menu so its first paint matches. */
    apply_menu_theme(hwnd, g_editorDarkMode);
    int dpi = window_dpi(hwnd);

    if (!g_focusMode) SetMenu(hwnd, build_menu());
    create_toolbar(hwnd);
    create_navigation_controls(hwnd);

    explorer_init(hwnd, explorer_request_open, show_text_input, reveal_path_in_explorer,
                  explorer_path_renamed, explorer_request_change_root);
    if (g_explorerVisible && g_currentWorkspaceFolder) explorer_set_root(g_currentWorkspaceFolder);

    create_document_tabs(hwnd, dpi);
    create_output_panel(hwnd); /* before the editor views, which apply the font to it */
    create_editor_views(hwnd);
    initialize_first_document();
    create_preview_control(hwnd);

    SetTimer(hwnd, IDT_FILE_WATCH, FILE_WATCH_INTERVAL_MS, NULL);
    SetTimer(hwnd, IDT_AUTOSAVE, AUTOSAVE_INTERVAL_MS, NULL);
    update_status_bar();
    update_outline();
    layout(hwnd);
}

/* Runs the slow, non-essential startup work that would otherwise keep the
   window from appearing promptly: spell-checker COM activation, the
   WebView2 environment/controller handshake, and reopening the
   command-line file or the previous session's tabs. WinMain posts
   WM_APP_DEFERRED_INIT right after creating the window instead of running
   this inline, so it runs on the first pass through the message loop -
   after the window is already shown - rather than stretching out the
   startup wait cursor while nothing is on screen yet. */
static void run_deferred_startup_init(void) {
    detect_high_contrast();
    if (g_comInitialized) init_spell_checker();
    /* The menu was built before the spell checker existed. */
    update_view_menu(g_hMainWnd);
    init_webview();
    if (g_pendingCliFile) {
        open_file_in_tab(g_pendingCliFile);
        free(g_pendingCliFile);
        g_pendingCliFile = NULL;
    } else if (!restore_recovery_snapshot()) {
        restore_session_documents();
    }
    refresh_git_status();
}

static void handle_editor_ui_update(HWND hwnd, HWND editor, const struct SCNotification *scn) {
    /* When the views share a document both produce UI notifications. Only
       the pane the user is actually working in should drive caret,
       brace, preview-scroll, and plugin-selection updates. Live spell
       checking is the exception: an unfocused split pane can still be
       scrolled (mouse wheel, dragging its own scrollbar) without moving
       focus, and the text newly brought into view there needs checking too. */
    if (GetFocus() == editor && g_lastEditView != editor) {
        g_lastEditView = editor;
        if (g_hTabs) InvalidateRect(g_hTabs, NULL, FALSE);
    }
    if (scn->updated & SC_UPDATE_V_SCROLL) schedule_visible_spell_check();
    if (editor != tracked_edit_view()) return;
    /* A secondary document has no relationship to the selected tab's live
       preview, outline or plugin selection events. */
    if (document_index_for_view(editor) != g_documentStore.active) return;
    if ((scn->updated & SC_UPDATE_V_SCROLL) && g_editorVisible && g_previewVisible) {
            if (g_ignoreNextEditorScroll) g_ignoreNextEditorScroll = 0;
            else SetTimer(hwnd, IDT_SCROLL_SYNC, SCROLL_SYNC_DELAY_MS, NULL);
    }
    if (scn->updated & SC_UPDATE_SELECTION) {
        /* Brace matching and column calculation can both scan beyond the
           changed byte. Coalesce them with the existing selection debounce
           instead of doing them inside every key notification. */
        SetTimer(hwnd, IDT_PLUGIN_SELECTION, PLUGIN_SELECTION_DELAY_MS, NULL);
    }
    if (g_hMinimap && g_minimapVisible && IsWindowVisible(g_hMinimap) &&
        (scn->updated & SC_UPDATE_V_SCROLL))
        update_minimap_viewport();
}

static void schedule_word_completions(HWND hwnd, HWND editor) {
    if (g_largeFile) return;
    g_pendingCompletionView = editor;
    g_pendingCompletionDocument = (sptr_t)SendMessageA(editor, SCI_GETDOCPOINTER, 0, 0);
    SetTimer(hwnd, IDT_WORD_COMPLETION, WORD_COMPLETION_DELAY_MS, NULL);
}

static void handle_editor_margin_click(const struct SCNotification *scn) {
    if (scn->margin != MARGIN_BOOKMARKS) return;

    /* This handler is used only for the selected tab; an independently
       retained split document ignores marker clicks. */
    Sci_Position line = (Sci_Position)SendMessageA(
        g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)scn->position, 0);
    if (!(scn->modifiers & SCMOD_SHIFT) &&
        is_debuggable_source(debug_source_kind(g_currentFile)))
        toggle_breakpoint_at_line(line);
    else
        toggle_bookmark_at_line(line);
}

static void enter_large_file_mode_if_needed(HWND hwnd) {
    if (g_documentStore.switching || g_largeFile) return;

    Sci_Position length = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    Sci_Position threshold = (Sci_Position)large_file_threshold_bytes();
    if (length < threshold) return;

    g_largeFile = 1;
    PostMessageW(hwnd, WM_APP_APPLY_LARGE_FILE_MODE, (WPARAM)g_documentStore.active, 0);
}

/* Rendering a Markdown preview takes roughly linear time in the document's
   size, so above a few megabytes a single re-render can run well past the
   150ms debounce below - and since it runs on this thread, the whole
   application stops responding until it finishes, repeating on every pause
   in typing. Below this length a live re-render stays comfortably under
   half a second; above it, schedule_updates_after_edit() leaves the preview
   showing what it last rendered instead of freezing the window on every
   keystroke pause. Opening a file, switching to its tab, exporting, or
   toggling the preview back on still renders the current text in full -
   those are one-off waits the user asked for, not a surprise mid-typing. */
#define LIVE_PREVIEW_MAX_LENGTH (4 * 1024 * 1024)

static void schedule_updates_after_edit(HWND hwnd, const struct SCNotification *scn) {
    /* HTML previews show the saved file; other previews show unsaved text. */
    Sci_Position editLength = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    if (!g_largeFile && g_previewVisible && g_activeLanguage != LANG_HTML &&
        editLength <= LIVE_PREVIEW_MAX_LENGTH)
        SetTimer(hwnd, IDT_PREVIEW_UPDATE, PREVIEW_UPDATE_DELAY_MS, NULL);
    if (!g_largeFile) schedule_visible_spell_check();

    /* Full-document status and outline scans are debounced across typing. */
    SetTimer(hwnd, IDT_STATUS_UPDATE, STATUS_UPDATE_DELAY_MS, NULL);
    if (!g_largeFile && g_outlineVisible && g_activeLanguage == LANG_MARKDOWN)
        SetTimer(hwnd, IDT_OUTLINE_UPDATE, OUTLINE_UPDATE_DELAY_MS, NULL);
    if (scn->linesAdded != 0) {
        update_linenumber_margin_width(g_hEdit);
        if (g_hSplitEdit) update_linenumber_margin_width(g_hSplitEdit);
    }
}

/* Character edits are coalesced until the editor has been idle for the timer
   interval. A newline insertion/deletion shifts every following minimap row,
   so that structural change is refreshed immediately instead. */
static void schedule_minimap_after_edit(HWND hwnd, const struct SCNotification *scn) {
    if (!g_hMinimap || !g_minimapVisible || !IsWindowVisible(g_hMinimap)) return;
    if (scn->linesAdded != 0) {
        KillTimer(hwnd, IDT_MINIMAP_UPDATE);
        refresh_minimap_after_edit();
    } else {
        defer_minimap_paint();
        /* Resetting the existing timer prevents a full minimap repaint in
           the middle of a typing or Backspace burst. */
        SetTimer(hwnd, IDT_MINIMAP_UPDATE, MINIMAP_UPDATE_DELAY_MS, NULL);
    }
}

static void handle_document_modified(HWND hwnd, const struct SCNotification *scn) {
    if (!(scn->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) return;

    schedule_minimap_after_edit(hwnd, scn);

    /* Main-view edits update the selected tab. A split view retaining another
       document uses handle_secondary_document_modified() below. */
    if (!g_documentStore.switching && !g_dirty) set_dirty(1);
    if (!g_documentStore.switching) g_recoverySnapshotStale = 1;
    enter_large_file_mode_if_needed(hwnd);
    schedule_updates_after_edit(hwnd, scn);
}

static void handle_secondary_document_modified(HWND editor,
                                                const struct SCNotification *scn) {
    if (!(scn->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) ||
        g_documentStore.switching) return;
    int index = document_index_for_view(editor);
    if (index < 0 || index == g_documentStore.active) return;
    Document *doc = &g_documentStore.documents[index];
    if (!doc->dirty) {
        doc->dirty = 1;
        update_tab_label(index);
    }
    g_recoverySnapshotStale = 1;
}

static void handle_editor_notification(HWND hwnd, NMHDR *notification) {
    int fromMain = notification->hwndFrom == g_hEdit;
    int fromSplit = g_hSplitEdit && notification->hwndFrom == g_hSplitEdit;
    if (!fromMain && !fromSplit) return;

    struct SCNotification *scn = (struct SCNotification *)notification;
    switch (notification->code) {
        case SCN_UPDATEUI:
            handle_editor_ui_update(hwnd, notification->hwndFrom, scn);
            break;
        case SCN_CHARADDED:
            if (g_lastEditView != notification->hwndFrom) {
                g_lastEditView = notification->hwndFrom;
                if (g_hTabs) InvalidateRect(g_hTabs, NULL, FALSE);
            }
            if (document_index_for_view(notification->hwndFrom) == g_documentStore.active)
                schedule_word_completions(hwnd, notification->hwndFrom);
            break;
        case SCN_MARGINCLICK:
            if (document_index_for_view(notification->hwndFrom) == g_documentStore.active)
                handle_editor_margin_click(scn);
            break;
        case SCN_MODIFIED:
            /* Every view of the shared Scintilla document receives the same
               modification. Use the main view as the canonical notification
               so split-editor mode does not run document bookkeeping twice. */
            if (notification->hwndFrom == g_hEdit)
                handle_document_modified(hwnd, scn);
            else if (document_index_for_view(g_hSplitEdit) != g_documentStore.active)
                handle_secondary_document_modified(g_hSplitEdit, scn);
            break;
    }
}

static void destroy_main_window(HWND hwnd) {
    fire_plugin_event("shutdown", g_currentFile);
    save_all_document_breakpoints();
    save_layout_settings();
    KillTimer(hwnd, IDT_PREVIEW_UPDATE);
    KillTimer(hwnd, IDT_FILE_WATCH);
    KillTimer(hwnd, IDT_SCROLL_SYNC);
    KillTimer(hwnd, IDT_SPELL_CHECK);
    KillTimer(hwnd, IDT_AUTOSAVE);
    KillTimer(hwnd, IDT_OUTLINE_UPDATE);
    KillTimer(hwnd, IDT_STATUS_UPDATE);
    KillTimer(hwnd, IDT_PRINT_PREVIEW);
    KillTimer(hwnd, IDT_HTML_EXPORT);
    KillTimer(hwnd, IDT_EXPLORER_REFRESH);
    KillTimer(hwnd, IDT_PLUGIN_STATUS);
    KillTimer(hwnd, IDT_PLUGIN_SELECTION);
    KillTimer(hwnd, IDT_MINIMAP_UPDATE);
    KillTimer(hwnd, IDT_WORD_COMPLETION);
    explorer_shutdown();
    invalidate_markdown_workspace_cache();
    shutdown_output_panel();
    plugin_config_cache_clear();
    plugin_host_shutdown();
    if (g_splitScratchDocument) {
        SendMessageA(g_hSplitEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)g_splitScratchDocument);
        g_splitScratchDocument = 0;
    }
    stop_debug_location_tracking();
    if (g_webview && g_webMessageRegistered) {
        ICoreWebView2_remove_WebMessageReceived(g_webview, g_webMessageToken);
        g_webMessageRegistered = 0;
    }
    if (g_webviewController) ICoreWebView2Controller_Close(g_webviewController);
    if (g_spellChecker) { ISpellChecker_Release(g_spellChecker); g_spellChecker = NULL; }
    if (g_webview3) { ICoreWebView2_3_Release(g_webview3); g_webview3 = NULL; }
    if (g_webview) { ICoreWebView2_Release(g_webview); g_webview = NULL; }
    if (g_webviewController) { ICoreWebView2Controller_Release(g_webviewController); g_webviewController = NULL; }
    free(g_pendingHtmlBody);
    g_pendingHtmlBody = NULL;
    free(g_documentMappingDirectory);
    g_documentMappingDirectory = NULL;
    free(g_exportPath);
    g_exportPath = NULL;
    free(g_pendingPdfPath);
    g_pendingPdfPath = NULL;
    free(g_customCssPath);
    g_customCssPath = NULL;
    if (g_previewBkBrush) { DeleteObject(g_previewBkBrush); g_previewBkBrush = NULL; }
    if (g_mainBkBrush) { DeleteObject(g_mainBkBrush); g_mainBkBrush = NULL; }
    if (g_menuBkBrush) { DeleteObject(g_menuBkBrush); g_menuBkBrush = NULL; }
    if (g_menuFont) { DeleteObject(g_menuFont); g_menuFont = NULL; }
    for (int i = 0; i < g_recentFileCount; i++) free(g_recentFiles[i]);
    for (int i = 0; i < g_documentStore.count; i++) free(g_documentStore.documents[i].path);
    for (int i = 0; i < g_closedDocumentCount; i++) free(g_closedDocuments[i].path);
    for (int i = 0; i < g_wikiBackCount; i++) free(g_wikiBackStack[i]);
    PostQuitMessage(0);
}

static int handle_control_color(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT *result) {
    HWND control = (HWND)lParam;
    HDC hdc = (HDC)wParam;
    if (msg == WM_CTLCOLORSTATIC && control == g_hPreview && g_previewBkBrush) {
        SetBkColor(hdc, preview_bg_color());
        SetTextColor(hdc, preview_palette()->previewTextFg);
        *result = (LRESULT)g_previewBkBrush;
        return 1;
    }
    if (msg == WM_CTLCOLORLISTBOX && control == g_hOutline && g_mainBkBrush) {
        SetBkColor(hdc, theme_bg_color());
        SetTextColor(hdc, theme_fg_color());
        *result = (LRESULT)g_mainBkBrush;
        return 1;
    }
    /* The owner-drawn toolbar buttons erase themselves with this brush before
       they are drawn; the default light one flashes in dark mode. */
    if (msg == WM_CTLCOLORBTN && g_mainBkBrush) {
        *result = (LRESULT)g_mainBkBrush;
        return 1;
    }
    return 0;
}

static int handle_owner_measure(LPARAM lParam) {
    MEASUREITEMSTRUCT *item = (MEASUREITEMSTRUCT *)lParam;
    if (item->CtlType == ODT_MENU && item->itemData)
        measure_main_menu_item(item);
    else if (item->CtlType == ODT_LISTBOX && item->CtlID == ID_OUTLINE)
        measure_themed_listbox_item(g_hOutline, item);
    else
        return 0;
    return 1;
}

static int handle_owner_draw(LPARAM lParam) {
    const DRAWITEMSTRUCT *item = (const DRAWITEMSTRUCT *)lParam;
    if (item->CtlType == ODT_MENU && item->itemData)
        draw_main_menu_item(item);
    else if (item->CtlType == ODT_BUTTON)
        draw_toolbar_button(item);
    else if (item->CtlType == ODT_TAB && item->hwndItem == g_hTabs)
        draw_tab_item(item);
    else if (item->CtlType == ODT_LISTBOX && item->hwndItem == g_hOutline)
        draw_themed_listbox_item(g_hOutline, item);
    else
        return 0;
    return 1;
}

static LRESULT paint_main_background(HWND hwnd, HDC hdc) {
    if (!g_mainBkBrush) g_mainBkBrush = CreateSolidBrush(theme_bg_color());
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    FillRect(hdc, &clientRect, g_mainBkBrush);
    paint_splitter_gap(hdc, &g_splitterRect);
    paint_splitter_gap(hdc, &g_explorerSplitterRect);
    if (g_hSplitEdit && g_splitEditorVisible)
        paint_splitter_gap(hdc, &g_splitEditorSplitterRect);
    paint_splitter_gap(hdc, &g_outputSplitterRect);
    return 1;
}

static int handle_main_timer(HWND hwnd, UINT_PTR timerId) {
    if (timerId >= IDT_PLUGIN_DEFER_BASE) {
        if (plugin_host_is_busy()) {
            SetTimer(hwnd, timerId, PLUGIN_DEFER_RETRY_MS, NULL);
            return 1;
        }
        KillTimer(hwnd, timerId);
        int wasDirty = g_dirty;
        g_pluginChangedSaveEncoding = 0;
        char *error = plugin_host_run_deferred((int)(timerId - IDT_PLUGIN_DEFER_BASE), g_hEdit);
        refresh_plugin_performance_problems();
        sync_dirty_after_plugin(g_hEdit, wasDirty);
        if (error) show_plugin_error(error);
        free(error);
        return 1;
    }
    switch (timerId) {
        case IDT_HTML_EXPORT: ExportDone_Invoke(NULL, E_FAIL, NULL); break;
        case IDT_PREVIEW_UPDATE: update_preview(); break;
        case IDT_FILE_WATCH: check_external_file_change(); break;
        case IDT_SCROLL_SYNC: sync_preview_to_editor_scroll(); break;
        case IDT_SPELL_CHECK: perform_spell_check(); break;
        case IDT_AUTOSAVE: write_recovery_snapshot(); break;
        case IDT_OUTLINE_UPDATE:
            KillTimer(hwnd, IDT_OUTLINE_UPDATE);
            update_outline();
            break;
        case IDT_STATUS_UPDATE:
            KillTimer(hwnd, IDT_STATUS_UPDATE);
            update_status_bar();
            fire_plugin_event("text_changed", g_currentFile);
            break;
        case IDT_PRINT_PREVIEW: run_pending_print(); break;
        case IDT_DEBUG_LOCATION: poll_debug_location(); break;
        case IDT_EXPLORER_REFRESH:
            KillTimer(hwnd, IDT_EXPLORER_REFRESH);
            invalidate_markdown_workspace_cache();
            explorer_handle_change_message();
            break;
        case IDT_PLUGIN_STATUS:
            KillTimer(hwnd, IDT_PLUGIN_STATUS);
            g_statusMessageActive = 0;
            update_status_position();
            break;
        case IDT_PLUGIN_SELECTION:
            KillTimer(hwnd, IDT_PLUGIN_SELECTION);
            update_brace_highlight(tracked_edit_view());
            update_status_position();
            fire_plugin_event("selection_changed", g_currentFile);
            break;
        case IDT_MINIMAP_UPDATE:
            KillTimer(hwnd, IDT_MINIMAP_UPDATE);
            refresh_minimap_after_edit();
            break;
        case IDT_WORD_COMPLETION: {
            KillTimer(hwnd, IDT_WORD_COMPLETION);
            HWND editor = g_pendingCompletionView;
            sptr_t document = g_pendingCompletionDocument;
            g_pendingCompletionView = NULL;
            g_pendingCompletionDocument = 0;
            if (editor && IsWindow(editor) &&
                (sptr_t)SendMessageA(editor, SCI_GETDOCPOINTER, 0, 0) == document)
                update_word_completions(editor);
            break;
        }
        default: return 0;
    }
    return 1;
}

static int set_splitter_cursor(HWND hwnd) {
    POINT point;
    GetCursorPos(&point);
    ScreenToClient(hwnd, &point);
    /* The output panel's splitter runs across the bottom of the others, so
       it is tested first. */
    if (output_splitter_contains(point.x, point.y))
        SetCursor(LoadCursorA(NULL, (LPCSTR)IDC_SIZENS));
    else if (splitter_contains_x(point.x) || explorer_splitter_contains_x(point.x))
        SetCursor(LoadCursorA(NULL, (LPCSTR)IDC_SIZEWE));
    else if (split_editor_splitter_contains(point.x, point.y))
        SetCursor(LoadCursorA(NULL, (LPCSTR)(g_splitEditorVertical ? IDC_SIZEWE : IDC_SIZENS)));
    else
        return 0;
    return 1;
}

static void begin_splitter_drag(HWND hwnd, int x, int y) {
    if (output_splitter_contains(x, y))
        g_draggingOutput = 1;
    else if (splitter_contains_x(x))
        g_dragging = 1;
    else if (explorer_splitter_contains_x(x))
        g_draggingExplorer = 1;
    else if (split_editor_splitter_contains(x, y))
        g_draggingSplitEditor = 1;
    else
        return;
    SetCapture(hwnd);
}

static void update_splitter_drag(HWND hwnd, int x, int y) {
    int positionChanged = 0;
    int contentX = x - explorer_content_x(g_focusMode);
    if (g_dragging && contentX != g_splitterPos) {
        g_splitterPos = contentX;
        positionChanged = 1;
    } else if (g_draggingExplorer && x != g_explorerWidth) {
        g_explorerWidth = x;
        positionChanged = 1;
    } else if (g_draggingOutput) {
        /* The panel's bottom edge stays put while its top follows the mouse. */
        int height = g_outputSplitterRect.bottom + g_outputPanelHeight - y - SPLITTER_WIDTH / 2;
        if (height != g_outputPanelHeight) {
            g_outputPanelHeight = height;
            positionChanged = 1;
        }
    } else if (g_draggingSplitEditor) {
        int position = g_splitEditorVertical ? x - g_splitEditorRegionOrigin.x
                                             : y - g_splitEditorRegionOrigin.y;
        if (position != g_splitEditorPos) {
            g_splitEditorPos = position;
            positionChanged = 1;
        }
    }
    if (positionChanged) {
        layout(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
    }
}

static void end_splitter_drag(void) {
    if (!g_dragging && !g_draggingExplorer && !g_draggingSplitEditor && !g_draggingOutput) return;
    g_dragging = g_draggingExplorer = g_draggingSplitEditor = g_draggingOutput = 0;
    ReleaseCapture();
    save_layout_settings();
}

static void handle_main_notification(HWND hwnd, NMHDR *notification) {
    if (notification->hwndFrom == g_hTabs && notification->code == TCN_SELCHANGE) {
        switch_document(TabCtrl_GetCurSel(g_hTabs));
        return;
    }

    int isTooltip = notification->code == TTN_GETDISPINFOW ||
                    notification->code == TTN_GETDISPINFOA;
    if (isTooltip && g_hTabs && notification->hwndFrom == TabCtrl_GetToolTips(g_hTabs)) {
        int index = (int)notification->idFrom;
        const WCHAR *text = (index >= 0 && index < g_documentStore.count &&
            g_documentStore.documents[index].path) ? g_documentStore.documents[index].path : L"Unsaved document";
        if (notification->code == TTN_GETDISPINFOW) {
            ((NMTTDISPINFOW *)notification)->lpszText = (WCHAR *)text;
        } else {
            WideCharToMultiByte(CP_ACP, 0, text, -1, g_tabTooltipAnsi,
                sizeof(g_tabTooltipAnsi), NULL, NULL);
            ((NMTTDISPINFOA *)notification)->lpszText = g_tabTooltipAnsi;
        }
        return;
    }

    if (g_hExplorerTree && notification->hwndFrom == g_hExplorerTree)
        explorer_handle_notify(notification);
    else
        handle_editor_notification(hwnd, notification);
}

static void handle_main_window_command(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    if ((HWND)lParam == g_hOutline && HIWORD(wParam) == LBN_SELCHANGE) {
        int selected = (int)SendMessageW(g_hOutline, LB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected < g_outlineCount) {
            SendMessageA(g_hEdit, SCI_GOTOLINE, (WPARAM)g_outlineLines[selected], 0);
            SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
            SetFocus(g_hEdit);
        }
        return;
    }
    handle_main_command(hwnd, LOWORD(wParam));
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_APP_APPLY_LARGE_FILE_MODE:
            apply_large_file_mode_after_growth((int)wParam);
            return 0;
        case WM_APP_PLUGIN_DOCUMENT_ACTION:
            run_plugin_document_action((PluginDocumentAction *)lParam);
            return 0;
        case WM_APP_OUTPUT:
            handle_output_chunk((OutputChunk *)lParam);
            return 0;
        case WM_APP_GIT_STATUS:
            handle_git_status_chunk((OutputChunk *)lParam);
            return 0;
        case WM_APP_GIT_SYNC:
            handle_git_sync_chunk((OutputChunk *)lParam);
            return 0;
        case WM_APP_GIT_PANEL_ACTION:
            run_pending_git_panel_action();
            return 0;
        case WM_APP_GIT_BRANCHES:
            handle_git_branches_chunk((OutputChunk *)lParam);
            return 0;
        case WM_CREATE:
            initialize_main_window(hwnd);
            return 0;
        /* Regaining focus repaints the non-client area (active vs. inactive
           caption colour) straight from DefWindowProc's own WM_NCACTIVATE
           handling, without a separate WM_NCPAINT reaching this WndProc -
           so the seam has to be re-painted after both, or it comes back
           every time the window is refocused. */
        case WM_NCPAINT:
        case WM_NCACTIVATE: {
            LRESULT result = DefWindowProcW(hwnd, msg, wParam, lParam);
            erase_menu_bottom_seam(hwnd);
            return result;
        }
        /* Regaining focus from another application is the common moment a
           user has just run git commands (or another git client) outside
           the editor - refresh the branch/status cache so the panel doesn't
           keep showing refs that no longer exist. check_external_file_change()
           already covers edited files via its own poll timer; git has no
           such watcher, so it's refreshed here instead. */
        case WM_ACTIVATEAPP:
            if (wParam) refresh_git_status();
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            LRESULT result;
            if (handle_control_color(msg, wParam, lParam, &result)) return result;
            break;
        }
        case WM_MEASUREITEM:
            if (handle_owner_measure(lParam)) return TRUE;
            break;
        case WM_DRAWITEM:
            if (handle_owner_draw(lParam)) return TRUE;
            break;
        /* g_hEdit/g_hPreview/the toolbar buttons cover the rest of the client
           area themselves, so this only ever shows through on the toolbar
           strip's margins and the splitter gap. */
        case WM_ERASEBKGND:
            return paint_main_background(hwnd, (HDC)wParam);
        case WM_TIMER:
            if (handle_main_timer(hwnd, (UINT_PTR)wParam)) return 0;
            break;
        case WM_SIZE:
            layout(hwnd);
            /* layout() recomputes g_splitterRect, but resizing doesn't
               automatically re-run WM_ERASEBKGND (which is what actually
               paints the splitter gap) for its new position/size. */
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        case WM_SETCURSOR:
            if (set_splitter_cursor(hwnd)) return TRUE;
            break;
        case WM_LBUTTONDOWN:
            begin_splitter_drag(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEMOVE:
            update_splitter_drag(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEWHEEL:
            if (route_terminal_mouse_wheel(wParam, lParam)) return 0;
            break;
        case WM_LBUTTONUP:
            end_splitter_drag();
            return 0;
        case WM_DROPFILES:
            handle_dropfiles((HDROP)wParam);
            return 0;
        case WM_SYSCOLORCHANGE:
            detect_high_contrast();
            return 0;
        case WM_SETTINGCHANGE:
            if (lParam && wcscmp((const WCHAR *)lParam, L"ImmersiveColorSet") == 0) detect_high_contrast();
            break;
        case WM_NOTIFY:
            handle_main_notification(hwnd, (NMHDR *)lParam);
            return 0;
        case WM_COMMAND:
            handle_main_window_command(hwnd, wParam, lParam);
            return 0;
        case WM_APP_CLOSE_TAB:
            do_close_document_at((int)wParam);
            return 0;
        case WM_APP_DEFERRED_INIT:
            run_deferred_startup_init();
            return 0;
        case WM_APP_PLUGIN_TAB_CHECK:
            check_plugin_active_document();
            return 0;
        case TINKEREDIT_WM_EXPLORER_CHANGED:
            /* Debounced rather than refreshing immediately: a burst of
               changes (e.g. a git operation) collapses into one refresh
               shortly after the burst ends, same idea as the outline/status
               update timers above. */
            SetTimer(hwnd, IDT_EXPLORER_REFRESH, EXPLORER_REFRESH_DELAY_MS, NULL);
            return 0;
        case WM_CLOSE:
            do_exit();
            return 0;
        case WM_QUERYENDSESSION:
            save_all_document_breakpoints();
            save_layout_settings();
            save_session_documents();
            write_recovery_snapshot();
            return TRUE;
        case WM_DESTROY:
            destroy_main_window(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static WCHAR *command_line_file(void) {
    int argumentCount = 0;
    WCHAR **arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    WCHAR *file = (arguments && argumentCount > 1) ? wcsdup_heap(arguments[1]) : NULL;
    if (arguments) LocalFree(arguments);
    return file;
}

static void load_application_settings(const WCHAR *commandLineFile) {
    g_commandLinePreviewMode = commandLineFile != NULL;
    INITCOMMONCONTROLSEX commonControls = { sizeof(commonControls),
        ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&commonControls);
    load_font_settings();
    load_theme_settings();
    init_native_menu_theme_support();
    apply_menu_theme(NULL, g_editorDarkMode);
    load_layout_settings();
    load_find_settings();
    load_compiler_settings();
    if (commandLineFile &&
        (language_for_path(commandLineFile)->id == LANG_MARKDOWN || image_file_path(commandLineFile))) {
        /* File-association/command-line launches of a Markdown file are
           preview-only for this session, without overwriting the user's
           normal panel visibility. */
        g_previewVisible = 1;
        g_editorVisible = 0;
    } else if (g_commandLinePreviewMode) {
        /* Command-line launch of a non-Markdown, non-image file: show it in the editor
           for this session, same reasoning as above but the other pane. */
        g_previewVisible = 0;
        g_editorVisible = 1;
    }
    recent_list_load(&g_recentFileList);
    prune_missing_recent_files();
    recent_list_load(&g_recentFolderList);
    load_workspace_settings();
}

static void register_main_window_class(HINSTANCE instance, HICON largeIcon, HICON smallIcon) {
    WNDCLASSEXW windowClass = {0};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"TinkerEditorWndClass";
    windowClass.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); /* IDC_ARROW */
    /* WM_ERASEBKGND paints the themed background; a class brush would flash. */
    windowClass.hbrBackground = NULL;
    windowClass.hIcon = largeIcon;
    windowClass.hIconSm = smallIcon;
    RegisterClassExW(&windowClass);
}

static HWND create_main_window(HINSTANCE instance, HICON largeIcon, HICON smallIcon) {
    int x = g_haveSavedWindowRect ? g_savedWindowRect.left : CW_USEDEFAULT;
    int y = g_haveSavedWindowRect ? g_savedWindowRect.top : CW_USEDEFAULT;
    int width = g_haveSavedWindowRect
        ? g_savedWindowRect.right - g_savedWindowRect.left : DEFAULT_WINDOW_WIDTH;
    int height = g_haveSavedWindowRect
        ? g_savedWindowRect.bottom - g_savedWindowRect.top : DEFAULT_WINDOW_HEIGHT;
    HWND window = CreateWindowExW(WS_EX_ACCEPTFILES, L"TinkerEditorWndClass",
        L"Untitled.txt - Tinker Editor", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, width, height, NULL, NULL, instance, NULL);
    if (largeIcon) SendMessageW(window, WM_SETICON, ICON_BIG, (LPARAM)largeIcon);
    if (smallIcon) SendMessageW(window, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
    return window;
}

/* The key that types ` unshifted: VK_OEM_3 on a US keyboard but VK_OEM_8 on
   a UK one, so the output panel's shortcuts are bound to whichever this
   layout uses (VK_OEM_3 when ` needs Shift or AltGr). */
static WORD backtick_key(void) {
    SHORT key = VkKeyScanW(L'`');
    return key != -1 && HIBYTE(key) == 0 ? LOBYTE(key) : VK_OEM_3;
}

static HACCEL create_main_accelerators(void) {
    ACCEL accelerators[APP_FIXED_ACCELERATOR_MAX + 2 + PLUGIN_MAX_COMMANDS];
    size_t fixedCount = 0;
    const ACCEL *fixed = app_fixed_accelerators(&fixedCount);
    if (fixedCount > APP_FIXED_ACCELERATOR_MAX) return NULL;
    int count = (int)fixedCount;
    memcpy(accelerators, fixed, fixedCount * sizeof(ACCEL));
    WORD backtick = backtick_key();
    accelerators[count++] = (ACCEL){ FVIRTKEY | FCONTROL, backtick, IDM_VIEW_OUTPUT_PANEL };
    accelerators[count++] = (ACCEL){ FVIRTKEY | FCONTROL | FSHIFT, backtick, IDM_VIEW_TERMINAL };
    for (int i = 0; i < plugin_host_command_count() && count < (int)(sizeof(accelerators) / sizeof(accelerators[0])); i++) {
        BYTE flags;
        WORD key;
        if (!plugin_host_command_accelerator(i, &flags, &key)) continue;
        ACCEL candidate = { flags, key, (WORD)(IDM_PLUGIN_COMMAND_0 + i) };
        int conflict = 0;
        for (int existing = 0; existing < count; existing++)
            if (accelerators[existing].fVirt == candidate.fVirt &&
                accelerators[existing].key == candidate.key) { conflict = 1; break; }
        if (!conflict) accelerators[count++] = candidate;
    }
    return CreateAcceleratorTableA(accelerators, count);
}

static void rebuild_main_accelerators(void) {
    HACCEL replacement = create_main_accelerators();
    if (!replacement) return;
    HACCEL old = g_hAccel;
    g_hAccel = replacement;
    if (old) DestroyAcceleratorTable(old);
}

static int run_message_loop(HWND mainWindow) {
    MSG message;
    int result;
    for (;;) {
        /* GetMessage normally returns posted messages before hardware input.
           A command producing output continuously can therefore keep the
           terminal's posted chunks ahead of keys that are already waiting.
           Pull a pending terminal key explicitly so input is delayed by at
           most the one output chunk currently being dispatched. */
        HWND terminal = terminal_view_window(g_terminalView);
        if (terminal && PeekMessageW(&message, terminal, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE))
            result = 1;
        else
            result = GetMessage(&message, NULL, 0, 0);
        if (result <= 0) break;
        /* A focused terminal owns Ctrl/Alt, navigation, and function keys;
           route them before editor accelerators and dialog/menu handling. */
        if (terminal_view_pretranslate_message(g_terminalView, &message)) continue;
        if (find_dialog_shortcut(&message)) continue;
        if (g_hFindDlg && IsDialogMessage(g_hFindDlg, &message)) continue;
        if (explorer_pretranslate_message(&message)) continue;
        if (g_hAccel && TranslateAcceleratorA(mainWindow, g_hAccel, &message)) continue;
        if (IsDialogMessage(mainWindow, &message)) continue;
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
    return result < 0 ? 1 : 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    WCHAR *commandLineFile = command_line_file();
    HMODULE scintillaModule = LoadLibraryW(L"Scintilla.dll");
    if (!scintillaModule) {
        MessageBoxW(NULL, L"Could not load Scintilla.dll (expected next to tinkeredit.exe).",
                    L"Tinker Editor", MB_OK | MB_ICONERROR);
        free(commandLineFile);
        return 1;
    }

    load_application_settings(commandLineFile);
    HRESULT comResult = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    g_comInitialized = SUCCEEDED(comResult);

    HICON largeIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    HICON smallIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    register_main_window_class(hInstance, largeIcon, smallIcon);
    load_menus_and_plugins();
    HWND mainWindow = create_main_window(hInstance, largeIcon, smallIcon);
    g_pendingCliFile = commandLineFile;
    HACCEL accelerators = create_main_accelerators();
    g_hAccel = accelerators;

    /* The progress dialog needs the main window handle and COM state, both of
       which exist by now. */
    g_progressDialogHost.owner = g_hMainWnd;
    g_progressDialogHost.comInitialized = &g_comInitialized;
    progress_dialog_init(&g_progressDialogHost);

    ShowWindow(mainWindow, g_startMaximized ? SW_SHOWMAXIMIZED : nCmdShow);
    UpdateWindow(mainWindow);
    PostMessageW(mainWindow, WM_APP_DEFERRED_INIT, 0, 0);

    int exitCode = run_message_loop(mainWindow);
    if (g_hAccel) DestroyAcceleratorTable(g_hAccel);
    if (g_comInitialized) CoUninitialize();
    FreeLibrary(scintillaModule);
    return exitCode;
}

