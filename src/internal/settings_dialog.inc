/* ---- Settings dialog ----------------------------------------------------
   A single window over eight categories (left-hand list, one panel of
   controls shown at a time) that just launches or toggles the same actions
   already reachable from the View menu one at a time - do_choose_font(),
   select_theme(), toggle_word_wrap(), etc. Every control applies and
   persists immediately via whichever of those it calls, the same as
   clicking the equivalent menu item would, so there's nothing to commit or
   revert: Close (and Esc) just dismiss the window.

   The controls are described by SETTINGS_CONTROLS below, one row each, and
   named by SettingsControl. A control's window ID is derived from its name
   (ID_SETTINGS_FIRST + name), so the creation code and WM_COMMAND handling
   can't disagree about which number means what.

   Every plugin with settings is a category of its own. Its owned values,
   including each menu.checkbox() command, appear in the panel on the right. */

/* Defined in plugin_bridge.inc, included after this file. */
static void manage_plugins(void);
static void run_plugin_command(int index);

enum {
    ID_SETTINGS_CATEGORIES = 10, ID_SETTINGS_PLUGIN_VALUES = 11, ID_SETTINGS_FIRST = 100
};

typedef enum {
    SETTINGS_APPEARANCE, SETTINGS_FONT, SETTINGS_WORD_WRAP,
    SETTINGS_PREVIEW, SETTINGS_EDITOR, SETTINGS_COMPILER, SETTINGS_GIT, SETTINGS_PLUGINS,
    SETTINGS_PLUGIN_VALUES, SETTINGS_CATEGORY_COUNT
} SettingsCategory;

typedef enum {
    SC_THEME_LIGHT, SC_THEME_DARK,
    SC_FONT_LABEL, SC_FONT_CHOOSE,
    SC_WORD_WRAP,
    SC_PREVIEW_ON_LEFT, SC_PREVIEW_THEME_CAPTION, SC_PREVIEW_THEME_LIGHT, SC_PREVIEW_THEME_DARK,
    SC_CSS_CAPTION, SC_CSS_LABEL, SC_CSS_CHOOSE, SC_CSS_CLEAR,
    SC_SPELL_CHECK, SC_SHOW_WHITESPACE, SC_INDENT_GUIDES, SC_MINIMAP, SC_USE_SPACES,
    SC_TAB_WIDTH_CAPTION, SC_TAB_WIDTH,
    SC_SPELL_LANGUAGE_CAPTION, SC_SPELL_LANGUAGE_LABEL, SC_SPELL_LANGUAGE_CHOOSE,
    SC_COMPILER_HINT,
    SC_VCVARS_CAPTION, SC_VCVARS, SC_VCVARS_BROWSE,
    SC_GCC_CAPTION, SC_GCC, SC_GCC_BROWSE,
    SC_CLANG_CAPTION, SC_CLANG, SC_CLANG_BROWSE,
    SC_ZIG_CAPTION, SC_ZIG, SC_ZIG_BROWSE,
    SC_LUA_CAPTION, SC_LUA, SC_LUA_BROWSE,
    SC_GDB_CAPTION, SC_GDB, SC_GDB_BROWSE,
    SC_FLAGS_CAPTION, SC_FLAGS,
    SC_DIFFTOOL_USE,
    SC_DIFFTOOL_CAPTION, SC_DIFFTOOL, SC_DIFFTOOL_BROWSE,
    SC_MERGETOOL_CAPTION, SC_MERGETOOL, SC_MERGETOOL_BROWSE,
    SC_PLUGINS_HINT, SC_PLUGINS_MANAGE,
    SC_PLUGIN_VALUES_HINT, SC_PLUGIN_VALUE_ADD, SC_PLUGIN_VALUE_EDIT, SC_PLUGIN_VALUE_REMOVE,
    SC_COUNT
} SettingsControl;

typedef enum {
    SETTINGS_TEXT,        /* a STATIC caption or value label */
    SETTINGS_CHECK,       /* checkbox showing *setting != 0 */
    SETTINGS_RADIO_FIRST, /* first radio button of a group */
    SETTINGS_RADIO,       /* checked when *setting equals its value */
    SETTINGS_BUTTON,      /* owner-drawn push button */
    SETTINGS_PATH_EDIT,   /* text box editing *path; committed on losing focus */
    SETTINGS_NUMBER_EDIT, /* digits-only text box */
} SettingsControlKind;

typedef struct SettingsControlSpec {
    SettingsCategory category;
    SettingsControlKind kind;
    const WCHAR *text;
    int x, y, width, height; /* relative to the panel's top-left corner */
    const int *setting;      /* SETTINGS_CHECK / SETTINGS_RADIO* */
    int value;               /* SETTINGS_RADIO*: the setting value that checks it */
    WCHAR **path;            /* SETTINGS_PATH_EDIT */
} SettingsControlSpec;

/* One row per control, indexed by SettingsControl, so a new control needs a
   name above and a row here. Browse buttons directly follow the path box
   they fill in. */
