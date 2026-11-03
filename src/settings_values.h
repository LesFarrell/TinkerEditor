#ifndef SETTINGS_VALUES_H
#define SETTINGS_VALUES_H

#include <windows.h>

/* Widths and metrics shared with the persistence code. Keep this header free
   of function prototypes so a module can depend on the persisted values alone
   without pulling in the load/save surface (see settings_io.h). */

/* Wider than LOGFONT's own LF_FACESIZE (32 WCHARs): that cap is a limit of
   the classic ChooseFont/LOGFONT API's round-trip, not of anything this app
   needs to store or hand to Scintilla (SCI_STYLESETFONT takes a plain
   NUL-terminated string). do_choose_font() works around ChooseFontW's
   truncation with a dialog hook so real font names longer than 31
   characters survive intact. */
#define MAX_FONT_FACE_NAME 128

/* Pixel width of the editor/preview splitter; save_layout_settings() converts
   the live splitter position to/from a ratio using this. Also used by the
   splitter's own layout/hit-testing code in tinkeredit.c. */
#define SPLITTER_WIDTH 4

/* User-configurable Run/Debug C/C++ compiler backend; see load_compiler_settings()
   and find_native_compiler(). Auto keeps the existing GCC/Clang/Zig/MSVC
   priority search; the others pin Run/Debug to just one of them. */
typedef enum {
    COMPILER_BACKEND_AUTO = 0, COMPILER_BACKEND_GCC, COMPILER_BACKEND_CLANG,
    COMPILER_BACKEND_ZIG, COMPILER_BACKEND_MSVC
} CompilerBackendPreference;

/* Shared application state persisted to the normal or portable settings.json.
   Defined (without static) in tinkeredit.c; read and written here. Transient
   Find/Replace state lives in find_state.h instead. */
extern HWND g_hMainWnd;
extern WCHAR g_fontFaceName[MAX_FONT_FACE_NAME];
extern int g_fontPointSize;
extern int g_fontWeight;
extern int g_fontItalic;
extern int g_previewDarkMode;
extern int g_editorDarkMode;
extern WCHAR *g_customCssPath;
extern int g_previewOnLeft;
extern int g_previewVisible;
extern int g_editorVisible;
extern int g_focusMode;
extern int g_wordWrap;
extern int g_spellCheckEnabled;
extern int g_showWhitespace;
extern int g_indentGuides;
extern int g_markChangedLines;
extern int g_indentUseSpaces;
extern int g_tabWidth;
extern int g_outlineVisible;
extern int g_minimapVisible;
extern int g_editorZoom;
extern int g_largeFileThresholdMB;
extern WCHAR g_spellLanguage[LOCALE_NAME_MAX_LENGTH];
extern int g_splitterRatio;
extern int g_splitterPos;
extern int g_splitEditorVisible;
extern int g_splitEditorRatio;
extern int g_splitEditorVertical;
extern int g_outputPanelVisible;
extern int g_outputPanelHeight;
extern int g_outputPanelOnRight;
extern int g_outputPanelWidth;
extern int g_runInConsole;
extern int g_commandLinePreviewMode;
extern RECT g_savedWindowRect;
extern int g_haveSavedWindowRect;
extern int g_startMaximized;
extern CompilerBackendPreference g_compilerBackend;
extern WCHAR *g_vcvarsallOverridePath;
extern WCHAR *g_gccBinOverride;
extern WCHAR *g_clangBinOverride;
extern WCHAR *g_zigBinOverride;
extern WCHAR *g_luaBinOverride;
extern WCHAR *g_gdbBinOverride;
extern WCHAR *g_compilerExtraFlags;

#endif
