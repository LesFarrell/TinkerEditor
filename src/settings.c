#include "settings_values.h"
#include "settings_io.h"
#include "find_state.h"

#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>

#include "layout_metrics.h" /* explorer geometry values + explorer_content_x() */
#include "path_utils.h"
#include "plugin_settings.h"
#include "text_codec.h"
#include "text_search.h" /* SEARCH_* find options */
#include "tinkeredit.h" /* OUTPUT_TAB_RUN/OUTPUT_TAB_COUNT, for g_outputTab */

/* Persisted settings: normally %APPDATA%\tinkeredit\settings.json, or
   portable-data\settings.json when portable.mode is beside the executable. */
WCHAR *get_settings_path(void) {
    WCHAR *directory = get_user_config_directory();
    if (!directory) return NULL;
    WCHAR *settings = path_join(directory, L"settings.json");
    free(directory);
    return settings;
}

/* The JSON store uses the same section/key model the application already had,
   which keeps settings call sites small while changing the persistence format. */
WCHAR *read_profile_string(const WCHAR *section, const WCHAR *key, const WCHAR *path) {
    char *sectionUtf8 = to_utf8(section), *keyUtf8 = to_utf8(key);
    char *valueUtf8 = sectionUtf8 && keyUtf8
        ? plugin_settings_get(path, sectionUtf8, keyUtf8) : NULL;
    WCHAR *value = valueUtf8 && valueUtf8[0] ? to_wide(valueUtf8) : NULL;
    free(valueUtf8); free(keyUtf8); free(sectionUtf8);
    return value;
}

int read_setting_int(const WCHAR *section, const WCHAR *key, int defaultValue,
                     const WCHAR *path) {
    WCHAR *value = read_profile_string(section, key, path), *end = NULL;
    long parsed = value ? wcstol(value, &end, 10) : 0;
    int result = value && end != value && *end == 0 ? (int)parsed : defaultValue;
    free(value);
    return result;
}

int write_setting_string(const WCHAR *section, const WCHAR *key, const WCHAR *value,
                         const WCHAR *path) {
    char *sectionUtf8 = to_utf8(section), *keyUtf8 = to_utf8(key);
    char *valueUtf8 = value ? to_utf8(value) : NULL;
    int ok = sectionUtf8 && keyUtf8 && (!value || valueUtf8) &&
        plugin_settings_set(path, sectionUtf8, keyUtf8, valueUtf8);
    free(valueUtf8); free(keyUtf8); free(sectionUtf8);
    return ok;
}

int clear_setting_section(const WCHAR *section, const WCHAR *path) {
    /* Removing known keys is handled by callers that own bounded sections.
       Dynamic sections use this sentinel operation in the store. */
    char *sectionUtf8 = to_utf8(section);
    int ok = sectionUtf8 && plugin_settings_clear_section(path, sectionUtf8);
    free(sectionUtf8);
    return ok;
}

static void write_profile_int(const WCHAR *section, const WCHAR *key, int value,
                              const WCHAR *path) {
    WCHAR buffer[32];
    swprintf(buffer, 32, L"%d", value);
    write_setting_string(section, key, buffer, path);
}

#define read_profile_int read_setting_int

static void read_setting_into(const WCHAR *section, const WCHAR *key, const WCHAR *fallback,
                              WCHAR *output, size_t capacity, const WCHAR *path) {
    WCHAR *value = read_profile_string(section, key, path);
    const WCHAR *source = value ? value : fallback;
    wcsncpy(output, source, capacity - 1);
    output[capacity - 1] = 0;
    free(value);
}

/* A section's Theme key: 1 for github-dark, 0 for github-light, -1 when it
   isn't set (or holds anything else). */
static int read_theme_choice(const WCHAR *section, const WCHAR *path) {
    WCHAR value[32];
    read_setting_into(section, L"Theme", L"", value, 32, path);
    if (_wcsicmp(value, L"github-dark") == 0) return 1;
    if (_wcsicmp(value, L"github-light") == 0) return 0;
    return -1;
}