static const SettingsControlSpec SETTINGS_CONTROLS[SC_COUNT] = {
    [SC_THEME_LIGHT] = { SETTINGS_APPEARANCE, SETTINGS_RADIO_FIRST, L"Light", 0, 0, 220, 22, &g_editorDarkMode, 0, NULL },
    [SC_THEME_DARK] = { SETTINGS_APPEARANCE, SETTINGS_RADIO, L"Dark", 0, 28, 220, 22, &g_editorDarkMode, 1, NULL },

    [SC_FONT_LABEL] = { SETTINGS_FONT, SETTINGS_TEXT, NULL, 0, 0, 320, 20, NULL, 0, NULL },
    [SC_FONT_CHOOSE] = { SETTINGS_FONT, SETTINGS_BUTTON, L"Choose Font...", 0, 28, 140, 26, NULL, 0, NULL },

    [SC_WORD_WRAP] = { SETTINGS_WORD_WRAP, SETTINGS_CHECK, L"Wrap long lines in the editor", 0, 0, 320, 22, &g_wordWrap, 1, NULL },

    [SC_PREVIEW_ON_LEFT] = { SETTINGS_PREVIEW, SETTINGS_CHECK, L"Show preview on the left", 0, 0, 320, 22, &g_previewOnLeft, 1, NULL },
    [SC_PREVIEW_THEME_CAPTION] = { SETTINGS_PREVIEW, SETTINGS_TEXT, L"Markdown theme:", 0, 36, 320, 20, NULL, 0, NULL },
    [SC_PREVIEW_THEME_LIGHT] = { SETTINGS_PREVIEW, SETTINGS_RADIO_FIRST, L"GitHub Light", 0, 58, 220, 22, &g_previewDarkMode, 0, NULL },
    [SC_PREVIEW_THEME_DARK] = { SETTINGS_PREVIEW, SETTINGS_RADIO, L"GitHub Dark", 0, 86, 220, 22, &g_previewDarkMode, 1, NULL },
    [SC_CSS_CAPTION] = { SETTINGS_PREVIEW, SETTINGS_TEXT, L"Custom preview CSS:", 0, 122, 320, 20, NULL, 0, NULL },
    [SC_CSS_LABEL] = { SETTINGS_PREVIEW, SETTINGS_TEXT, NULL, 0, 144, 320, 20, NULL, 0, NULL },
    [SC_CSS_CHOOSE] = { SETTINGS_PREVIEW, SETTINGS_BUTTON, L"Choose CSS...", 0, 170, 120, 26, NULL, 0, NULL },
    [SC_CSS_CLEAR] = { SETTINGS_PREVIEW, SETTINGS_BUTTON, L"Clear", 128, 170, 90, 26, NULL, 0, NULL },

    [SC_SPELL_CHECK] = { SETTINGS_EDITOR, SETTINGS_CHECK, L"Spell check", 0, 0, 320, 22, &g_spellCheckEnabled, 1, NULL },
    [SC_SHOW_WHITESPACE] = { SETTINGS_EDITOR, SETTINGS_CHECK, L"Show whitespace", 0, 28, 320, 22, &g_showWhitespace, 1, NULL },
    [SC_INDENT_GUIDES] = { SETTINGS_EDITOR, SETTINGS_CHECK, L"Indent guides", 0, 56, 320, 22, &g_indentGuides, 1, NULL },
    [SC_MINIMAP] = { SETTINGS_EDITOR, SETTINGS_CHECK, L"Minimap", 0, 84, 320, 22, &g_minimapVisible, 1, NULL },
    [SC_USE_SPACES] = { SETTINGS_EDITOR, SETTINGS_CHECK, L"Use spaces for indentation", 0, 112, 320, 22, &g_indentUseSpaces, 1, NULL },
    [SC_TAB_WIDTH_CAPTION] = { SETTINGS_EDITOR, SETTINGS_TEXT, L"Spaces per tab:", 0, 140, 120, 24, NULL, 0, NULL },
    [SC_TAB_WIDTH] = { SETTINGS_EDITOR, SETTINGS_NUMBER_EDIT, NULL, 124, 140, 50, 24, NULL, 0, NULL },
    [SC_SPELL_LANGUAGE_CAPTION] = { SETTINGS_EDITOR, SETTINGS_TEXT, L"Spell check language:", 0, 172, 200, 20, NULL, 0, NULL },
    [SC_SPELL_LANGUAGE_LABEL] = { SETTINGS_EDITOR, SETTINGS_TEXT, NULL, 0, 194, 320, 20, NULL, 0, NULL },
    [SC_SPELL_LANGUAGE_CHOOSE] = { SETTINGS_EDITOR, SETTINGS_BUTTON, L"Choose...", 0, 220, 120, 26, NULL, 0, NULL },

    [SC_COMPILER_HINT] = { SETTINGS_COMPILER, SETTINGS_TEXT,
        L"Optional overrides (leave blank to search PATH automatically):", 0, 0, 380, 18, NULL, 0, NULL },
#define COMPILER_PATH_ROW(name, label, y, slot) \
    [name##_CAPTION] = { SETTINGS_COMPILER, SETTINGS_TEXT, label, 0, (y), 90, 24, NULL, 0, NULL }, \
    [name] = { SETTINGS_COMPILER, SETTINGS_PATH_EDIT, NULL, 96, (y), 176, 24, NULL, 0, (slot) }, \
    [name##_BROWSE] = { SETTINGS_COMPILER, SETTINGS_BUTTON, L"Browse...", 280, (y), 90, 24, NULL, 0, NULL }
    COMPILER_PATH_ROW(SC_VCVARS, L"MSVC vcvars:", 22, &g_vcvarsallOverridePath),
    COMPILER_PATH_ROW(SC_GCC, L"GCC/G++:", 56, &g_gccBinOverride),
    COMPILER_PATH_ROW(SC_CLANG, L"Clang:", 90, &g_clangBinOverride),
    COMPILER_PATH_ROW(SC_ZIG, L"Zig:", 124, &g_zigBinOverride),
    COMPILER_PATH_ROW(SC_LUA, L"Lua:", 158, &g_luaBinOverride),
    COMPILER_PATH_ROW(SC_GDB, L"GDB:", 192, &g_gdbBinOverride),
#undef COMPILER_PATH_ROW
    [SC_FLAGS_CAPTION] = { SETTINGS_COMPILER, SETTINGS_TEXT,
        L"Extra flags passed to Run/Debug C/C++ builds:", 0, 228, 380, 20, NULL, 0, NULL },
    [SC_FLAGS] = { SETTINGS_COMPILER, SETTINGS_PATH_EDIT, NULL, 0, 252, 380, 24, NULL, 0, &g_compilerExtraFlags },

    [SC_DIFFTOOL_USE] = { SETTINGS_GIT, SETTINGS_CHECK,
        L"Open file diffs in an external diff tool instead of the Output panel",
        0, 0, 380, 22, &g_useExternalDiffTool, 1, NULL },
    [SC_DIFFTOOL_CAPTION] = { SETTINGS_GIT, SETTINGS_TEXT, L"Diff tool (e.g. WinMerge, Beyond Compare):",
        0, 36, 380, 20, NULL, 0, NULL },
    [SC_DIFFTOOL] = { SETTINGS_GIT, SETTINGS_PATH_EDIT, NULL, 0, 58, 280, 24, NULL, 0, &g_externalDiffToolPath },
    [SC_DIFFTOOL_BROWSE] = { SETTINGS_GIT, SETTINGS_BUTTON, L"Browse...", 288, 58, 92, 24, NULL, 0, NULL },
    [SC_MERGETOOL_CAPTION] = { SETTINGS_GIT, SETTINGS_TEXT,
        L"Merge tool (used to resolve conflicts, optional):", 0, 96, 380, 20, NULL, 0, NULL },
    [SC_MERGETOOL] = { SETTINGS_GIT, SETTINGS_PATH_EDIT, NULL, 0, 118, 280, 24, NULL, 0, &g_externalMergeToolPath },
    [SC_MERGETOOL_BROWSE] = { SETTINGS_GIT, SETTINGS_BUTTON, L"Browse...", 288, 118, 92, 24, NULL, 0, NULL },

    [SC_PLUGINS_HINT] = { SETTINGS_PLUGINS, SETTINGS_TEXT,
        L"Install, enable, or disable individual plugins:", 0, 0, 380, 20, NULL, 0, NULL },
    [SC_PLUGINS_MANAGE] = { SETTINGS_PLUGINS, SETTINGS_BUTTON, L"Manage Plugins...", 0, 28, 140, 26, NULL, 0, NULL },

    [SC_PLUGIN_VALUES_HINT] = { SETTINGS_PLUGIN_VALUES, SETTINGS_TEXT,
        L"Settings (built-in defaults are shown when no value exists):", 0, 0, 380, 20, NULL, 0, NULL },
    [SC_PLUGIN_VALUE_ADD] = { SETTINGS_PLUGIN_VALUES, SETTINGS_BUTTON, L"Add...", 0, 258, 90, 26, NULL, 0, NULL },
    [SC_PLUGIN_VALUE_EDIT] = { SETTINGS_PLUGIN_VALUES, SETTINGS_BUTTON, L"Edit...", 98, 258, 90, 26, NULL, 0, NULL },
    [SC_PLUGIN_VALUE_REMOVE] = { SETTINGS_PLUGIN_VALUES, SETTINGS_BUTTON, L"Remove", 196, 258, 90, 26, NULL, 0, NULL },
};

typedef struct SettingsDialogState {
    HWND categoryList;
    int activeCategory;
    char *activePlugin;
    HWND controls[SC_COUNT];
    HWND pluginValuesList;
    PluginSettingEntry *pluginValues;
    int pluginValueCount;
} SettingsDialogState;

typedef struct KnownPluginSetting {
    const char *section;
    const char *key;
    const char *defaultValue;
} KnownPluginSetting;

static int plugin_value_is_boolean(const PluginSettingEntry *entry);
static int plugin_value_is_checked(const PluginSettingEntry *entry);

static const KnownPluginSetting KNOWN_PLUGIN_SETTINGS[] = {
    { "Plugin:constraint-writer.lua", "forbidden_words", "" },
    { "Plugin:constraint-writer.lua", "forbidden_letter", "" },
    { "Plugin:constraint-writer.lua", "word_target", "0" },
    { "Plugin:constraint-writer.lua", "max_sentence_words", "20" },
    { "Plugin:line-tools.lua", "align_delimiter", "=" },
};

static char *settings_copy_string(const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (copy) memcpy(copy, text, length + 1);
    return copy;
}

static int is_known_plugin_setting(const char *section, const char *key) {
    for (size_t i = 0; i < sizeof(KNOWN_PLUGIN_SETTINGS) / sizeof(KNOWN_PLUGIN_SETTINGS[0]); i++)
        if (strcmp(section, KNOWN_PLUGIN_SETTINGS[i].section) == 0 &&
            strcmp(key, KNOWN_PLUGIN_SETTINGS[i].key) == 0) return 1;
    return 0;
}

static PluginSettingEntry *find_plugin_setting(PluginSettingEntry *entries, int count,
                                                const char *section, const char *key) {
    for (int i = 0; i < count; i++)
        if (strcmp(entries[i].section, section) == 0 && strcmp(entries[i].key, key) == 0)
            return &entries[i];
    return NULL;
}

static int copy_plugin_setting(PluginSettingEntry *target, const char *section,
                               const char *key, const char *value) {
    target->section = settings_copy_string(section);
    target->key = settings_copy_string(key);
    target->value = settings_copy_string(value);
    return target->section && target->key && target->value;
}

static const char *plugin_setting_owner(const char *section) {
    return strncmp(section, "Plugin:", 7) == 0 ? section + 7 : section;
}

static void append_plugin_setting_rows(SettingsDialogState *state, const char *plugin) {
    for (int i = 0; i < state->pluginValueCount; i++) {
        PluginSettingEntry *entry = &state->pluginValues[i];
        if (_stricmp(plugin_setting_owner(entry->section), plugin) != 0) continue;
        WCHAR *wideKey = to_wide(entry->key);
        WCHAR *wideValue = to_wide(entry->value);
        if (wideKey && wideValue) {
            WCHAR label[512];
            if (plugin_value_is_boolean(entry))
                swprintf(label, sizeof(label) / sizeof(label[0]), L"    %ls", wideKey);
            else
                swprintf(label, sizeof(label) / sizeof(label[0]), L"    %ls = %.180ls",
                         wideKey, wideValue);
            int row = (int)SendMessageW(state->pluginValuesList, LB_ADDSTRING, 0, (LPARAM)label);
            if (row != LB_ERR && row != LB_ERRSPACE)
                SendMessageW(state->pluginValuesList, LB_SETITEMDATA, (WPARAM)row, (LPARAM)i);
        }
        free(wideValue); free(wideKey);
    }
}

static int find_plugin_category(SettingsDialogState *state, const WCHAR *plugin) {
    int rows = (int)SendMessageW(state->categoryList, LB_GETCOUNT, 0, 0);
    for (int row = 0; row < rows; row++) {
        if ((int)SendMessageW(state->categoryList, LB_GETITEMDATA, (WPARAM)row, 0) !=
            SETTINGS_PLUGIN_VALUES) continue;
        WCHAR name[320];
        if (SendMessageW(state->categoryList, LB_GETTEXT, (WPARAM)row, (LPARAM)name) != LB_ERR &&
            _wcsicmp(name, plugin) == 0) return row;
    }
    return -1;
}

static void ensure_plugin_category(SettingsDialogState *state, const char *plugin) {
    WCHAR *widePlugin = to_wide(plugin);
    if (!widePlugin) return;
    if (find_plugin_category(state, widePlugin) < 0) {
        int row = (int)SendMessageW(state->categoryList, LB_ADDSTRING, 0, (LPARAM)widePlugin);
        if (row != LB_ERR && row != LB_ERRSPACE)
            SendMessageW(state->categoryList, LB_SETITEMDATA, (WPARAM)row,
                         (LPARAM)SETTINGS_PLUGIN_VALUES);
    }
    free(widePlugin);
}

/* Lists the settings plugins can read through config.get(). Known prompt-based
   bundled settings are always present using their defaults; stored custom keys
   follow them. This keeps a freshly installed copy configurable too. */
static void populate_settings_plugin_values(SettingsDialogState *state) {
    plugin_settings_free_entries(state->pluginValues, state->pluginValueCount);
    state->pluginValues = NULL;
    state->pluginValueCount = 0;
    SendMessageW(state->pluginValuesList, LB_RESETCONTENT, 0, 0);

    WCHAR *path = get_settings_path();
    PluginSettingEntry *stored = NULL;
    int storedCount = 0;
    if (!path || !plugin_settings_list(path, "Plugin:", &stored, &storedCount)) {
        free(path);
        return;
    }
    free(path);
    int knownCount = (int)(sizeof(KNOWN_PLUGIN_SETTINGS) / sizeof(KNOWN_PLUGIN_SETTINGS[0]));
    int commandCount = plugin_host_command_count();
    PluginSettingEntry *merged = (PluginSettingEntry *)calloc(
        (size_t)(knownCount + storedCount + commandCount), sizeof(*merged));
    if (!merged) {
        plugin_settings_free_entries(stored, storedCount);
        return;
    }
    int count = 0, ok = 1;
    for (int i = 0; i < knownCount && ok; i++) {
        const KnownPluginSetting *known = &KNOWN_PLUGIN_SETTINGS[i];
        PluginSettingEntry *saved = find_plugin_setting(stored, storedCount, known->section, known->key);
        ok = copy_plugin_setting(&merged[count], known->section, known->key,
                                 saved ? saved->value : known->defaultValue);
        count++;
    }
    for (int i = 0; i < storedCount && ok; i++) {
        if (is_known_plugin_setting(stored[i].section, stored[i].key)) continue;
        ok = copy_plugin_setting(&merged[count], stored[i].section, stored[i].key, stored[i].value);
        count++;
    }
    /* Checkbox defaults do not exist in settings.json until first toggled.
       Include every registered one so its plugin panel is their sole UI home. */
    for (int i = 0; i < commandCount && ok; i++) {
        const char *owner = plugin_host_command_checkbox_owner(i);
        const char *key = plugin_host_command_checkbox_key(i);
        if (!owner || !key) continue;
        size_t sectionLength = strlen(owner) + 8;
        char *section = (char *)malloc(sectionLength);
        if (!section) { ok = 0; break; }
        snprintf(section, sectionLength, "Plugin:%s", owner);
        if (!find_plugin_setting(merged, count, section, key)) {
            ok = copy_plugin_setting(&merged[count], section, key,
                                     plugin_host_command_is_checked(i) ? "1" : "0");
            count++;
        }
        free(section);
    }
    plugin_settings_free_entries(stored, storedCount);
    if (!ok) {
        plugin_settings_free_entries(merged, count);
        return;
    }
    state->pluginValues = merged;
    state->pluginValueCount = count;
    /* Plugins get their own category only when they have at least one value
       to show. Saved values keep removed plugins reachable until the user
       removes those values. */
    for (int i = 0; i < count; i++)
        ensure_plugin_category(state, plugin_setting_owner(merged[i].section));
    if (state->activeCategory == SETTINGS_PLUGIN_VALUES && state->activePlugin)
        append_plugin_setting_rows(state, state->activePlugin);
}

static void update_settings_font_label(SettingsDialogState *state) {
    WCHAR text[MAX_FONT_FACE_NAME + 32];
    swprintf(text, sizeof(text) / sizeof(text[0]), L"%ls, %dpt", g_fontFaceName, g_fontPointSize);
    SetWindowTextW(state->controls[SC_FONT_LABEL], text);
}

static void update_settings_css_label(SettingsDialogState *state) {
    SetWindowTextW(state->controls[SC_CSS_LABEL],
                   g_customCssPath && g_customCssPath[0] ? g_customCssPath : L"(none)");
    EnableWindow(state->controls[SC_CSS_CLEAR], g_customCssPath && g_customCssPath[0]);
}

/* Mirrors choose_spell_language()'s own label building (LOCALE_SLOCALIZEDDISPLAYNAME
   over the raw BCP-47 tag) so the Editor category shows the same name the
   language picker itself lists it under. */
static void update_settings_spell_label(SettingsDialogState *state) {
    WCHAR displayName[LOCALE_NAME_MAX_LENGTH] = L"";
    if (g_spellLanguage[0])
        GetLocaleInfoEx(g_spellLanguage, LOCALE_SLOCALIZEDDISPLAYNAME, displayName, LOCALE_NAME_MAX_LENGTH);
    SetWindowTextW(state->controls[SC_SPELL_LANGUAGE_LABEL], displayName[0] ? displayName : g_spellLanguage);
}

/* Whether a check box or radio button shows as on: read from the setting
   itself, so it can't drift from what the menu items toggle. */
static int settings_toggle_is_on(SettingsControl control) {
    const SettingsControlSpec *spec = &SETTINGS_CONTROLS[control];
    return spec->setting && (*spec->setting != 0) == (spec->value != 0);
}

/* Repaints every check box and radio button after one of them was clicked:
   picking a radio button also clears the others in its group. */
static void refresh_settings_toggles(SettingsDialogState *state) {
    for (int i = 0; i < SC_COUNT; i++)
        if (SETTINGS_CONTROLS[i].setting) InvalidateRect(state->controls[i], NULL, FALSE);
}

static void update_settings_tab_width(SettingsDialogState *state) {
    WCHAR text[16];
    swprintf(text, sizeof(text) / sizeof(text[0]), L"%d", g_tabWidth);
    SetWindowTextW(state->controls[SC_TAB_WIDTH], text);
}

static int is_flag_edit_whitespace(WCHAR ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

/* Applied on EN_KILLFOCUS from any of the Compiler category's edit boxes
   (the extra-flags box and the GCC/Clang/Zig/Lua/GDB/MSVC path overrides alike),
   the same as set_tab_width() is for the Editor category's - trims
   surrounding whitespace, persists via save_compiler_settings(), and is a
   no-op when the trimmed text hasn't actually changed so tabbing through a
   field without editing it doesn't rewrite settings.json. For the flags box,
   quote_extra_compiler_flags() (see launch_native_file()/launch_msvc_file())
   re-splits the saved text into individual flags at build time. */
/* Trims surrounding whitespace and stores the result in *slot if it actually
   changed. Returns 1 when it did (so the caller knows to persist), 0 when
   tabbing through the field without editing it left nothing to save. */
static int set_trimmed_wstring_setting(WCHAR **slot, const WCHAR *text) {
    size_t start = 0, end = text ? wcslen(text) : 0;
    while (start < end && is_flag_edit_whitespace(text[start])) start++;
    while (end > start && is_flag_edit_whitespace(text[end - 1])) end--;
    WCHAR *value = NULL;
    if (end > start) {
        value = (WCHAR *)malloc((end - start + 1) * sizeof(WCHAR));
        if (value) {
            for (size_t i = 0; i < end - start; i++) value[i] = text[start + i];
            value[end - start] = 0;
        }
    }
    if (value && *slot && wcscmp(value, *slot) == 0) {
        free(value);
        return 0;
    }
    if (!value && !*slot) return 0;
    free(*slot);
    *slot = value;
    return 1;
}

static void set_compiler_string_setting(WCHAR **slot, const WCHAR *text) {
    if (set_trimmed_wstring_setting(slot, text)) save_compiler_settings();
}

static void set_git_string_setting(WCHAR **slot, const WCHAR *text) {
    if (set_trimmed_wstring_setting(slot, text)) save_git_settings();
}

/* The MSVC path box's Browse button: unlike the GCC/Clang/Zig/Lua/GDB boxes
   below, an override here names one specific file (vcvarsall.bat) rather
   than a folder to search, so this picks a file the same way
   do_choose_custom_css() does instead of reusing choose_workspace_folder(). */
static void choose_vcvarsall_path(HWND editControl) {
    DWORD cap = LONG_PATH_CAPACITY;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    if (g_vcvarsallOverridePath) wcsncpy(path, g_vcvarsallOverridePath, cap - 1);
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"vcvarsall.bat\0vcvarsall.bat\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        replace_wstring(&g_vcvarsallOverridePath, path);
        save_compiler_settings();
        SetWindowTextW(editControl, g_vcvarsallOverridePath);
    }
    free(path);
}

/* Shared by the GCC/Clang/Zig/Lua/GDB Browse buttons: each of those overrides
   names a folder to search for that toolchain's executable(s) instead of
   PATH (see find_native_compiler()/find_lua_interpreter()), so picking one
   is just choose_workspace_folder()'s existing folder picker reused for a
   different setting. */
static void choose_compiler_folder(WCHAR **slot, HWND editControl) {
    WCHAR *folder = choose_workspace_folder();
    if (!folder) return;
    replace_wstring(slot, folder);
    free(folder);
    save_compiler_settings();
    SetWindowTextW(editControl, *slot);
}

/* The diff tool box's Browse button: like the MSVC vcvars box, this names one
   specific executable rather than a folder to search. */
static void choose_diff_tool_path(HWND editControl) {
    DWORD cap = LONG_PATH_CAPACITY;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    if (g_externalDiffToolPath) wcsncpy(path, g_externalDiffToolPath, cap - 1);
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        replace_wstring(&g_externalDiffToolPath, path);
        save_git_settings();
        SetWindowTextW(editControl, g_externalDiffToolPath);
    }
    free(path);
}

static void choose_merge_tool_path(HWND editControl) {
    DWORD cap = LONG_PATH_CAPACITY;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    if (g_externalMergeToolPath) wcsncpy(path, g_externalMergeToolPath, cap - 1);
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        replace_wstring(&g_externalMergeToolPath, path);
        save_git_settings();
        SetWindowTextW(editControl, g_externalMergeToolPath);
    }
    free(path);
}