void load_font_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    read_setting_into(L"Font", L"FaceName", L"Consolas",
                      g_fontFaceName, MAX_FONT_FACE_NAME, path);
    g_fontPointSize = read_profile_int(L"Font", L"PointSize", g_fontPointSize, path);
    if (g_fontPointSize < 1) g_fontPointSize = 1;
    g_fontWeight = read_profile_int(L"Font", L"Weight", FW_NORMAL, path);
    if (g_fontWeight < 1) g_fontWeight = FW_NORMAL;
    g_fontItalic = read_profile_int(L"Font", L"Italic", 0, path) ? 1 : 0;
    free(path);
}

void load_theme_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    int previewTheme = read_theme_choice(L"Preview", path);
    if (previewTheme >= 0) {
        g_previewDarkMode = previewTheme;
    } else {
        WCHAR value[32];
        /* Migrate settings written by releases that exposed a DarkMode toggle. */
        read_setting_into(L"Preview", L"DarkMode", L"0", value, 32, path);
        g_previewDarkMode = (value[0] == L'1' || value[0] == L't' || value[0] == L'T' ||
                             value[0] == L'y' || value[0] == L'Y');
    }
    int editorTheme = read_theme_choice(L"Editor", path);
    if (editorTheme >= 0) {
        g_editorDarkMode = editorTheme;
    } else {
        /* No Editor Theme key yet - upgrading from a release where the editor
           and preview shared one theme, so start the editor matching whatever
           the preview was already set to. */
        g_editorDarkMode = g_previewDarkMode;
    }
    g_customCssPath = read_profile_string(L"Preview", L"CustomCSS", path);
    free(path);
}

void save_theme_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    write_setting_string(L"Preview", L"Theme",
                         g_previewDarkMode ? L"github-dark" : L"github-light", path);
    write_setting_string(L"Preview", L"DarkMode", NULL, path);
    write_setting_string(L"Editor", L"Theme",
                         g_editorDarkMode ? L"github-dark" : L"github-light", path);
    write_setting_string(L"Preview", L"CustomCSS",
                         g_customCssPath ? g_customCssPath : L"", path);
    free(path);
}

/* Every persisted pane-split ratio (preview, explorer, split editor)
   is a permille clamped to this same [50,950] range, both when read back at
   startup and when re-derived from live pixel positions on save. */
static int clamp_ratio(int value) {
    if (value < 50) return 50;
    if (value > 950) return 950;
    return value;
}

void load_layout_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    g_previewOnLeft = read_profile_int(L"View", L"PreviewOnLeft", 0, path) != 0;
    g_previewVisible = read_profile_int(L"View", L"PreviewVisible", g_previewVisible, path) != 0;
    g_editorVisible = read_profile_int(L"View", L"EditorVisible", g_editorVisible, path) != 0;
    g_focusMode = read_profile_int(L"View", L"FocusMode", 0, path) != 0;
    g_wordWrap = read_profile_int(L"View", L"WordWrap", g_wordWrap, path) != 0;
    g_spellCheckEnabled = read_profile_int(L"View", L"SpellCheck", g_spellCheckEnabled, path) != 0;
    g_showWhitespace = read_profile_int(L"View", L"ShowWhitespace", g_showWhitespace, path) != 0;
    g_indentGuides = read_profile_int(L"View", L"IndentGuides", g_indentGuides, path) != 0;
    g_markChangedLines = read_profile_int(L"View", L"MarkChangedLines", g_markChangedLines, path) != 0;
    g_indentUseSpaces = read_profile_int(L"View", L"UseSpacesForIndent", g_indentUseSpaces, path) != 0;
    g_tabWidth = read_profile_int(L"View", L"TabWidth", g_tabWidth, path);
    if (g_tabWidth < 1) g_tabWidth = 1;
    if (g_tabWidth > 16) g_tabWidth = 16;
    g_outlineVisible = read_profile_int(L"View", L"OutlineVisible", g_outlineVisible, path) != 0;
    g_outlineOnLeft = read_profile_int(L"View", L"OutlineOnLeft", 0, path) != 0;
    g_minimapVisible = read_profile_int(L"View", L"MinimapVisible", g_minimapVisible, path) != 0;
    g_explorerVisible = read_profile_int(L"View", L"ExplorerVisible", g_explorerVisible, path) != 0;
    g_explorerWidthRatio = clamp_ratio(read_profile_int(L"View", L"ExplorerWidthRatio", g_explorerWidthRatio, path));
    /* Keep the panel at the width the user dragged it to. Older settings
       only have a ratio, which layout() still uses when this key is absent. */
    int explorerWidth = read_profile_int(L"View", L"ExplorerWidth", -1, path);
    g_explorerWidth = explorerWidth > 0 ? explorerWidth : -1;
    g_gitPanelVisible = read_profile_int(L"View", L"GitPanelVisible", 0, path) != 0;
    int gitPanelWidth = read_profile_int(L"View", L"GitPanelWidth", -1, path);
    g_gitPanelWidth = gitPanelWidth > 0 ? gitPanelWidth : -1;
    g_gitPanelOnRight = read_profile_int(L"View", L"GitPanelOnRight", 1, path) != 0;
    g_mergePanelVisible = read_profile_int(L"View", L"MergePanelVisible", 0, path) != 0;
    int mergePanelWidth = read_profile_int(L"View", L"MergePanelWidth", -1, path);
    g_mergePanelWidth = mergePanelWidth > 0 ? mergePanelWidth : -1;
    g_editorZoom = read_profile_int(L"View", L"EditorZoom", g_editorZoom, path);
    if (g_editorZoom < -10) g_editorZoom = -10;
    if (g_editorZoom > 20) g_editorZoom = 20;
    g_largeFileThresholdMB = read_profile_int(L"View", L"LargeFileThresholdMB", g_largeFileThresholdMB, path);
    if (g_largeFileThresholdMB < 1) g_largeFileThresholdMB = 1;
    /* Above 2 GB a file opens in large-file mode regardless (see load_file()). */
    if (g_largeFileThresholdMB > 2047) g_largeFileThresholdMB = 2047;
    read_setting_into(L"SpellCheck", L"Language", L"en-GB", g_spellLanguage,
                      LOCALE_NAME_MAX_LENGTH, path);
    if (!g_previewVisible && !g_editorVisible) g_editorVisible = 1;

    g_splitterRatio = clamp_ratio(read_profile_int(L"View", L"SplitterRatio", 500, path));

    g_splitEditorVisible = read_profile_int(L"View", L"SplitEditorVisible", 0, path) != 0;
    g_splitEditorRatio = clamp_ratio(read_profile_int(L"View", L"SplitEditorRatio", 500, path));
    g_splitEditorVertical = read_profile_int(L"View", L"SplitEditorVertical", 0, path) != 0;
    g_outputPanelVisible = read_profile_int(L"View", L"OutputPanelVisible", 0, path) != 0;
    int outputHeight = read_profile_int(L"View", L"OutputPanelHeight", -1, path);
    g_outputPanelHeight = outputHeight > 0 ? outputHeight : -1;
    g_outputPanelOnRight = read_profile_int(L"View", L"OutputPanelOnRight", 0, path) != 0;
    int outputWidth = read_profile_int(L"View", L"OutputPanelWidth", -1, path);
    g_outputPanelWidth = outputWidth > 0 ? outputWidth : -1;
    g_outputTab = read_profile_int(L"View", L"OutputTab", OUTPUT_TAB_RUN, path);
    if (g_outputTab < 0 || g_outputTab >= OUTPUT_TAB_COUNT) g_outputTab = OUTPUT_TAB_RUN;
    g_runInConsole = read_profile_int(L"View", L"RunInSeparateConsole", 0, path) != 0;

    int left = read_profile_int(L"Window", L"Left", 0, path);
    int top = read_profile_int(L"Window", L"Top", 0, path);
    int width = read_profile_int(L"Window", L"Width", 0, path);
    int height = read_profile_int(L"Window", L"Height", 0, path);
    RECT candidate = { left, top, left + width, top + height };
    if (width >= 400 && height >= 300 &&
        MonitorFromRect(&candidate, MONITOR_DEFAULTTONULL) != NULL) {
        g_savedWindowRect = candidate;
        g_haveSavedWindowRect = 1;
    }
    g_startMaximized = read_profile_int(L"Window", L"Maximized", 0, path) != 0;
    free(path);
}