static void toggle_use_external_diff_tool(void) {
    g_useExternalDiffTool = !g_useExternalDiffTool;
    save_git_settings();
}

static int selected_plugin_value(SettingsDialogState *state) {
    int row = (int)SendMessageW(state->pluginValuesList, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR) return -1;
    int index = (int)SendMessageW(state->pluginValuesList, LB_GETITEMDATA, (WPARAM)row, 0);
    return index >= 0 && index < state->pluginValueCount ? index : -1;
}

static void select_plugin_value(SettingsDialogState *state, int index) {
    int rows = (int)SendMessageW(state->pluginValuesList, LB_GETCOUNT, 0, 0);
    for (int row = 0; row < rows; row++) {
        if ((int)SendMessageW(state->pluginValuesList, LB_GETITEMDATA, (WPARAM)row, 0) != index)
            continue;
        SendMessageW(state->pluginValuesList, LB_SETCURSEL, (WPARAM)row, 0);
        return;
    }
}

static void update_plugin_value_buttons(SettingsDialogState *state) {
    int selected = selected_plugin_value(state) >= 0;
    EnableWindow(state->controls[SC_PLUGIN_VALUE_EDIT], selected);
    EnableWindow(state->controls[SC_PLUGIN_VALUE_REMOVE], selected);
}