void save_layout_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    write_profile_int(L"View", L"PreviewOnLeft", g_previewOnLeft, path);
    write_profile_int(L"View", L"FocusMode", g_focusMode, path);
    write_profile_int(L"View", L"WordWrap", g_wordWrap, path);
    write_profile_int(L"View", L"SpellCheck", g_spellCheckEnabled, path);
    write_profile_int(L"View", L"ShowWhitespace", g_showWhitespace, path);
    write_profile_int(L"View", L"IndentGuides", g_indentGuides, path);
    write_profile_int(L"View", L"MarkChangedLines", g_markChangedLines, path);
    write_profile_int(L"View", L"UseSpacesForIndent", g_indentUseSpaces, path);
    write_profile_int(L"View", L"TabWidth", g_tabWidth, path);
    write_profile_int(L"View", L"OutlineVisible", g_outlineVisible, path);
    write_profile_int(L"View", L"OutlineOnLeft", g_outlineOnLeft, path);
    write_profile_int(L"View", L"MinimapVisible", g_minimapVisible, path);
    write_profile_int(L"View", L"ExplorerVisible", g_explorerVisible, path);
    write_profile_int(L"View", L"EditorZoom", g_editorZoom, path);
    write_profile_int(L"View", L"LargeFileThresholdMB", g_largeFileThresholdMB, path);
    write_setting_string(L"SpellCheck", L"Language", g_spellLanguage, path);
    if (!g_commandLinePreviewMode) {
        write_profile_int(L"View", L"PreviewVisible", g_previewVisible, path);
        write_profile_int(L"View", L"EditorVisible", g_editorVisible, path);
    }

    if (g_hMainWnd) {
        RECT client;
        GetClientRect(g_hMainWnd, &client);
        /* g_splitterPos is relative to the content area beside the explorer
           and Git panels, not the full client width - explorer_panel_width()
           and git_panel_width() are the same widths layout() shrinks that
           content area by (on whichever side each panel is docked), so the
           persisted ratio has to be measured against the same narrower
           width or it drifts by however wide the panels currently are. */
        int sidePanelsWidth = explorer_panel_width(g_focusMode) + git_panel_width(g_focusMode);
        if (g_splitterPos >= 0) {
            int available = client.right - sidePanelsWidth - SPLITTER_WIDTH;
            if (available > 0) g_splitterRatio = clamp_ratio(MulDiv(g_splitterPos, 1000, available));
        }
        if (g_explorerWidth >= 0 && client.right > 0)
            g_explorerWidthRatio = clamp_ratio(MulDiv(g_explorerWidth, 1000, client.right));
    }
    write_profile_int(L"View", L"SplitterRatio", g_splitterRatio, path);
    write_profile_int(L"View", L"ExplorerWidthRatio", g_explorerWidthRatio, path);
    if (g_explorerWidth > 0)
        write_profile_int(L"View", L"ExplorerWidth", g_explorerWidth, path);
    write_profile_int(L"View", L"GitPanelVisible", g_gitPanelVisible, path);
    if (g_gitPanelWidth > 0)
        write_profile_int(L"View", L"GitPanelWidth", g_gitPanelWidth, path);
    write_profile_int(L"View", L"GitPanelOnRight", g_gitPanelOnRight, path);
    write_profile_int(L"View", L"MergePanelVisible", g_mergePanelVisible, path);
    if (g_mergePanelWidth > 0)
        write_profile_int(L"View", L"MergePanelWidth", g_mergePanelWidth, path);
    write_profile_int(L"View", L"SplitEditorVisible", g_splitEditorVisible, path);
    write_profile_int(L"View", L"SplitEditorRatio", g_splitEditorRatio, path);
    write_profile_int(L"View", L"SplitEditorVertical", g_splitEditorVertical, path);
    write_profile_int(L"View", L"OutputPanelVisible", g_outputPanelVisible, path);
    if (g_outputPanelHeight > 0)
        write_profile_int(L"View", L"OutputPanelHeight", g_outputPanelHeight, path);
    write_profile_int(L"View", L"OutputPanelOnRight", g_outputPanelOnRight, path);
    if (g_outputPanelWidth > 0)
        write_profile_int(L"View", L"OutputPanelWidth", g_outputPanelWidth, path);
    write_profile_int(L"View", L"OutputTab", g_outputTab, path);
    write_profile_int(L"View", L"RunInSeparateConsole", g_runInConsole, path);

    WINDOWPLACEMENT placement = {0};
    placement.length = sizeof(placement);
    if (g_hMainWnd && GetWindowPlacement(g_hMainWnd, &placement)) {
        RECT normal = placement.rcNormalPosition;
        int maximized = placement.showCmd == SW_SHOWMAXIMIZED ||
            (placement.showCmd == SW_SHOWMINIMIZED &&
             (placement.flags & WPF_RESTORETOMAXIMIZED) != 0);
        write_profile_int(L"Window", L"Left", normal.left, path);
        write_profile_int(L"Window", L"Top", normal.top, path);
        write_profile_int(L"Window", L"Width", normal.right - normal.left, path);
        write_profile_int(L"Window", L"Height", normal.bottom - normal.top, path);
        write_profile_int(L"Window", L"Maximized", maximized, path);
    }
    free(path);
}