static int write_plugin_value(const char *section, const char *key, const char *value) {
    WCHAR *path = get_settings_path();
    int ok = path && plugin_settings_set(path, section, key, value);
    free(path);
    if (!ok)
        MessageBoxW(g_hMainWnd, L"Could not save the plugin setting.", L"Plugin Settings",
                    MB_OK | MB_ICONERROR);
    return ok;
}

static int plugin_value_checkbox_command(const PluginSettingEntry *entry) {
    const char *owner = strncmp(entry->section, "Plugin:", 7) == 0
        ? entry->section + 7 : entry->section;
    return plugin_host_find_checkbox(owner, entry->key);
}

static int plugin_value_is_boolean(const PluginSettingEntry *entry) {
    return plugin_value_checkbox_command(entry) >= 0 ||
           _stricmp(entry->value, "true") == 0 || _stricmp(entry->value, "false") == 0;
}

static int plugin_value_is_checked(const PluginSettingEntry *entry) {
    int command = plugin_value_checkbox_command(entry);
    if (command >= 0) return plugin_host_command_is_checked(command);
    return _stricmp(entry->value, "true") == 0;
}

static void toggle_plugin_value(SettingsDialogState *state, int index) {
    if (index < 0 || index >= state->pluginValueCount ||
        !plugin_value_is_boolean(&state->pluginValues[index])) return;
    PluginSettingEntry *entry = &state->pluginValues[index];
    int command = plugin_value_checkbox_command(entry);
    if (command >= 0)
        run_plugin_command(command);
    else
        write_plugin_value(entry->section, entry->key,
                           plugin_value_is_checked(entry) ? "false" : "true");
    populate_settings_plugin_values(state);
    if (index < state->pluginValueCount)
        select_plugin_value(state, index);
    update_plugin_value_buttons(state);
}