void save_font_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    write_setting_string(L"Font", L"FaceName", g_fontFaceName, path);
    write_profile_int(L"Font", L"PointSize", g_fontPointSize, path);
    write_profile_int(L"Font", L"Weight", g_fontWeight, path);
    write_setting_string(L"Font", L"Italic", g_fontItalic ? L"1" : L"0", path);
    free(path);
}

void load_find_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    read_setting_into(L"Find", L"FindWhat", L"", g_findWhat,
                      sizeof(g_findWhat) / sizeof(g_findWhat[0]), path);
    read_setting_into(L"Find", L"ReplaceWith", L"", g_replaceWith,
                      sizeof(g_replaceWith) / sizeof(g_replaceWith[0]), path);
    g_findOptions = 0;
    if (read_profile_int(L"Find", L"MatchCase", 0, path)) g_findOptions |= SEARCH_MATCH_CASE;
    if (read_profile_int(L"Find", L"WholeWord", 0, path)) g_findOptions |= SEARCH_WHOLE_WORD;
    if (read_profile_int(L"Find", L"Regex", 0, path)) g_findOptions |= SEARCH_REGEX;
    g_findInSelection = read_profile_int(L"Find", L"InSelection", 0, path) != 0;
    free(path);
}

void save_find_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    write_setting_string(L"Find", L"FindWhat", g_findWhat, path);
    write_setting_string(L"Find", L"ReplaceWith", g_replaceWith, path);
    write_profile_int(L"Find", L"MatchCase", (g_findOptions & SEARCH_MATCH_CASE) != 0, path);
    write_profile_int(L"Find", L"WholeWord", (g_findOptions & SEARCH_WHOLE_WORD) != 0, path);
    write_profile_int(L"Find", L"Regex", (g_findOptions & SEARCH_REGEX) != 0, path);
    write_profile_int(L"Find", L"InSelection", g_findInSelection != 0, path);
    free(path);
}

/* Backend may be hand-edited in settings.json rather than exposed through a menu:
   it pins Run/Debug C/C++ to one compiler instead of the default Auto
   priority search (GCC, Clang, Zig, then MSVC as a Run-only fallback). Every
   other key here - the per-toolchain path overrides and ExtraFlags - is what
   the Settings dialog's Compiler category exposes, and save_compiler_settings()
   round-trips Backend unchanged whenever it writes any of them back out. */
void load_compiler_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WCHAR value[16];
    read_setting_into(L"Compiler", L"Backend", L"Auto", value, 16, path);
    if (_wcsicmp(value, L"GCC") == 0) g_compilerBackend = COMPILER_BACKEND_GCC;
    else if (_wcsicmp(value, L"Clang") == 0) g_compilerBackend = COMPILER_BACKEND_CLANG;
    else if (_wcsicmp(value, L"Zig") == 0) g_compilerBackend = COMPILER_BACKEND_ZIG;
    else if (_wcsicmp(value, L"MSVC") == 0) g_compilerBackend = COMPILER_BACKEND_MSVC;
    else g_compilerBackend = COMPILER_BACKEND_AUTO;

    g_vcvarsallOverridePath = read_profile_string(L"Compiler", L"VcvarsallPath", path);
    g_gccBinOverride = read_profile_string(L"Compiler", L"GccPath", path);
    g_clangBinOverride = read_profile_string(L"Compiler", L"ClangPath", path);
    g_zigBinOverride = read_profile_string(L"Compiler", L"ZigPath", path);
    g_luaBinOverride = read_profile_string(L"Compiler", L"LuaPath", path);
    g_gdbBinOverride = read_profile_string(L"Compiler", L"GdbPath", path);
    g_compilerExtraFlags = read_profile_string(L"Compiler", L"ExtraFlags", path);
    free(path);
}

void save_compiler_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    const WCHAR *backend =
        g_compilerBackend == COMPILER_BACKEND_GCC ? L"GCC" :
        g_compilerBackend == COMPILER_BACKEND_CLANG ? L"Clang" :
        g_compilerBackend == COMPILER_BACKEND_ZIG ? L"Zig" :
        g_compilerBackend == COMPILER_BACKEND_MSVC ? L"MSVC" : L"Auto";
    write_setting_string(L"Compiler", L"Backend", backend, path);
    write_setting_string(L"Compiler", L"VcvarsallPath",
                         g_vcvarsallOverridePath ? g_vcvarsallOverridePath : L"", path);
    write_setting_string(L"Compiler", L"GccPath",
                         g_gccBinOverride ? g_gccBinOverride : L"", path);
    write_setting_string(L"Compiler", L"ClangPath",
                         g_clangBinOverride ? g_clangBinOverride : L"", path);
    write_setting_string(L"Compiler", L"ZigPath",
                         g_zigBinOverride ? g_zigBinOverride : L"", path);
    write_setting_string(L"Compiler", L"LuaPath",
                         g_luaBinOverride ? g_luaBinOverride : L"", path);
    write_setting_string(L"Compiler", L"GdbPath",
                         g_gdbBinOverride ? g_gdbBinOverride : L"", path);
    write_setting_string(L"Compiler", L"ExtraFlags",
                         g_compilerExtraFlags ? g_compilerExtraFlags : L"", path);
    free(path);
}

/* The Settings dialog's Git category: an optional external diff tool (e.g.
   WinMerge, Beyond Compare, kdiff3) used in place of the built-in text diff
   for single-file comparisons. Launched as "<path>" <left> <right>, the same
   two-file convention those tools already support. */
void load_git_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    g_useExternalDiffTool = read_profile_int(L"Git", L"UseExternalDiffTool", 0, path) != 0;
    g_externalDiffToolPath = read_profile_string(L"Git", L"DiffToolPath", path);
    g_externalMergeToolPath = read_profile_string(L"Git", L"MergeToolPath", path);
    free(path);
}

void save_git_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    write_profile_int(L"Git", L"UseExternalDiffTool", g_useExternalDiffTool, path);
    write_setting_string(L"Git", L"DiffToolPath",
                         g_externalDiffToolPath ? g_externalDiffToolPath : L"", path);
    write_setting_string(L"Git", L"MergeToolPath",
                         g_externalMergeToolPath ? g_externalMergeToolPath : L"", path);
    free(path);
}