static LRESULT CALLBACK SettingsPluginValuesSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData) {
    SettingsDialogState *state = (SettingsDialogState *)refData;
    if (msg == WM_KEYDOWN && wParam == VK_SPACE) {
        toggle_plugin_value(state, selected_plugin_value(state));
        return 0;
    }
    if (msg == WM_CHAR && wParam == L' ') return 0;
    if (msg == WM_LBUTTONDOWN) {
        int row = checklist_row_at(hwnd, lParam);
        RECT rowRect;
        int index = row >= 0
            ? (int)SendMessageW(hwnd, LB_GETITEMDATA, (WPARAM)row, 0) : -1;
        int onBox = row >= 0 && index >= 0 && index < state->pluginValueCount &&
            plugin_value_is_boolean(&state->pluginValues[index]) &&
            SendMessageW(hwnd, LB_GETITEMRECT, (WPARAM)row, (LPARAM)&rowRect) != LB_ERR &&
            GET_X_LPARAM(lParam) < checklist_box_rect(hwnd, &rowRect).right + checklist_scaled(hwnd, 4);
        LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
        if (onBox) toggle_plugin_value(state, index);
        return result;
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, SettingsPluginValuesSubclassProc, subclassId);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void add_plugin_value(SettingsDialogState *state) {
    if (!state->activePlugin) return;
    WCHAR key[65] = L"", value[4096] = L"";
    if (!show_text_input(L"Add Plugin Setting", L"Setting key (up to 64 characters):",
                         L"", key, (int)(sizeof(key) / sizeof(key[0]))) || !key[0]) return;
    if (!show_text_input(L"Add Plugin Setting", L"Setting value:",
                         L"", value, (int)(sizeof(value) / sizeof(value[0])))) return;

    char *utf8Key = to_utf8(key);
    char *utf8Value = to_utf8(value);
    char *section = (char *)malloc(strlen(state->activePlugin) + 8);
    if (section) sprintf(section, "Plugin:%s", state->activePlugin);
    if (section && utf8Key && utf8Value && write_plugin_value(section, utf8Key, utf8Value))
        populate_settings_plugin_values(state);
    else if (!section || !utf8Key || !utf8Value)
        MessageBoxW(g_hMainWnd, L"The plugin setting is too large to save.", L"Plugin Settings",
                    MB_OK | MB_ICONERROR);
    free(section); free(utf8Value); free(utf8Key);
    update_plugin_value_buttons(state);
}

static void edit_plugin_value(SettingsDialogState *state) {
    int index = selected_plugin_value(state);
    if (index < 0) return;
    PluginSettingEntry *entry = &state->pluginValues[index];
    WCHAR *initial = to_wide(entry->value);
    if (!initial) return;
    size_t capacity = wcslen(initial) + 4096;
    if (capacity > INT_MAX) { free(initial); return; }
    WCHAR *value = (WCHAR *)calloc(capacity, sizeof(WCHAR));
    if (!value) { free(initial); return; }
    if (show_text_input(L"Edit Plugin Setting", L"Setting value:", initial,
                        value, (int)capacity)) {
        char *utf8Value = to_utf8(value);
        if (utf8Value && write_plugin_value(entry->section, entry->key, utf8Value))
            populate_settings_plugin_values(state);
        free(utf8Value);
    }
    free(value);
    free(initial);
    update_plugin_value_buttons(state);
}

static void remove_plugin_value(SettingsDialogState *state) {
    int index = selected_plugin_value(state);
    if (index < 0) return;
    PluginSettingEntry *entry = &state->pluginValues[index];
    if (MessageBoxW(g_hMainWnd,
                    L"Remove this saved value? A bundled setting will return to its default.",
                    L"Plugin Settings", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (write_plugin_value(entry->section, entry->key, NULL))
        populate_settings_plugin_values(state);
    update_plugin_value_buttons(state);
}

/* Shows the active category's controls and hides every other one. */
static void update_settings_category(SettingsDialogState *state) {
    for (int i = 0; i < SC_COUNT; i++)
        ShowWindow(state->controls[i],
                   (int)SETTINGS_CONTROLS[i].category == state->activeCategory ? SW_SHOW : SW_HIDE);
    ShowWindow(state->pluginValuesList,
               state->activeCategory == SETTINGS_PLUGIN_VALUES ? SW_SHOW : SW_HIDE);
}

/* The shared g_popupBkBrush is normally created once when a popup dialog
   opens and never changes for that dialog's lifetime (see its own comment
   above) - a fine assumption for every other popup here, none of which can
   change the theme while they're open. This one can (the Appearance
   radios), so picking a theme needs to also recreate that brush and re-skin
   the controls that were themed at creation time, then force everything to
   repaint with the new colors instead of leaving stale ones on screen. */
static void refresh_settings_dialog_theme(HWND hwnd, SettingsDialogState *state) {
    popup_dialog_theme_created(hwnd);
    apply_explorer_theme(state->categoryList);
    /* RDW_FRAME repaints the themed borders of the list and edit fields. */
    RedrawWindow(hwnd, NULL, NULL,
                 RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void create_settings_category_list(HWND hwnd, SettingsDialogState *state, HINSTANCE instance) {
    static const WCHAR *const categories[] = {
        L"Appearance", L"Font", L"Word Wrap", L"Preview", L"Editor", L"Compiler", L"Git", L"Plugins"
    };
    state->categoryList = CreateWindowExW(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
        LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        12, 12, 180, 300, hwnd, (HMENU)ID_SETTINGS_CATEGORIES, instance, NULL);
    theme_list_frame(state->categoryList);
    for (int i = 0; i <= SETTINGS_PLUGINS; i++) {
        int row = (int)SendMessageW(state->categoryList, LB_ADDSTRING, 0, (LPARAM)categories[i]);
        if (row != LB_ERR && row != LB_ERRSPACE)
            SendMessageW(state->categoryList, LB_SETITEMDATA, (WPARAM)row, (LPARAM)i);
    }
    apply_explorer_theme(state->categoryList);
}

/* Creates one control from its SETTINGS_CONTROLS row, positioned relative
   to the panel at (panelX, panelY), and shows the setting it reflects. */
static HWND create_settings_control(HWND hwnd, HINSTANCE instance, SettingsControl control,
                                    int panelX, int panelY) {
    const SettingsControlSpec *spec = &SETTINGS_CONTROLS[control];
    const WCHAR *windowClass = L"BUTTON";
    DWORD style = WS_CHILD | WS_TABSTOP;
    switch (spec->kind) {
        /* Centred vertically, so a caption given its text box's row lines up with it. */
        case SETTINGS_TEXT: windowClass = L"STATIC"; style = WS_CHILD | SS_CENTERIMAGE; break;
        /* Check boxes and radio buttons are owner-drawn too, by
           draw_toggle_button(), from the setting they show. */
        case SETTINGS_CHECK: case SETTINGS_RADIO: case SETTINGS_BUTTON: style |= BS_OWNERDRAW; break;
        case SETTINGS_RADIO_FIRST: style |= WS_GROUP | BS_OWNERDRAW; break;
        case SETTINGS_NUMBER_EDIT: style |= ES_NUMBER; /* fall through */
        case SETTINGS_PATH_EDIT:
            windowClass = L"EDIT"; style |= ES_AUTOHSCROLL; break;
    }
    HMENU id = spec->kind == SETTINGS_TEXT ? NULL : (HMENU)(UINT_PTR)(ID_SETTINGS_FIRST + control);
    HWND window = CreateWindowExW(0, windowClass, spec->text ? spec->text : L"", style,
        panelX + spec->x, panelY + spec->y, spec->width, spec->height, hwnd, id, instance, NULL);
    if (spec->kind == SETTINGS_NUMBER_EDIT || spec->kind == SETTINGS_PATH_EDIT) theme_edit_field(window);
    if (spec->path) SetWindowTextW(window, *spec->path ? *spec->path : L"");
    return window;
}

static void initialize_settings_dialog(HWND hwnd, SettingsDialogState *state) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    const int panelX = 208, panelY = 16;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
    create_settings_category_list(hwnd, state, instance);
    for (int i = 0; i < SC_COUNT; i++)
        state->controls[i] = create_settings_control(hwnd, instance, (SettingsControl)i, panelX, panelY);
    state->pluginValuesList = CreateWindowExW(0, L"LISTBOX", L"",
        WS_CHILD | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
        LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        panelX, panelY + 24, 380, 226, hwnd, (HMENU)ID_SETTINGS_PLUGIN_VALUES, instance, NULL);
    theme_list_frame(state->pluginValuesList);
    apply_explorer_theme(state->pluginValuesList);
    SetWindowSubclass(state->pluginValuesList, SettingsPluginValuesSubclassProc,
                      ID_SETTINGS_PLUGIN_VALUES, (DWORD_PTR)state);
    populate_settings_plugin_values(state);
    update_plugin_value_buttons(state);
    EnableWindow(state->controls[SC_SPELL_CHECK], g_spellChecker != NULL);
    EnableWindow(state->controls[SC_SPELL_LANGUAGE_CHOOSE], g_spellChecker != NULL);
    CreateWindowExW(0, L"BUTTON", L"Close",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
        536, 330, 78, 26, hwnd, (HMENU)IDCANCEL, instance, NULL);
    use_gui_font(hwnd);
    remeasure_listbox_rows(state->categoryList, (UINT)MulDiv(28, GetDpiForWindow(hwnd), 96));
    update_settings_font_label(state);
    update_settings_css_label(state);
    update_settings_spell_label(state);
    update_settings_tab_width(state);
    SendMessageW(state->categoryList, LB_SETCURSEL, 0, 0);
    update_settings_category(state);
    SetFocus(state->categoryList);
    popup_dialog_theme_opened(hwnd);
}

/* An edit box lost focus: store what it now says. */
static void commit_settings_edit(SettingsDialogState *state, SettingsControl control) {
    HWND edit = state->controls[control];
    if (control == SC_TAB_WIDTH) {
        WCHAR text[16];
        GetWindowTextW(edit, text, sizeof(text) / sizeof(text[0]));
        set_tab_width((int)wcstol(text, NULL, 10));
        update_settings_tab_width(state);
        return;
    }
    WCHAR **slot = SETTINGS_CONTROLS[control].path;
    if (!slot) return;
    WCHAR text[1024];
    GetWindowTextW(edit, text, sizeof(text) / sizeof(text[0]));
    if (SETTINGS_CONTROLS[control].category == SETTINGS_GIT) set_git_string_setting(slot, text);
    else set_compiler_string_setting(slot, text);
    SetWindowTextW(edit, *slot ? *slot : L"");
}

static void handle_settings_button(HWND hwnd, SettingsDialogState *state, SettingsControl control) {
    switch (control) {
        case SC_THEME_LIGHT: select_theme(g_hMainWnd, 0); refresh_settings_dialog_theme(hwnd, state); break;
        case SC_THEME_DARK: select_theme(g_hMainWnd, 1); refresh_settings_dialog_theme(hwnd, state); break;
        case SC_FONT_CHOOSE: do_choose_font(); update_settings_font_label(state); break;
        case SC_WORD_WRAP: toggle_word_wrap(g_hMainWnd); break;
        case SC_PREVIEW_ON_LEFT: toggle_panel_sides(g_hMainWnd); break;
        case SC_CSS_CHOOSE: do_choose_custom_css(); update_settings_css_label(state); break;
        case SC_CSS_CLEAR: clear_custom_css(); update_settings_css_label(state); break;
        case SC_PREVIEW_THEME_LIGHT: select_preview_theme(g_hMainWnd, 0); break;
        case SC_PREVIEW_THEME_DARK: select_preview_theme(g_hMainWnd, 1); break;
        case SC_SPELL_CHECK: toggle_spell_check(g_hMainWnd); break;
        case SC_SHOW_WHITESPACE: toggle_show_whitespace(g_hMainWnd); break;
        case SC_INDENT_GUIDES: toggle_indent_guides(g_hMainWnd); break;
        case SC_MINIMAP: toggle_minimap(g_hMainWnd); break;
        case SC_USE_SPACES: toggle_indent_use_spaces(g_hMainWnd); break;
        case SC_SPELL_LANGUAGE_CHOOSE: choose_spell_language(); update_settings_spell_label(state); break;
        case SC_VCVARS_BROWSE: choose_vcvarsall_path(state->controls[SC_VCVARS]); break;
        case SC_GCC_BROWSE: case SC_CLANG_BROWSE: case SC_ZIG_BROWSE:
        case SC_LUA_BROWSE: case SC_GDB_BROWSE:
            /* Each Browse button directly follows the path box it fills in. */
            choose_compiler_folder(SETTINGS_CONTROLS[control - 1].path, state->controls[control - 1]);
            break;
        case SC_DIFFTOOL_USE: toggle_use_external_diff_tool(); break;
        case SC_DIFFTOOL_BROWSE: choose_diff_tool_path(state->controls[SC_DIFFTOOL]); break;
        case SC_MERGETOOL_BROWSE: choose_merge_tool_path(state->controls[SC_MERGETOOL]); break;
        case SC_PLUGINS_MANAGE: manage_plugins(); populate_settings_plugin_values(state); break;
        case SC_PLUGIN_VALUE_ADD: add_plugin_value(state); break;
        case SC_PLUGIN_VALUE_EDIT: edit_plugin_value(state); break;
        case SC_PLUGIN_VALUE_REMOVE: remove_plugin_value(state); break;
        default: break;
    }
}

static void handle_settings_command(HWND hwnd, SettingsDialogState *state, WPARAM wParam) {
    WORD controlId = LOWORD(wParam);
    WORD event = HIWORD(wParam);
    if (controlId == ID_SETTINGS_CATEGORIES) {
        if (event != LBN_SELCHANGE) return;
        int selection = (int)SendMessageW(state->categoryList, LB_GETCURSEL, 0, 0);
        if (selection != LB_ERR) {
            int category = (int)SendMessageW(state->categoryList, LB_GETITEMDATA,
                                             (WPARAM)selection, 0);
            free(state->activePlugin);
            state->activePlugin = NULL;
            state->activeCategory = category;
            if (category == SETTINGS_PLUGIN_VALUES) {
                int length = (int)SendMessageW(state->categoryList, LB_GETTEXTLEN,
                                               (WPARAM)selection, 0);
                WCHAR *name = length >= 0 ? (WCHAR *)calloc((size_t)length + 1, sizeof(WCHAR)) : NULL;
                if (name) {
                    SendMessageW(state->categoryList, LB_GETTEXT, (WPARAM)selection, (LPARAM)name);
                    state->activePlugin = to_utf8(name);
                    free(name);
                }
                populate_settings_plugin_values(state);
            }
            update_settings_category(state);
            update_plugin_value_buttons(state);
        }
    } else if (controlId == ID_SETTINGS_PLUGIN_VALUES) {
        if (event == LBN_SELCHANGE) update_plugin_value_buttons(state);
        else if (event == LBN_DBLCLK) {
            int index = selected_plugin_value(state);
            if (index >= 0 && plugin_value_is_boolean(&state->pluginValues[index]))
                toggle_plugin_value(state, index);
            else
                edit_plugin_value(state);
        }
    } else if (controlId == IDCANCEL) {
        DestroyWindow(hwnd);
    } else if (controlId >= ID_SETTINGS_FIRST && controlId < ID_SETTINGS_FIRST + SC_COUNT) {
        SettingsControl control = (SettingsControl)(controlId - ID_SETTINGS_FIRST);
        if (event == EN_KILLFOCUS) commit_settings_edit(state, control);
        else if (event == BN_CLICKED) {
            handle_settings_button(hwnd, state, control);
            refresh_settings_toggles(state);
        }
    }
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsDialogState *state = (SettingsDialogState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        state = (SettingsDialogState *)((CREATESTRUCTW *)lParam)->lpCreateParams;
        initialize_settings_dialog(hwnd, state);
        return 0;
    }
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
        case WM_ERASEBKGND: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN:
        case WM_CTLCOLORLISTBOX: {
            LRESULT handled = 0;
            LRESULT result = popup_dialog_theme_message(hwnd, msg, wParam, &handled);
            if (handled) return result;
            break;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
            if (mis->CtlType == ODT_LISTBOX) {
                HWND target = mis->CtlID == ID_SETTINGS_PLUGIN_VALUES ? state->pluginValuesList
                    : state->categoryList;
                measure_themed_listbox_item(target, mis);
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) {
                int control = (int)dis->CtlID - ID_SETTINGS_FIRST;
                SettingsControlKind kind = control >= 0 && control < SC_COUNT
                    ? SETTINGS_CONTROLS[control].kind : SETTINGS_BUTTON;
                if (kind == SETTINGS_CHECK || kind == SETTINGS_RADIO || kind == SETTINGS_RADIO_FIRST)
                    draw_toggle_button(dis, kind != SETTINGS_CHECK, settings_toggle_is_on((SettingsControl)control));
                else
                    draw_popup_button(dis);
                return TRUE;
            }
            if (dis->CtlType == ODT_LISTBOX && dis->hwndItem == state->categoryList) {
                draw_themed_listbox_item(state->categoryList, dis);
                return TRUE;
            }
            if (dis->CtlType == ODT_LISTBOX && dis->hwndItem == state->pluginValuesList) {
                int index = (int)dis->itemData;
                if (index >= 0 && index < state->pluginValueCount &&
                    plugin_value_is_boolean(&state->pluginValues[index])) {
                    Checklist view = { state->pluginValuesList, NULL, NULL };
                    draw_checklist_item(&view, dis, plugin_value_is_checked(&state->pluginValues[index]),
                                        plugin_value_is_checked(&state->pluginValues[index]) ? L"True" : L"False");
                } else {
                    draw_themed_listbox_item(state->pluginValuesList, dis);
                }
                return TRUE;
            }
            break;
        }
        case WM_NCDESTROY:
            plugin_settings_free_entries(state->pluginValues, state->pluginValueCount);
            state->pluginValues = NULL;
            state->pluginValueCount = 0;
            free(state->activePlugin);
            state->activePlugin = NULL;
            popup_dialog_theme_destroyed();
            break;
        case WM_COMMAND:
            handle_settings_command(hwnd, state, wParam);
            return 0;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void show_settings_dialog(void) {
    if (!register_popup_class(L"TinkerEditorSettingsDialog", SettingsWndProc)) return;
    SettingsDialogState state = {0};
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"TinkerEditorSettingsDialog", L"Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 660, 420, g_hMainWnd, NULL,
        GetModuleHandleW(NULL), &state);
    if (!dialog) return;
    run_modal_popup(dialog);
}

/* Menu commands, grouped by the feature they operate on. */
