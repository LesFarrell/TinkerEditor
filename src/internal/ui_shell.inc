static HWND active_edit_view(void);
static void show_plugin_error(const char *report);

/* Backing storage for the status bar's four segments (0: Ln/Col, 1: Words/
   Characters, 2: encoding, 3: save state). The strip is fully repainted by
   StatusSubclassProc below instead of through SB_SETTEXT + owner-draw:
   comctl32 draws a light 3D bevel between parts by itself regardless of
   SBT_OWNERDRAW/SBT_NOBORDERS, which reads as bright seams against a dark
   theme. Parts must own their text for as long as the control might repaint
   them, so a transient stack buffer would otherwise be read back as a
   dangling pointer once these functions return. */
static WCHAR g_statusPartText[4][128];

/* Set while a transient message (show_plugin_reload_status) occupies part 3,
   so routine refreshes triggered elsewhere (typing, caret moves, tab
   switches) don't stomp it before its own timer restores the normal text. */
static int g_statusMessageActive = 0;

/* The status bar deliberately computes from Scintilla's UTF-8 document model:
   character counts are Unicode-aware while columns remain editor columns. */
/* Ln/Col, encoding, and dirty-status fields only - all O(1) Scintilla
   queries, safe to call on every caret move or scroll tick. Word/character
   counts need a full-document scan, so they're left to update_status_bar()
   and only refreshed when the text can actually have changed. */
static void update_status_position(void) {
    if (!g_hStatus || !g_hEdit || g_documentStore.count <= 0) return;
    if (active_document_is_image()) {
        wcscpy(g_statusPartText[0], L"Image preview");
        g_statusPartText[2][0] = 0;
        if (!g_statusMessageActive) wcscpy(g_statusPartText[3], L"Read only");
        InvalidateRect(g_hStatus, NULL, FALSE);
        return;
    }
    HWND view = tracked_edit_view();
    Sci_Position position = (Sci_Position)SendMessageA(view, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position line = (Sci_Position)SendMessageA(view, SCI_LINEFROMPOSITION, (WPARAM)position, 0) + 1;
    Sci_Position column = (Sci_Position)SendMessageA(view, SCI_GETCOLUMN, (WPARAM)position, 0) + 1;
    swprintf(g_statusPartText[0], 128, L"Ln %lld, Col %lld", (long long)line, (long long)column);
    int eol = (int)SendMessageA(view, SCI_GETEOLMODE, 0, 0);
    const WCHAR *eolName = eol == SC_EOL_LF ? L"LF" : eol == SC_EOL_CR ? L"CR" : L"CRLF";
    WCHAR encodingName[40];
    switch (g_encoding) {
        case TEXT_ENCODING_UTF16LE: wcscpy(encodingName, L"UTF-16 LE"); break;
        case TEXT_ENCODING_UTF16BE: wcscpy(encodingName, L"UTF-16 BE"); break;
        case TEXT_ENCODING_ANSI:
            swprintf(encodingName, 40, L"ANSI (CP %u)", text_ansi_code_page());
            break;
        default: wcscpy(encodingName, L"UTF-8"); break;
    }
    swprintf(g_statusPartText[2], 128, L"%ls%ls   %ls", encodingName, g_hasBom ? L" BOM" : L"", eolName);
    if (!g_statusMessageActive) {
        const WCHAR *saveState = !g_dirty ? L"Saved"
                 : g_largeFile ? L"Modified" : L"Modified · recovery enabled";
        if (g_gitStatusText[0]) swprintf(g_statusPartText[3], 128, L"%ls                              %ls", saveState, g_gitStatusText);
        else swprintf(g_statusPartText[3], 128, L"%ls", saveState);
    }
    InvalidateRect(g_hStatus, NULL, FALSE);
}

static void update_status_bar(void) {
    update_status_position();
    if (!g_hStatus || !g_hEdit || g_documentStore.count <= 0) return;
    if (active_document_is_image()) {
        g_statusPartText[1][0] = 0;
        InvalidateRect(g_hStatus, NULL, FALSE);
        return;
    }
    if (g_largeFile) {
        double mib = (double)(Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0) / (1024.0 * 1024.0);
        swprintf(g_statusPartText[1], 128, L"Large file mode   %.1f MB", mib);
        InvalidateRect(g_hStatus, NULL, FALSE);
        return;
    }
    Sci_Position length = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    Sci_Position characters = (Sci_Position)SendMessageA(g_hEdit, SCI_COUNTCHARACTERS, 0, (LPARAM)length);
    int words = 0;
    /* Scintilla owns a contiguous UTF-8 buffer. Reading it in place avoids a
       document-sized allocation and copy after every pause in typing. The
       pointer remains valid for this synchronous scan. */
    const char *text = (const char *)SendMessageA(g_hEdit, SCI_GETCHARACTERPOINTER, 0, 0);
    if (text) {
        int inWord = 0;
        for (Sci_Position i = 0; i < length; i++) {
            unsigned char ch = (unsigned char)text[i];
            int wordChar = ch >= 0x80 || isalnum(ch) || ch == '_' || ch == '\'';
            if (wordChar && !inWord) words++;
            inWord = wordChar;
        }
    }
    swprintf(g_statusPartText[1], 128, L"Words %d   Characters %lld", words, (long long)characters);
    InvalidateRect(g_hStatus, NULL, FALSE);
}

static void show_plugin_reload_status(const WCHAR *text) {
    if (!g_hStatus) return;
    KillTimer(g_hMainWnd, IDT_STATUS_UPDATE);
    const size_t capacity = sizeof(g_statusPartText[3]) / sizeof(g_statusPartText[3][0]);
    wcsncpy(g_statusPartText[3], text, capacity - 1);
    g_statusPartText[3][capacity - 1] = 0;
    g_statusMessageActive = 1;
    InvalidateRect(g_hStatus, NULL, FALSE);
    SetTimer(g_hMainWnd, IDT_PLUGIN_STATUS, PLUGIN_STATUS_DURATION_MS, NULL);
}

/* Defined in output_panel.inc, included after this file - the status bar's
   Git segment opens the same branch menu as the Git panel. */
static void show_git_branch_menu(HWND hwnd, POINT screenPoint);

/* Paints the whole status bar strip itself in the app's own GitHub palette:
   comctl32's default rendering has no opt-in for a custom light/dark palette
   the way the editor/menu/toolbar do, and (unlike those) drawing only the
   owner-drawn parts still leaves the control's own bevel between them, which
   shows up as light seams. SB_SETPARTS (via layout()) still establishes the
   four part rectangles this reads with SB_GETRECT; nothing else about the
   control's own painting is used. */
static LRESULT CALLBACK StatusSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR subclassId, DWORD_PTR refData) {
    (void)subclassId; (void)refData;
    if (msg == WM_ERASEBKGND) {
        return 1; /* WM_PAINT fills the whole strip itself. */
    } else if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        fill_rect_color(dc, &client, theme_bg_color());
        SetTextColor(dc, theme_fg_color());
        SetBkMode(dc, TRANSPARENT);
        for (int i = 0; i < 4; i++) {
            RECT partRect = {0};
            if (!SendMessageW(hwnd, SB_GETRECT, (WPARAM)i, (LPARAM)&partRect)) continue;
            partRect.left += 4;
            partRect.right -= 4;
            /* Part 3 is the only one with room to spare (it fills whatever
               the other three don't use - see layout()'s SB_SETPARTS), so
               the Git branch text reads from the status bar's right edge
               rather than floating right after the encoding part. */
            UINT align = i == 3 ? DT_RIGHT : DT_LEFT;
            DrawTextW(dc, g_statusPartText[i], -1, &partRect,
                      align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        EndPaint(hwnd, &ps);
        return 0;
    } else if (msg == WM_LBUTTONUP) {
        /* Part 3 holds the Git text (see update_status_position()); clicking
           it opens the branch menu directly instead of requiring the Git
           menu on the menu bar. */
        RECT partRect;
        if (g_gitStatusText[0] && SendMessageW(hwnd, SB_GETRECT, (WPARAM)3, (LPARAM)&partRect)) {
            POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (PtInRect(&partRect, point)) {
                POINT screenPoint = point;
                ClientToScreen(hwnd, &screenPoint);
                show_git_branch_menu(hwnd, screenPoint);
                return 0;
            }
        }
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, StatusSubclassProc, 1);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}


/* ---- the menu bar ----
   Built from the menu definition (src/menus.lua, built into the exe, or the
   user's copy - see load_menu_definition()). This code adds what the
   definition can't know: shortcut text, check marks and enabled states,
   the generated lists, and the commands plugins place by label.

   A plugin command's label is a '/'-separated path. "Text/Reverse Lines"
   goes on a Text submenu of the definition's plugins list. A leading '/'
   places it relative to the menu bar instead: "/Extras/Insert Date" adds
   a top-level Extras menu, and "/Tools/Word Count" adds to the existing
   Tools menu (after a separator). menu.separator(path) entries name only
   menus, so "/Tools" is the Tools menu itself, and they're placed in order
   among the commands. */

/* Must match PLUGIN_MAX_CONTEXT_DEPTH (plugin_host.h) - editor_input.inc's
   append_plugin_context_menu_items() sizes its parts[] array from that
   constant and passes it to split_plugin_label() here. */
#define PLUGIN_LABEL_MAX_PARTS PLUGIN_MAX_CONTEXT_DEPTH
#define MENU_BAR_MAX_MENUS 24

typedef struct {
    const WCHAR *name;
    HMENU menu;
    int separated; /* whether plugin items placed directly in it follow a separator */
} MenuBarEntry;

/* The menu bar's items are owner-drawn from these strings, so they must
   outlive the menu that uses them. build_menu() alternates between the two
   sets: by the time a set is reused, refresh_menu() has destroyed the menu
   built from it. */
static WCHAR *g_menuBarLabels[2][MENU_BAR_MAX_MENUS];
static int g_menuBarGeneration = 0;

/* An owner-drawn menu-bar label for name, kept until the menu bar built
   alongside it is destroyed; NULL when there's no room or memory. */
static const WCHAR *keep_menu_bar_label(const WCHAR *name) {
    WCHAR **labels = g_menuBarLabels[g_menuBarGeneration];
    int slot = 0;
    while (slot < MENU_BAR_MAX_MENUS && labels[slot]) slot++;
    if (slot >= MENU_BAR_MAX_MENUS) return NULL;
    labels[slot] = wcsdup_heap(name);
    return labels[slot];
}

static int menu_item_is_separator(HMENU menu, int position) {
    MENUITEMINFOW info = {0};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_FTYPE;
    return GetMenuItemInfoW(menu, (UINT)position, TRUE, &info) && (info.fType & MFT_SEPARATOR);
}

/* Separators only ever go between items, never two together. */
static void append_separator(HMENU menu) {
    int count = GetMenuItemCount(menu);
    if (count > 0 && !menu_item_is_separator(menu, count - 1)) AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
}

static void trim_trailing_separator(HMENU menu) {
    int count = GetMenuItemCount(menu);
    if (count > 0 && menu_item_is_separator(menu, count - 1)) DeleteMenu(menu, (UINT)(count - 1), MF_BYPOSITION);
}

/* The same for menu and every submenu in it: a plugin's separator can be
   the last thing put in a menu. */
static void trim_trailing_separators(HMENU menu) {
    trim_trailing_separator(menu);
    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; i++) {
        HMENU submenu = GetSubMenu(menu, i);
        if (submenu) trim_trailing_separators(submenu);
    }
}

/* A definition label as shown: "{codepage}" filled in, and the shortcut
   (if any) after a tab. */
static WCHAR *menu_item_text(const char *label, const char *shortcut) {
    WCHAR *wide = to_wide(label);
    if (!wide) return NULL;
    WCHAR codePage[16] = L"";
    const WCHAR *marker = wcsstr(wide, L"{codepage}");
    if (marker) swprintf(codePage, 16, L"%u", text_ansi_code_page());
    size_t length = wcslen(wide) + wcslen(codePage) + (shortcut ? strlen(shortcut) + 1 : 0) + 1;
    WCHAR *text = (WCHAR *)malloc(length * sizeof(WCHAR));
    if (text) {
        if (marker) {
            size_t before = (size_t)(marker - wide);
            wcsncpy(text, wide, before);
            text[before] = 0;
            wcscat(text, codePage);
            wcscat(text, marker + wcslen(L"{codepage}"));
        } else {
            wcscpy(text, wide);
        }
        if (shortcut) {
            wcscat(text, L"\t");
            size_t used = wcslen(text);
            /* Shortcut text is ASCII. */
            for (const char *c = shortcut; *c; c++) text[used++] = (WCHAR)(unsigned char)*c;
            text[used] = 0;
        }
    }
    free(wide);
    return text;
}

/* Recent Files and Recent Workspaces: numbered entries and a Clear item. */
static void append_recent_list(HMENU menu, WCHAR **paths, int count, UINT firstId, UINT clearId,
                               const char *emptyLabel, const char *clearLabel) {
    if (count == 0) {
        AppendMenuA(menu, MF_STRING | MF_GRAYED, 0, emptyLabel);
        return;
    }
    for (int i = 0; i < count; i++) {
        size_t cap = wcslen(paths[i]) * 2 + 16;
        WCHAR *escaped = (WCHAR *)malloc(cap * sizeof(WCHAR));
        WCHAR *label = (WCHAR *)malloc((cap + 16) * sizeof(WCHAR));
        if (escaped && label) {
            escape_ampersands(paths[i], escaped, cap);
            swprintf(label, cap + 16, L"&%d %ls", i + 1, escaped);
            AppendMenuW(menu, MF_STRING, firstId + (UINT)i, label);
        }
        free(escaped);
        free(label);
    }
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, clearId, clearLabel);
}

/* Splits label in place into its non-empty '/'-separated parts. */
static int split_plugin_label(WCHAR *label, WCHAR **parts) {
    int count = 0;
    for (WCHAR *part = label; part && count < PLUGIN_LABEL_MAX_PARTS;) {
        WCHAR *slash = wcschr(part, L'/');
        if (slash) *slash = 0;
        if (*part) parts[count++] = part;
        part = slash ? slash + 1 : NULL;
    }
    return count;
}

/* Compares menu names, ignoring case and '&' mnemonic markers. */
static int menu_names_match(const WCHAR *a, const WCHAR *b) {
    WCHAR plainA[128], plainB[128];
    const int maxLength = (int)(sizeof(plainA) / sizeof(plainA[0])) - 1;
    int lengthA = 0, lengthB = 0;
    for (; *a && lengthA < maxLength; a++) if (*a != L'&') plainA[lengthA++] = *a;
    for (; *b && lengthB < maxLength; b++) if (*b != L'&') plainB[lengthB++] = *b;
    plainA[lengthA] = 0;
    plainB[lengthB] = 0;
    return lstrcmpiW(plainA, plainB) == 0;
}

/* Plugin items placed directly in a definition menu follow a separator,
   added just before the first of them (not for items in its submenus). */
static void separate_plugin_items(MenuBarEntry *entry) {
    if (!entry || entry->separated) return;
    append_separator(entry->menu);
    entry->separated = 1;
}

/* The submenu of parent called name, or NULL if there's none. */
static HMENU find_plugin_submenu(HMENU parent, const WCHAR *name) {
    int count = GetMenuItemCount(parent);
    for (int i = 0; i < count; i++) {
        WCHAR text[128] = L"";
        MENUITEMINFOW info = {0};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_SUBMENU | MIIM_STRING;
        info.dwTypeData = text;
        info.cch = sizeof(text) / sizeof(text[0]);
        if (GetMenuItemInfoW(parent, (UINT)i, TRUE, &info) && info.hSubMenu && menu_names_match(text, name))
            return info.hSubMenu;
    }
    return NULL;
}

/* The submenu of parent called name, created at the end if there's none.
   entry is parent's menu-bar entry when parent is a menu-bar menu, else NULL. */
static HMENU plugin_submenu(HMENU parent, const WCHAR *name, MenuBarEntry *entry) {
    HMENU existing = find_plugin_submenu(parent, name);
    if (existing) return existing;
    HMENU submenu = CreatePopupMenu();
    if (!submenu) return NULL;
    separate_plugin_items(entry);
    if (!AppendMenuW(parent, MF_POPUP, (UINT_PTR)submenu, name)) {
        DestroyMenu(submenu);
        submenu = NULL;
    }
    return submenu;
}

/* Puts plugin command `command` at parts[first..] under menu. */
static void place_plugin_command(HMENU menu, WCHAR **parts, int first, int partCount, MenuBarEntry *entry,
                                 int command) {
    for (int part = first; menu && part < partCount - 1; part++)
        menu = plugin_submenu(menu, parts[part], part == first ? entry : NULL);
    if (!menu) return;
    if (entry && menu == entry->menu) separate_plugin_items(entry);
    char *label = to_utf8(parts[partCount - 1]);
    WCHAR *text = label ? menu_item_text(label, plugin_host_command_shortcut(command)) : NULL;
    AppendMenuW(menu, MF_STRING, IDM_PLUGIN_COMMAND_0 + command,
                text ? text : parts[partCount - 1]);
    free(text);
    free(label);
}

/* Puts a plugin separator (menu.separator) at the end of the submenu path
   parts[first..] under menu. Unlike a command, it creates no submenus: one
   that doesn't exist yet has nothing in it to separate. */
static void place_plugin_separator(HMENU menu, WCHAR **parts, int first, int partCount, MenuBarEntry *entry) {
    for (int part = first; menu && part < partCount; part++) menu = find_plugin_submenu(menu, parts[part]);
    if (!menu) return;
    if (entry && menu == entry->menu) separate_plugin_items(entry);
    append_separator(menu);
}

/* The "plugins" list: commands plugins added without a leading '/', and
   those labelled "/Name" alone, which name no menu to go on (the same rule
   add_plugin_menu_items() applies without a list). Separators go here
   unless their path starts with '/'; an empty path is the list itself. */
static void append_plugin_list(HMENU menu) {
    int count = plugin_host_command_count();
    /* Two passes, so a command placed directly here lands above one that
       opens a submenu, matching the convention add_plugin_menu_items()
       applies to menu-bar menus. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count; i++) {
            if (plugin_host_command_is_placed(i)) continue;
            WCHAR *label = to_wide(plugin_host_command_label(i));
            if (!label) continue;
            WCHAR *parts[PLUGIN_LABEL_MAX_PARTS];
            int fromMenuBar = label[0] == L'/';
            int partCount = split_plugin_label(label, parts);
            int isSeparator = plugin_host_command_is_separator(i);
            int placedHere = isSeparator ? !(fromMenuBar && partCount > 0)
                                          : (partCount && !(fromMenuBar && partCount > 1));
            int opensSubmenu = placedHere && !isSeparator && partCount > 1;
            if (placedHere && opensSubmenu == (pass == 1)) {
                if (isSeparator) place_plugin_separator(menu, parts, 0, partCount, NULL);
                else place_plugin_command(menu, parts, 0, partCount, NULL, i);
            }
            free(label);
        }
    }
}

/* Fills menu from definition items; *pluginsListed is set when the
   "plugins" list is among them. */
static void append_definition_items(HMENU menu, const PluginMenuItem *items, int count, int *pluginsListed) {
    for (int i = 0; i < count; i++) {
        const PluginMenuItem *item = &items[i];
        switch (item->kind) {
            case PLUGIN_MENU_SEPARATOR:
                append_separator(menu);
                break;
            case PLUGIN_MENU_COMMAND: {
                const MenuCommand *command = menu_command_find(item->name);
                WCHAR *text = command ? menu_item_text(item->label, command->shortcut) : NULL;
                if (text) AppendMenuW(menu, MF_STRING, command->id, text);
                free(text);
                break;
            }
            case PLUGIN_MENU_FUNCTION: {
                WCHAR *text = menu_item_text(item->label, plugin_host_command_shortcut(item->command));
                if (text) AppendMenuW(menu, MF_STRING, IDM_PLUGIN_COMMAND_0 + (UINT)item->command, text);
                free(text);
                break;
            }
            case PLUGIN_MENU_SUBMENU: {
                HMENU submenu = CreatePopupMenu();
                WCHAR *text = submenu ? menu_item_text(item->label, NULL) : NULL;
                if (text) {
                    append_definition_items(submenu, item->items, item->itemCount, pluginsListed);
                    AppendMenuW(menu, MF_POPUP, (UINT_PTR)submenu, text);
                } else if (submenu) {
                    DestroyMenu(submenu);
                }
                free(text);
                break;
            }
            case PLUGIN_MENU_LIST:
                if (strcmp(item->name, "recent_files") == 0) {
                    append_recent_list(menu, g_recentFiles, g_recentFileCount, IDM_FILE_RECENT_0,
                                       IDM_FILE_RECENT_CLEAR, "(no recent files)", "&Clear Recent Files");
                } else if (strcmp(item->name, "recent_workspaces") == 0) {
                    append_recent_list(menu, g_recentFolders, g_recentFolderCount, IDM_RECENT_WORKSPACE,
                                       IDM_RECENT_WORKSPACE_CLEAR, "(no recent workspaces)",
                                       "&Clear Recent Workspaces");
                } else if (strcmp(item->name, "plugins") == 0) {
                    append_plugin_list(menu);
                    *pluginsListed = 1;
                }
                break;
        }
    }
    trim_trailing_separator(menu);
}

/* Adds a menu to the bar. One plugins created goes before Help, when there
   is one, and needs no separator before plugin items. */
static MenuBarEntry *add_menu_bar_menu(HMENU menuBar, MenuBarEntry *entries, int *entryCount,
                                       const WCHAR *name, HMENU menu, int createdByPlugins) {
    const WCHAR *label = *entryCount < MENU_BAR_MAX_MENUS ? keep_menu_bar_label(name) : NULL;
    if (!label) return NULL;
    UINT position = (UINT)-1;
    for (int i = 0; createdByPlugins && i < *entryCount; i++) {
        if (!menu_names_match(entries[i].name, L"Help")) continue;
        for (int p = GetMenuItemCount(menuBar) - 1; p >= 0; p--)
            if (GetSubMenu(menuBar, p) == entries[i].menu) position = (UINT)p;
    }
    InsertMenuW(menuBar, position, MF_BYPOSITION | MF_POPUP | MF_OWNERDRAW, (UINT_PTR)menu, label);
    entries[*entryCount] = (MenuBarEntry){ name, menu, createdByPlugins };
    return &entries[(*entryCount)++];
}

/* The menu-bar entry called name, or NULL if there's none. */
static MenuBarEntry *find_menu_bar_entry(MenuBarEntry *entries, int entryCount, const WCHAR *name) {
    for (int i = 0; i < entryCount; i++)
        if (menu_names_match(entries[i].name, name)) return &entries[i];
    return NULL;
}

/* The menu-bar entry called name, with a new menu added if there's none. */
static MenuBarEntry *plugin_top_menu(HMENU menuBar, MenuBarEntry *entries, int *entryCount, const WCHAR *name) {
    MenuBarEntry *existing = find_menu_bar_entry(entries, *entryCount, name);
    if (existing) return existing;
    HMENU menu = CreatePopupMenu();
    MenuBarEntry *entry = menu ? add_menu_bar_menu(menuBar, entries, entryCount, name, menu, 1) : NULL;
    if (menu && !entry) DestroyMenu(menu);
    return entry;
}

/* Places the plugin commands the definition and its plugins list haven't:
   those labelled from the menu bar ("/Tools/..."), and, when the definition
   has no plugins list, the rest, on a Plugins menu of their own. */
static void add_plugin_menu_items(HMENU menuBar, MenuBarEntry *entries, int *entryCount, int pluginsListed) {
    /* Names of created top-level menus point into these until the end. */
    WCHAR *copies[PLUGIN_MAX_COMMANDS] = {0};
    static WCHAR *parts[PLUGIN_MAX_COMMANDS][PLUGIN_LABEL_MAX_PARTS];
    int partCount[PLUGIN_MAX_COMMANDS] = {0};
    int fromMenuBar[PLUGIN_MAX_COMMANDS] = {0};
    int count = plugin_host_command_count();
    for (int i = 0; i < count; i++) {
        if (plugin_host_command_is_placed(i)) continue;
        WCHAR *label = copies[i] = to_wide(plugin_host_command_label(i));
        if (!label) continue;
        fromMenuBar[i] = label[0] == L'/';
        partCount[i] = split_plugin_label(label, parts[i]);
    }
    /* Two passes, so a plugin command placed directly in a menu lands above
       one that opens a submenu there, matching the convention menus.lua
       follows: single items first, items with submenus at the bottom. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count; i++) {
            if (!copies[i]) continue;
            int isSeparator = plugin_host_command_is_separator(i);
            int opensSubmenu = !isSeparator && fromMenuBar[i] && partCount[i] > 2;
            if (opensSubmenu != (pass == 1)) continue;
            if (isSeparator) {
                /* A separator's path names only menus: "/Snippets" is the
                   Snippets menu itself. Menus it names must exist already. */
                if (fromMenuBar[i] && partCount[i] > 0) {
                    MenuBarEntry *entry = find_menu_bar_entry(entries, *entryCount, parts[i][0]);
                    if (entry) place_plugin_separator(entry->menu, parts[i], 1, partCount[i], entry);
                } else if (!pluginsListed) {
                    MenuBarEntry *entry = find_menu_bar_entry(entries, *entryCount, L"&Plugins");
                    if (entry) place_plugin_separator(entry->menu, parts[i], 0, partCount[i], NULL);
                }
            /* "/Name" alone has no menu to go on, so it goes with the others. */
            } else if (fromMenuBar[i] && partCount[i] > 1) {
                MenuBarEntry *entry = plugin_top_menu(menuBar, entries, entryCount, parts[i][0]);
                if (entry) place_plugin_command(entry->menu, parts[i], 1, partCount[i], entry, i);
            } else if (partCount[i] && !pluginsListed) {
                MenuBarEntry *entry = plugin_top_menu(menuBar, entries, entryCount, L"&Plugins");
                if (entry) place_plugin_command(entry->menu, parts[i], 0, partCount[i], NULL, i);
            }
        }
    }
    for (int i = 0; i < count; i++) free(copies[i]);
}

typedef struct {
    int enabled;
    int checked;
    int radio;
} CommandState;

/* The check mark and enabled state of a built-in command that has them;
   zero for the rest. */
static int command_state(UINT id, CommandState *state) {
    *state = (CommandState){ 1, 0, 0 };
    if (id >= IDM_PLUGIN_COMMAND_0 && id < IDM_PLUGIN_COMMAND_0 + PLUGIN_MAX_COMMANDS) {
        int index = (int)(id - IDM_PLUGIN_COMMAND_0);
        int stateKnown = 0;
        if (plugin_host_command_is_checkable(index)) {
            state->checked = plugin_host_command_is_checked(index);
            stateKnown = 1;
        }
        if (plugin_host_command_has_enabled(index)) {
            HWND editor = active_edit_view();
            if (editor) {
                int oldPluginDocument = g_pluginCommandDocument;
                g_pluginCommandDocument = document_index_for_view(editor);
                char *error = plugin_host_command_enabled(index, editor, &state->enabled);
                g_pluginCommandDocument = oldPluginDocument;
                if (error) show_plugin_error(error);
                free(error);
            }
            stateKnown = 1;
        }
        return stateKnown;
    }
#define STATE_CHECK(value) (state->checked = (value) != 0)
#define STATE_RADIO(value) (state->radio = 1, state->checked = (value) != 0)
    switch (id) {
        case IDM_FILE_SAVE: case IDM_FILE_SAVEAS:
        case IDM_FILE_INSERT:
            state->enabled = !active_document_is_image(); break;
        case IDM_FILE_REOPEN_UTF8: case IDM_FILE_REOPEN_UTF16LE:
        case IDM_FILE_REOPEN_UTF16BE: case IDM_FILE_REOPEN_ANSI:
            state->enabled = g_currentFile != NULL && !active_document_is_image(); break;
        case IDM_FILE_REOPEN_CLOSED: state->enabled = g_closedDocumentCount != 0; break;
        case IDM_FILE_GO_BACK: state->enabled = g_wikiBackCount != 0; break;
        case IDM_FILE_CLOSE:
            state->enabled = !g_documentStore.documents[g_documentStore.active].pinned; break;
        case IDM_VIEW_PIN_TAB:
            STATE_CHECK(g_documentStore.documents[g_documentStore.active].pinned); break;
        case IDM_MARKDOWN_BACKLINKS:
            state->enabled = g_currentFile && g_activeLanguage == LANG_MARKDOWN &&
                             !active_document_is_image(); break;
        case IDM_VIEW_TOGGLEPREVIEW: STATE_CHECK(g_previewVisible); break;
        case IDM_VIEW_TOGGLEEDITOR: STATE_CHECK(g_editorVisible); break;
        case IDM_VIEW_THEME_GITHUB_LIGHT: STATE_RADIO(!g_editorDarkMode); break;
        case IDM_VIEW_THEME_GITHUB_DARK: STATE_RADIO(g_editorDarkMode); break;
        case IDM_VIEW_PREVIEW_THEME_LIGHT: STATE_RADIO(!g_previewDarkMode); break;
        case IDM_VIEW_PREVIEW_THEME_DARK: STATE_RADIO(g_previewDarkMode); break;
        case IDM_VIEW_CLEAR_CUSTOM_CSS: state->enabled = g_customCssPath != NULL; break;
        case IDM_VIEW_PREVIEW_ON_LEFT: STATE_CHECK(g_previewOnLeft); break;
        case IDM_VIEW_FOCUS_MODE: STATE_CHECK(g_focusMode); break;
        case IDM_VIEW_WORD_WRAP: STATE_CHECK(g_wordWrap); break;
        case IDM_VIEW_SPELL_CHECK: STATE_CHECK(g_spellCheckEnabled); state->enabled = g_spellChecker != NULL; break;
        case IDM_VIEW_OUTLINE: STATE_CHECK(g_outlineVisible); break;
        case IDM_VIEW_EXPLORER: STATE_CHECK(g_explorerVisible); break;
        case IDM_VIEW_EXPLORER_ON_RIGHT: STATE_CHECK(g_explorerOnRight); break;
        case IDM_VIEW_GIT: STATE_CHECK(g_gitPanelVisible); break;
        case IDM_VIEW_GIT_ON_RIGHT: STATE_CHECK(g_gitPanelOnRight); break;
        case IDM_VIEW_OUTPUT_PANEL: STATE_CHECK(g_outputPanelVisible); break;
        case IDM_VIEW_OUTPUT_PANEL_ON_RIGHT: STATE_CHECK(g_outputPanelOnRight); break;
        case IDM_RUN_USE_CONSOLE: STATE_CHECK(g_runInConsole); break;
        case IDM_VIEW_MINIMAP: STATE_CHECK(g_minimapVisible); break;
        case IDM_VIEW_TOGGLESPLIT: STATE_CHECK(g_splitEditorVisible && !g_splitEditorVertical); break;
        case IDM_VIEW_SPLIT_VERTICAL: STATE_CHECK(g_splitEditorVisible && g_splitEditorVertical); break;
        case IDM_VIEW_RESET_SPLIT_EDITOR: state->enabled = g_splitEditorVisible != 0; break;
        case IDM_VIEW_SPLIT_DOCUMENT: {
            state->enabled = 0;
            for (int i = 0; i < g_documentStore.count; i++) {
                const Document *doc = &g_documentStore.documents[i];
                if (doc->viewKind == DOCUMENT_VIEW_TEXT && !doc->largeFile) {
                    state->enabled = 1;
                    break;
                }
            }
            break;
        }
        case IDM_VIEW_SHOW_WHITESPACE: STATE_CHECK(g_showWhitespace); break;
        case IDM_VIEW_INDENT_GUIDES: STATE_CHECK(g_indentGuides); break;
        case IDM_VIEW_CHANGED_LINES: STATE_CHECK(g_markChangedLines); break;
        case IDM_VIEW_USE_SPACES_INDENT: STATE_CHECK(g_indentUseSpaces); break;
        default: return 0;
    }
#undef STATE_CHECK
#undef STATE_RADIO
    return 1;
}

/* Sets each built-in command's check mark and enabled state, wherever the
   definition put it. A submenu holding only unavailable built-in commands
   is greyed too (Reopen with Encoding, with no file). Returns whether menu
   should be greyed by that rule. */
static int apply_menu_states(HMENU menu) {
    int count = GetMenuItemCount(menu);
    int anyEnabled = 0, anyDisabledCommand = 0;
    for (int i = 0; i < count; i++) {
        MENUITEMINFOW info = {0};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_ID | MIIM_SUBMENU | MIIM_FTYPE | MIIM_STATE;
        if (!GetMenuItemInfoW(menu, (UINT)i, TRUE, &info) || (info.fType & MFT_SEPARATOR)) continue;
        CommandState state;
        if (info.hSubMenu) {
            int grey = apply_menu_states(info.hSubMenu);
            info.fMask = MIIM_STATE;
            info.fState = grey ? (info.fState | MFS_GRAYED) : (info.fState & ~(UINT)MFS_GRAYED);
            SetMenuItemInfoW(menu, (UINT)i, TRUE, &info);
            if (grey) anyDisabledCommand = 1;
            else anyEnabled = 1;
        } else if (command_state(info.wID, &state)) {
            info.fMask = MIIM_STATE | MIIM_FTYPE;
            info.fState = (state.enabled ? 0 : MFS_GRAYED) | (state.checked ? MFS_CHECKED : 0);
            info.fType = state.radio ? (info.fType | MFT_RADIOCHECK) : (info.fType & ~(UINT)MFT_RADIOCHECK);
            SetMenuItemInfoW(menu, (UINT)i, TRUE, &info);
            if (state.enabled) anyEnabled = 1;
            else anyDisabledCommand = 1;
        } else if (!(info.fState & MFS_GRAYED)) {
            anyEnabled = 1;
        }
    }
    return !anyEnabled && anyDisabledCommand;
}

static HMENU build_menu(void) {
    g_menuBarGeneration ^= 1;
    for (int i = 0; i < MENU_BAR_MAX_MENUS; i++) {
        free(g_menuBarLabels[g_menuBarGeneration][i]);
        g_menuBarLabels[g_menuBarGeneration][i] = NULL;
    }
    HMENU menuBar = CreateMenu();
    MenuBarEntry entries[MENU_BAR_MAX_MENUS];
    WCHAR *names[MENU_BAR_MAX_MENUS] = {0};
    int entryCount = 0, pluginsListed = 0;

    int count, nameCount = 0;
    const PluginMenuItem *menus = plugin_host_menu(&count);
    for (int i = 0; i < count && nameCount < MENU_BAR_MAX_MENUS; i++) {
        WCHAR *name = names[nameCount++] = to_wide(menus[i].label);
        HMENU menu = name ? CreatePopupMenu() : NULL;
        if (!menu) continue;
        append_definition_items(menu, menus[i].items, menus[i].itemCount, &pluginsListed);
        if (!add_menu_bar_menu(menuBar, entries, &entryCount, name, menu, 0)) DestroyMenu(menu);
    }
    /* Without any definition (it failed to load), just enough to fix it. */
    if (count == 0) {
        HMENU menu = CreatePopupMenu();
        if (menu) {
            append_plugin_list(menu);
            pluginsListed = 1;
            append_separator(menu);
            AppendMenuA(menu, MF_STRING, IDM_PLUGIN_RELOAD, "&Reload Plugins");
            AppendMenuA(menu, MF_STRING, IDM_PLUGIN_NEW, "&New Plugin...");
            AppendMenuA(menu, MF_STRING, IDM_PLUGIN_OPEN_FOLDER, "Open Plugins &Folder");
            AppendMenuA(menu, MF_STRING, IDM_PLUGIN_MANAGE, "&Manage Plugins...");
            AppendMenuA(menu, MF_STRING, IDM_PLUGIN_CUSTOMIZE_MENUS, "&Customize Menus...");
            AppendMenuA(menu, MF_STRING, IDM_PLUGIN_CUSTOMIZE_THEME, "Customize &Theme Colours...");
            if (!add_menu_bar_menu(menuBar, entries, &entryCount, L"&Plugins", menu, 0)) DestroyMenu(menu);
        }
    }
    add_plugin_menu_items(menuBar, entries, &entryCount, pluginsListed);
    trim_trailing_separators(menuBar);
    apply_menu_states(menuBar);
    for (int i = 0; i < MENU_BAR_MAX_MENUS; i++) free(names[i]);

    set_menu_background(menuBar);
    return menuBar;
}

/* Rebuild and install the menu bar (used after the recent-files list changes,
   since Win32 menus aren't backed by a live model - the HMENU has to be
   replaced whenever its contents differ). */
static void refresh_menu(HWND hwnd) {
    HMENU oldMenu = GetMenu(hwnd);
    if (g_focusMode) {
        if (oldMenu) {
            SetMenu(hwnd, NULL);
            DestroyMenu(oldMenu);
        }
        DrawMenuBar(hwnd);
        return;
    }
    SetMenu(hwnd, build_menu());
    if (oldMenu) DestroyMenu(oldMenu);
    DrawMenuBar(hwnd);
}

static int g_dragging = 0;

static void set_webview_bounds(RECT wvRc, BOOL visible) {
    if (!g_webviewController) return;
    ICoreWebView2Controller_put_Bounds(g_webviewController, wvRc);
    ICoreWebView2Controller_put_IsVisible(g_webviewController, visible);
}

/* ---- formatting toolbar: a strip of owner-drawn BUTTON children of the
   main window, reserved space in layout() above the editor/preview panes.
   The icon painters themselves live in toolbar_icons.c/.h; a NULL paint
   function here marks a separator (a small gap, no button). ---- */
struct ToolbarButtonDef {
    WORD id;
    ToolbarIconPaintFn paint; /* NULL marks a separator (a small gap, no button) */
    const char *tip;
};
static const struct ToolbarButtonDef TOOLBAR_BUTTONS[] = {
    { IDM_FMT_BOLD, toolbar_icon_bold, "Bold" },
    { IDM_FMT_ITALIC, toolbar_icon_italic, "Italic" },
    { IDM_FMT_STRIKETHROUGH, toolbar_icon_strikethrough, "Strikethrough" },
    { IDM_FMT_CODE, toolbar_icon_code, "Inline code" },
    { 0, NULL, NULL },
    { IDM_FMT_H1, toolbar_icon_h1, "Heading 1" },
    { IDM_FMT_H2, toolbar_icon_h2, "Heading 2" },
    { IDM_FMT_H3, toolbar_icon_h3, "Heading 3" },
    { IDM_FMT_QUOTE, toolbar_icon_quote, "Blockquote" },
    { 0, NULL, NULL },
    { IDM_FMT_BULLETLIST, toolbar_icon_bullet_list, "Bullet list" },
    { IDM_FMT_NUMBERLIST, toolbar_icon_number_list, "Numbered list" },
    { IDM_FMT_TASKLIST, toolbar_icon_task_list, "Task list" },
    { 0, NULL, NULL },
    { IDM_FMT_LINK, toolbar_icon_link, "Insert link" },
    { IDM_FMT_IMAGE, toolbar_icon_image, "Insert image" },
    { IDM_FMT_TABLE, toolbar_icon_table, "Insert table" },
    { IDM_FMT_CODEBLOCK, toolbar_icon_codeblock, "Insert code block" },
    { IDM_FMT_FOOTNOTE, toolbar_icon_footnote, "Insert footnote" },
    { IDM_FMT_HRULE, toolbar_icon_hrule, "Insert horizontal rule" },
    { IDM_FMT_ALERT, toolbar_icon_alert, "Insert alert" },
    { 0, NULL, NULL },
    { IDM_VIEW_TOGGLEPREVIEW, toolbar_icon_togglepreview, "Toggle preview" },
    { IDM_VIEW_PREVIEW_ON_LEFT, toolbar_icon_swappanels, "Swap panel sides" },
    { IDM_VIEW_TOGGLESPLIT, toolbar_icon_splitview, "Toggle horizontal editor split" },
    { 0, NULL, NULL },
    { IDM_FILE_OPEN_TERMINAL, toolbar_icon_terminal, "Open terminal here" },
};
#define TOOLBAR_BUTTON_COUNT (int)(sizeof(TOOLBAR_BUTTONS) / sizeof(TOOLBAR_BUTTONS[0]))
#define TOOLBAR_BUTTON_SIZE (TOOLBAR_HEIGHT - 6)
static HWND g_hToolbarButtons[TOOLBAR_BUTTON_COUNT];
static HWND g_hToolbarTooltip;

/* Recolors the toolbar buttons' shared tooltip to match the current app
   theme (same GitHub light/dark palette as theme_bg_color()/theme_fg_color()).
   Visual styles are switched off for this control in create_toolbar() below,
   since themed tooltips ignore TTM_SETTIPBKCOLOR/TTM_SETTIPTEXTCOLOR and
   would otherwise stay the OS's light tooltip colors even in dark mode. */
static void apply_toolbar_tooltip_theme(void) {
    if (!g_hToolbarTooltip) return;
    SendMessageW(g_hToolbarTooltip, TTM_SETTIPBKCOLOR, (WPARAM)theme_bg_color(), 0);
    SendMessageW(g_hToolbarTooltip, TTM_SETTIPTEXTCOLOR, (WPARAM)theme_fg_color(), 0);
}

static void create_toolbar(HWND hwnd) {
    g_hToolbarTooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hwnd, NULL, GetModuleHandleA(NULL), NULL);
    if (g_hToolbarTooltip) {
        SetWindowPos(g_hToolbarTooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowTheme(g_hToolbarTooltip, L"", L"");
        apply_toolbar_tooltip_theme();
    }

    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
        if (!TOOLBAR_BUTTONS[i].paint) continue;
        WCHAR *tip = to_wide(TOOLBAR_BUTTONS[i].tip);
        g_hToolbarButtons[i] = CreateWindowExW(0, L"BUTTON", tip ? tip : L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)TOOLBAR_BUTTONS[i].id, GetModuleHandleA(NULL), NULL);
        /* WS_CLIPSIBLINGS: these buttons sit edge-to-edge and get MoveWindow'd
           on every layout() (including mid-drag during a live resize, where
           layout_toolbar_at()'s row-wrap can shift several at once) - without it,
           one button's owner-draw paint can bleed into a neighbour's area.
           The window text isn't displayed (owner-drawn), but is kept as the
           accessible name / tooltip-eligible text for the button. */
        if (g_hToolbarTooltip && tip) {
            TOOLINFOW ti = { 0 };
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = hwnd;
            ti.uId = (UINT_PTR)g_hToolbarButtons[i];
            ti.hinst = GetModuleHandleA(NULL);
            ti.lpszText = tip;
            SendMessageW(g_hToolbarTooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
        free(tip);
    }
}

/* Paint one owner-drawn toolbar button: a flat/pressed frame around the
   button's icon, drawn via the icon's paint function. Called from
   WM_DRAWITEM in WndProc. Windows reports ODS_SELECTED automatically for
   owner-draw buttons while the mouse is held down over them; true hover
   highlighting would need per-button subclassing to track WM_MOUSEMOVE/
   WM_MOUSELEAVE, which isn't worth the complexity for this toolbar. */
static void draw_toolbar_button(const DRAWITEMSTRUCT *dis) {
    int index = -1;
    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
        if (g_hToolbarButtons[i] == dis->hwndItem) { index = i; break; }
    }
    if (index < 0 || !TOOLBAR_BUTTONS[index].paint) return;

    COLORREF fg = theme_fg_color();
    COLORREF pressedBg = theme_palette()->pressedBg;

    int pressed = (dis->itemState & ODS_SELECTED) != 0;
    fill_rect_color(dis->hDC, &dis->rcItem, pressed ? pressedBg : theme_bg_color());

    if (dis->itemState & ODS_FOCUS) {
        HPEN focusPen = CreatePen(PS_SOLID, 1, fg);
        HPEN oldPen = (HPEN)SelectObject(dis->hDC, focusPen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
        Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
        SelectObject(dis->hDC, oldPen);
        SelectObject(dis->hDC, oldBrush);
        DeleteObject(focusPen);
    }

    RECT iconRc = dis->rcItem;
    InflateRect(&iconRc, -2, -2);
    TOOLBAR_BUTTONS[index].paint(dis->hDC, &iconRc, fg);
}

/* Lay the toolbar buttons out left-to-right along the reserved top strip,
   with a wider gap standing in for a separator between groups. All buttons
   are the same size, sized to the toolbar's height. */
static int layout_toolbar_at(int width, int top) {
    int x = 4, y = top + 3;
    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
        if (!TOOLBAR_BUTTONS[i].paint) {
            if (x > 4) x += 10;
            continue;
        }
        HWND hBtn = g_hToolbarButtons[i];
        if (!hBtn) continue;
        if (x > 4 && x + TOOLBAR_BUTTON_SIZE > width - 4) {
            x = 4;
            y += TOOLBAR_BUTTON_SIZE + 4;
        }
        MoveWindow(hBtn, x, y, TOOLBAR_BUTTON_SIZE, TOOLBAR_BUTTON_SIZE, TRUE);
        x += TOOLBAR_BUTTON_SIZE + 4;
    }
    return y + TOOLBAR_BUTTON_SIZE + 3;
}

/* Positions the editor within [x, x+width) x [y, y+h), carving a strip off
   its right edge for the minimap when one should be shown - regardless of
   which side of the splitter the editor itself is on, so the minimap always
   sits immediately next to the text it overviews. */
static int split_editor_position(int extent, int splitterSize) {
    int available = extent - splitterSize;
    if (available < 0) available = 0;
    if (g_splitEditorPos < 0)
        g_splitEditorPos = MulDiv(available, g_splitEditorRatio, 1000);

    int minimum = MulDiv(available, 50, 1000);
    if (minimum < 60) minimum = 60;
    if (minimum * 2 > available) minimum = available / 2;
    if (g_splitEditorPos < minimum) g_splitEditorPos = minimum;
    if (g_splitEditorPos > available - minimum) g_splitEditorPos = available - minimum;
    if (available > 0) g_splitEditorRatio = MulDiv(g_splitEditorPos, 1000, available);
    return g_splitEditorPos;
}

static void layout_split_editor(HWND hwnd, int x, int y, int width, int height) {
    int splitterSize = MulDiv(SPLIT_EDITOR_SPLITTER_SIZE, GetDpiForWindow(hwnd), 96);
    if (splitterSize < 1) splitterSize = 1;
    int extent = g_splitEditorVertical ? width : height;
    int mainSize = split_editor_position(extent, splitterSize);
    int secondSize = extent - mainSize - splitterSize;

    if (g_splitEditorVertical) {
        g_splitEditorSplitterRect = (RECT){x + mainSize, y, x + mainSize + splitterSize, y + height};
        MoveWindow(g_hEdit, x, y, mainSize, height, TRUE);
        MoveWindow(g_hSplitEdit, x + mainSize + splitterSize, y, secondSize, height, TRUE);
    } else {
        g_splitEditorSplitterRect = (RECT){x, y + mainSize, x + width, y + mainSize + splitterSize};
        MoveWindow(g_hEdit, x, y, width, mainSize, TRUE);
        MoveWindow(g_hSplitEdit, x, y + mainSize + splitterSize, width, secondSize, TRUE);
    }
    ShowWindow(g_hEdit, SW_SHOW);
    ShowWindow(g_hSplitEdit, SW_SHOW);
}

static void layout_editor_with_minimap(HWND hwnd, int x, int y, int width, int h) {
    g_splitEditorRegionOrigin = (POINT){x, y};
    int minimapWidth = (g_hMinimap && g_minimapVisible && !g_focusMode) ?
        MulDiv(MINIMAP_WIDTH, GetDpiForWindow(hwnd), 96) : 0;
    if (minimapWidth > width - 80) minimapWidth = 0; /* not enough room to keep the editor usable */

    /* With the preview pane also open, the minimap otherwise butts straight
       up against the editor with nothing to mark the boundary between them -
       leave a thin gap for the theme background to show through as a
       splitter line, matching the wider one between the editor and preview. */
    int minimapGap = (minimapWidth > 0 && g_previewVisible) ?
        MulDiv(MINIMAP_SPLITTER_WIDTH, GetDpiForWindow(hwnd), 96) : 0;
    if (minimapGap < 1 && minimapWidth > 0 && g_previewVisible) minimapGap = 1;

    int editorWidth = width - minimapWidth - minimapGap;

    /* Both editor views occupy the column beside the minimap. The minimap is
       an overview of the shared document, so it spans the full height. */
    if (g_hSplitEdit && g_splitEditorVisible && !g_focusMode) {
        layout_split_editor(hwnd, x, y, editorWidth, h);
    } else {
        g_splitEditorSplitterRect = (RECT){0, 0, 0, 0};
        MoveWindow(g_hEdit, x, y, editorWidth, h, TRUE);
        ShowWindow(g_hEdit, SW_SHOW);
        if (g_hSplitEdit) ShowWindow(g_hSplitEdit, SW_HIDE);
    }

    if (!g_hMinimap) return;
    if (minimapWidth > 0) {
        MoveWindow(g_hMinimap, x + editorWidth + minimapGap, y, minimapWidth, h, TRUE);
        ShowWindow(g_hMinimap, SW_SHOW);
        update_minimap_viewport();
    } else {
        ShowWindow(g_hMinimap, SW_HIDE);
    }
}

/* The part of the main window's client area not yet given to a panel.
   layout() hands it to each panel in turn, and each one takes its share
   from one edge. */
typedef struct LayoutArea {
    int x, y, width, height;
} LayoutArea;

/* The status bar along the bottom edge. */
static void layout_status_bar(HWND hwnd, LayoutArea *area) {
    if (!g_hStatus) return;
    int statusHeight = STATUS_BAR_HEIGHT;
    MoveWindow(g_hStatus, 0, area->height - statusHeight, area->width, statusHeight, TRUE);
    int dpi = area->width > 0 ? GetDpiForWindow(hwnd) : 96;
    int parts[4] = { MulDiv(STATUS_POSITION_PART_RIGHT, dpi, 96), MulDiv(STATUS_COUNT_PART_RIGHT, dpi, 96),
                     MulDiv(STATUS_ENCODING_PART_RIGHT, dpi, 96), -1 };
    SendMessageW(g_hStatus, SB_SETPARTS, 4, (LPARAM)parts);
    /* StatusSubclassProc paints from these part rectangles via SB_GETRECT,
       so a resize needs a repaint to match the new sizes. Resizing can't
       change the text, so the full-document word count only runs the first
       time (before any part text exists), not on every WM_SIZE or
       splitter-drag mouse move. */
    if (g_statusPartText[1][0]) InvalidateRect(g_hStatus, NULL, FALSE);
    else update_status_bar();
    area->height -= statusHeight;
}

/* The formatting toolbar spans the full window width, above both the file
   explorer panel and the editor/preview - the explorer panel is docked
   below it, not beside it. The toolbar sits directly under the menu bar.
   It edits Markdown source, so it's only useful (and only shown) while the
   editor pane is visible. */
static void layout_toolbar(LayoutArea *area) {
    int showToolbar = editor_pane_visible() && !g_focusMode;
    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++)
        if (g_hToolbarButtons[i]) ShowWindow(g_hToolbarButtons[i], showToolbar ? SW_SHOW : SW_HIDE);
    int y = showToolbar ? layout_toolbar_at(area->width, 0) : 0;
    area->y = y;
    area->height -= y;
    if (area->height < 0) area->height = 0;
}

/* The document tab strip, docked above the editor/preview/outline only -
   not above the file explorer, which spans the full height beside it.
   Called after layout_explorer() has carved the explorer out of area. */
static void layout_tabs(LayoutArea *area) {
    ShowWindow(g_hTabs, g_focusMode ? SW_HIDE : SW_SHOW);
    if (g_focusMode) return;
    MoveWindow(g_hTabs, area->x, area->y, area->width, TAB_HEIGHT, TRUE);
    area->y += TAB_HEIGHT;
    area->height -= TAB_HEIGHT;
    if (area->height < 0) area->height = 0;
}

/* File explorer panel: docked at the left or right edge of the remaining
   content area (below the toolbar, above the status bar), per g_explorerOnRight. */
static void layout_explorer(HWND hwnd, LayoutArea *area) {
    if (!g_hExplorerTree || !g_explorerVisible || g_focusMode) {
        g_explorerSplitterRect = (RECT){0, 0, 0, 0};
        if (g_hExplorerTree) ShowWindow(g_hExplorerTree, SW_HIDE);
        return;
    }
    int dpi = GetDpiForWindow(hwnd);
    if (g_explorerWidth < 0)
        g_explorerWidth = MulDiv(area->width, g_explorerWidthRatio, 1000);
    int minWidth = MulDiv(EXPLORER_MIN_WIDTH, dpi, 96);
    int maxWidth = area->width * 2 / 3;
    if (g_explorerWidth < minWidth) g_explorerWidth = minWidth;
    if (maxWidth > minWidth && g_explorerWidth > maxWidth) g_explorerWidth = maxWidth;
    if (g_explorerOnRight) {
        int explorerX = area->x + area->width - g_explorerWidth;
        g_explorerSplitterRect = (RECT){ explorerX - SPLITTER_WIDTH, area->y,
                                         explorerX, area->y + area->height };
        MoveWindow(g_hExplorerTree, explorerX, area->y, g_explorerWidth, area->height, TRUE);
        area->width -= g_explorerWidth + SPLITTER_WIDTH;
    } else {
        g_explorerSplitterRect = (RECT){ area->x + g_explorerWidth, area->y,
                                         area->x + g_explorerWidth + SPLITTER_WIDTH, area->y + area->height };
        MoveWindow(g_hExplorerTree, area->x, area->y, g_explorerWidth, area->height, TRUE);
        area->x += g_explorerWidth + SPLITTER_WIDTH;
        area->width -= g_explorerWidth + SPLITTER_WIDTH;
    }
    if (area->width < 0) area->width = 0;
    ShowWindow(g_hExplorerTree, SW_SHOW);
}

/* The output panel docks under the editor, preview and outline (a splitter
   along its top edge for its height) or, when g_outputPanelOnRight, beside
   them instead (a splitter along its left edge for its width). Either way
   it keeps the size the user dragged it to, within what the window can
   spare. Its Debug tab is where a debug session's status and variables show
   (see output_panel.inc). */
static void layout_output_panel(HWND hwnd, LayoutArea *area) {
    g_outputSplitterRect = (RECT){0, 0, 0, 0};
    if (!g_hOutputPanel) return;
    int dpi = GetDpiForWindow(hwnd);
    if (g_outputPanelOnRight) {
        if (!g_outputPanelVisible || g_focusMode || area->width < OUTPUT_PANEL_MIN_SPACE_X) {
            ShowWindow(g_hOutputPanel, SW_HIDE);
            return;
        }
        int minimum = MulDiv(OUTPUT_PANEL_MIN_WIDTH, dpi, 96);
        int maximum = area->width - minimum - SPLITTER_WIDTH;
        if (g_outputPanelWidth < 0) g_outputPanelWidth = MulDiv(OUTPUT_PANEL_WIDTH, dpi, 96);
        if (g_outputPanelWidth > maximum) g_outputPanelWidth = maximum;
        if (g_outputPanelWidth < minimum) g_outputPanelWidth = minimum;
        int left = area->x + area->width - g_outputPanelWidth;
        MoveWindow(g_hOutputPanel, left, area->y, g_outputPanelWidth, area->height, TRUE);
        ShowWindow(g_hOutputPanel, SW_SHOW);
        g_outputSplitterRect = (RECT){ left - SPLITTER_WIDTH, area->y, left, area->y + area->height };
        area->width -= g_outputPanelWidth + SPLITTER_WIDTH;
        return;
    }
    if (!g_outputPanelVisible || g_focusMode || area->height < OUTPUT_PANEL_MIN_SPACE) {
        ShowWindow(g_hOutputPanel, SW_HIDE);
        return;
    }
    int minimum = MulDiv(OUTPUT_PANEL_MIN_HEIGHT, dpi, 96);
    int maximum = area->height - minimum - SPLITTER_WIDTH;
    if (g_outputPanelHeight < 0) g_outputPanelHeight = MulDiv(OUTPUT_PANEL_HEIGHT, dpi, 96);
    if (g_outputPanelHeight > maximum) g_outputPanelHeight = maximum;
    if (g_outputPanelHeight < minimum) g_outputPanelHeight = minimum;
    int top = area->y + area->height - g_outputPanelHeight;
    MoveWindow(g_hOutputPanel, area->x, top, area->width, g_outputPanelHeight, TRUE);
    ShowWindow(g_hOutputPanel, SW_SHOW);
    g_outputSplitterRect = (RECT){ area->x, top - SPLITTER_WIDTH, area->x + area->width, top };
    area->height -= g_outputPanelHeight + SPLITTER_WIDTH;
}

/* The outline pane, docked at the right edge. */
static void layout_outline(HWND hwnd, LayoutArea *area) {
    if (!g_hOutline) return;
    if (!g_outlineVisible || !editor_pane_visible() || g_focusMode) {
        ShowWindow(g_hOutline, SW_HIDE);
        return;
    }
    int outlineWidth = MulDiv(OUTLINE_WIDTH, GetDpiForWindow(hwnd), 96);
    if (outlineWidth > area->width / 3) outlineWidth = area->width / 3;
    area->width -= outlineWidth;
    MoveWindow(g_hOutline, area->x + area->width, area->y, outlineWidth, area->height, TRUE);
    ShowWindow(g_hOutline, SW_SHOW);
}

/* The editor and the preview, alone or side by side with the splitter
   between them, fill whatever the panels above left over. */
static void layout_editor_and_preview(HWND hwnd, const LayoutArea *area) {
    int x = area->x, y = area->y, width = area->width, h = area->height;
    int showEditor = editor_pane_visible();
    int showPreview = preview_pane_visible();
    if (showEditor && !showPreview) {
        layout_editor_with_minimap(hwnd, x, y, width, h);
        ShowWindow(g_hPreview, SW_HIDE);
        RECT zero = { 0, 0, 0, 0 };
        set_webview_bounds(zero, FALSE);
        return;
    }

    if (!showEditor && showPreview) {
        ShowWindow(g_hEdit, SW_HIDE);
        if (g_hMinimap) ShowWindow(g_hMinimap, SW_HIDE);
        if (g_hSplitEdit) ShowWindow(g_hSplitEdit, SW_HIDE);
        ShowWindow(g_hPreview, SW_SHOW);
        MoveWindow(g_hPreview, x, y, width, h, TRUE);
        /* WebView2 controller bounds are relative to g_hPreview's own client
           origin (which MoveWindow already positioned), not the main window. */
        RECT wvRc = { 0, 0, width, h };
        set_webview_bounds(wvRc, TRUE);
        return;
    }

    /* both visible (the !g_editorVisible && !g_previewVisible case is prevented by the toggle handlers) */
    if (g_splitterPos < 0)
        g_splitterPos = MulDiv(width - SPLITTER_WIDTH, g_splitterRatio, 1000);
    if (g_splitterPos < MIN_PANE_WIDTH) g_splitterPos = MIN_PANE_WIDTH;
    if (g_splitterPos > width - MIN_PANE_WIDTH)
        g_splitterPos = (width - MIN_PANE_WIDTH > MIN_PANE_WIDTH) ? width - MIN_PANE_WIDTH : MIN_PANE_WIDTH;

    ShowWindow(g_hPreview, SW_SHOW);
    g_splitterRect = (RECT){ x + g_splitterPos, y, x + g_splitterPos + SPLITTER_WIDTH, y + h };
    int rightWidth = width - g_splitterPos - SPLITTER_WIDTH;
    RECT wvRc;
    if (g_previewOnLeft) {
        MoveWindow(g_hPreview, x, y, g_splitterPos, h, TRUE);
        layout_editor_with_minimap(hwnd, x + g_splitterPos + SPLITTER_WIDTH, y, rightWidth, h);
        wvRc = (RECT){ 0, 0, g_splitterPos, h };
    } else {
        layout_editor_with_minimap(hwnd, x, y, g_splitterPos, h);
        MoveWindow(g_hPreview, x + g_splitterPos + SPLITTER_WIDTH, y, rightWidth, h, TRUE);
        wvRc = (RECT){ 0, 0, rightWidth, h };
    }
    set_webview_bounds(wvRc, TRUE);
}

/* Implemented in git_panel.inc, included later in the unity build - the Git
   panel is a full-height side panel like the explorer, so layout() carves
   it out alongside that one, before the tab strip and the rest. */
static void layout_git_panel(HWND hwnd, LayoutArea *area);

static void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    LayoutArea area = { 0, 0, rc.right, rc.bottom };
    g_splitterRect = (RECT){0, 0, 0, 0}; /* only the side-by-side layout sets this */
    layout_status_bar(hwnd, &area);
    layout_toolbar(&area);
    layout_explorer(hwnd, &area);
    layout_git_panel(hwnd, &area);
    layout_tabs(&area);
    layout_output_panel(hwnd, &area);
    layout_outline(hwnd, &area);
    layout_editor_and_preview(hwnd, &area);
}

/* Check marks and enabled states change without the menu being rebuilt;
   the definition decides where each item is, so the whole bar is walked. */
static void update_view_menu(HWND hwnd) {
    HMENU menuBar = GetMenu(hwnd);
    if (menuBar) apply_menu_states(menuBar);
}

/* The common tail of the View toggles: refresh check marks, re-lay out and
   repaint the panes, and remember the new layout. */
static void relayout_and_save(HWND hwnd) {
    update_view_menu(hwnd);
    layout(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
    save_layout_settings();
}

static void toggle_preview(HWND hwnd) {
    if (g_previewVisible && !g_editorVisible) return; /* would leave nothing visible */
    g_previewVisible = !g_previewVisible;
    update_view_menu(hwnd);
    layout(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
    if (g_previewVisible) update_preview();
    else KillTimer(hwnd, IDT_PREVIEW_UPDATE);
    save_layout_settings();
}

static void toggle_editor(HWND hwnd) {
    if (g_editorVisible && !g_previewVisible) return; /* would leave nothing visible */
    g_editorVisible = !g_editorVisible;
    sync_split_editor_document();
    relayout_and_save(hwnd);
}

static void toggle_panel_sides(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    /* g_splitterPos is relative to the content area beside the explorer
       and Git panels, so the mirrored position has to be computed against
       that narrower width, not the full client width. */
    int contentWidth = rc.right - explorer_panel_width(g_focusMode) - git_panel_width(g_focusMode);
    if (g_splitterPos >= 0 && contentWidth > SPLITTER_WIDTH) {
        g_splitterPos = contentWidth - SPLITTER_WIDTH - g_splitterPos;
    }
    g_previewOnLeft = !g_previewOnLeft;
    relayout_and_save(hwnd);
}

static void toggle_explorer_side(HWND hwnd) {
    g_explorerOnRight = !g_explorerOnRight;
    relayout_and_save(hwnd);
}

static void toggle_focus_mode(HWND hwnd) {
    g_focusMode = !g_focusMode;
    sync_split_editor_document();
    refresh_menu(hwnd);
    layout(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
    save_layout_settings();
    /* Focus mode hides the menu, so Ctrl+Shift+F11 is the only way back out.
       Keep focus on the editor where the accelerator is reachable - WebView2
       claims F11 for its own fullscreen whenever the preview has focus. */
    if (editor_pane_visible()) SetFocus(g_hEdit);
}

/* Cycles keyboard focus between the editor, preview, outline panel and
   output panel (when visible), and tab strip - Ctrl+F6 / Ctrl+Shift+F6, the same pane-cycling
   convention several other Windows apps use for keyboard-only navigation
   between major regions. WebView2 hosts its own separate HWND hierarchy, so
   giving it focus takes its own MoveFocus call rather than a plain
   SetFocus() on g_hPreview. */
static void cycle_pane_focus(int reverse) {
    HWND panes[6];
    int count = 0;
    if (editor_pane_visible() && g_hEdit) panes[count++] = g_hEdit;
    if (editor_pane_visible() && g_splitEditorVisible && g_hSplitEdit) panes[count++] = g_hSplitEdit;
    if (preview_pane_visible() && g_hPreview) panes[count++] = g_hPreview;
    if (g_outlineVisible && editor_pane_visible() && g_hOutline) panes[count++] = g_hOutline;
    if (g_outputPanelVisible && g_hOutputPanel) panes[count++] = g_hOutputPanel;
    if (g_hTabs) panes[count++] = g_hTabs;
    if (count == 0) return;

    HWND focused = GetFocus();
    int current = -1;
    for (int i = 0; i < count; i++) {
        if (panes[i] == focused || IsChild(panes[i], focused)) { current = i; break; }
    }
    HWND target = panes[current < 0 ? 0 : (current + (reverse ? count - 1 : 1)) % count];
    if (target == g_hPreview && g_webviewController) {
        ICoreWebView2Controller_MoveFocus(g_webviewController, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    } else if (target == g_hOutputPanel) {
        SetFocus(active_output_focus_target());
    } else {
        SetFocus(target);
    }
}

/* Edit-menu commands and their accelerators act on whichever editor pane has
   focus, so that Ctrl+C/Ctrl+Z/etc. do what the caret the user is actually
   looking at implies, rather than always reaching past a focused split pane
   to the main one. Menu clicks and accelerator keys don't change window
   focus in Win32, so GetFocus() here still reflects whichever pane the user
   was last typing in. */
static HWND active_edit_view(void) {
    return (g_hSplitEdit && GetFocus() == g_hSplitEdit) ? g_hSplitEdit : g_hEdit;
}

static void toggle_word_wrap(HWND hwnd) {
    g_wordWrap = !g_wordWrap;
    apply_word_wrap(g_hEdit);
    if (g_hSplitEdit) apply_word_wrap(g_hSplitEdit);
    /* Wrapping makes the horizontal extent meaningless; unwrapping has to
       re-measure it, which tracking does as lines come back into view. */
    if (!g_wordWrap) {
        SendMessageA(g_hEdit, SCI_SETSCROLLWIDTH, 1, 0);
        if (g_hSplitEdit) SendMessageA(g_hSplitEdit, SCI_SETSCROLLWIDTH, 1, 0);
    }
    update_view_menu(hwnd);
    save_layout_settings();
}

static void toggle_spell_check(HWND hwnd) {
    if (!g_spellChecker) return;
    g_spellCheckEnabled = !g_spellCheckEnabled;
    if (g_spellCheckEnabled) schedule_spell_check();
    else perform_spell_check(); /* clears existing indicators */
    update_view_menu(hwnd);
    save_layout_settings();
}

static void toggle_show_whitespace(HWND hwnd) {
    g_showWhitespace = !g_showWhitespace;
    apply_show_whitespace(g_hEdit);
    if (g_hSplitEdit) apply_show_whitespace(g_hSplitEdit);
    update_view_menu(hwnd);
    save_layout_settings();
}

static void toggle_indent_guides(HWND hwnd) {
    g_indentGuides = !g_indentGuides;
    apply_indent_guides(g_hEdit);
    if (g_hSplitEdit) apply_indent_guides(g_hSplitEdit);
    update_view_menu(hwnd);
    save_layout_settings();
}

static void toggle_changed_lines(HWND hwnd) {
    g_markChangedLines = !g_markChangedLines;
    apply_change_markers(g_hEdit);
    if (g_hSplitEdit) apply_change_markers(g_hSplitEdit);
    update_view_menu(hwnd);
    save_layout_settings();
}

static void toggle_indent_use_spaces(HWND hwnd) {
    g_indentUseSpaces = !g_indentUseSpaces;
    apply_indentation(g_hEdit);
    if (g_hSplitEdit) apply_indentation(g_hSplitEdit);
    /* Switching to spaces only affects how Tab now types going forward
       (apply_indentation() above) - without also converting what's already
       there, the selected document is left with a mix of tabs and spaces even
       though the setting says "spaces". A different document retained in the
       split is not rewritten unexpectedly. */
    if (g_indentUseSpaces) convert_indentation(g_hEdit, 1);
    update_view_menu(hwnd);
    save_layout_settings();
}

static void set_tab_width(int width) {
    if (width < 1) width = 1;
    if (width > 16) width = 16;
    if (width == g_tabWidth) return;
    g_tabWidth = width;
    apply_indentation(g_hEdit);
    if (g_hSplitEdit) apply_indentation(g_hSplitEdit);
    save_layout_settings();
}

/* Scintilla's own zoom range is unbounded in either direction; clamp to a
   range that stays legible (-10) and useful (+20, roughly 3x) at the
   default 10pt-ish editor font. */
static void set_editor_zoom(int zoom) {
    if (zoom < -10) zoom = -10;
    if (zoom > 20) zoom = 20;
    g_editorZoom = zoom;
    SendMessageA(g_hEdit, SCI_SETZOOM, (WPARAM)g_editorZoom, 0);
    if (g_hSplitEdit) SendMessageA(g_hSplitEdit, SCI_SETZOOM, (WPARAM)g_editorZoom, 0);
    /* Zoom changes the rendered glyph width SCI_TEXTWIDTH measures, same as a
       font face/size change does - without this the gutter can stay sized for
       the pre-zoom font until the line count happens to cross a power-of-ten
       boundary. */
    remeasure_linenumber_margin(g_hEdit);
    if (g_hSplitEdit) remeasure_linenumber_margin(g_hSplitEdit);
    save_layout_settings();
}

static void adjust_editor_zoom(int delta) {
    set_editor_zoom(g_editorZoom + delta);
}

static void reset_editor_zoom(void) {
    set_editor_zoom(0);
}

static void fold_toggle_at_cursor(void) {
    Sci_Position pos = (Sci_Position)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position line = (Sci_Position)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)pos, 0);
    SendMessageA(g_hEdit, SCI_TOGGLEFOLD, (WPARAM)line, 0);
}

static void fold_all_lines(int expand) {
    SendMessageA(g_hEdit, SCI_FOLDALL,
                 (WPARAM)(expand ? SC_FOLDACTION_EXPAND : SC_FOLDACTION_CONTRACT), 0);
}

/* Sublime/VSCode-style Ctrl+D: selects the word under the caret, or extends
   the multi-selection to the next occurrence of the current selection. */
static void select_next_occurrence(HWND hEdit) {
    SendMessageA(hEdit, SCI_MULTIPLESELECTADDNEXT, 0, 0);
}

/* An unmatched brace makes SCI_BRACEMATCH scan to the document's end, on
   every caret move next to one - g_largeFile alone doesn't bound this, since
   its threshold (g_largeFileThresholdMB) is user-configurable up into the
   hundreds of MB for documents that should still get a responsive caret. */
#define BRACE_HIGHLIGHT_MAX_DOC_LENGTH (8 * 1024 * 1024)

/* Keeps the matching-brace highlight (STYLE_BRACELIGHT/STYLE_BRACEBAD, set up
   in apply_editor_theme) in sync with the caret. Called on every SCN_UPDATEUI. */
static void update_brace_highlight(HWND hEdit) {
    HWND other = hEdit == g_hSplitEdit ? g_hEdit : g_hSplitEdit;
    if (other) SendMessageA(other, SCI_BRACEHIGHLIGHT, (WPARAM)-1, (LPARAM)-1);
    Sci_Position bracePos = -1;
    if (!g_largeFile &&
        (Sci_Position)SendMessageA(hEdit, SCI_GETLENGTH, 0, 0) <= BRACE_HIGHLIGHT_MAX_DOC_LENGTH)
        bracePos = brace_position_near_caret(hEdit);
    if (bracePos < 0) {
        SendMessageA(hEdit, SCI_BRACEHIGHLIGHT, (WPARAM)-1, (LPARAM)-1);
        return;
    }
    Sci_Position match = (Sci_Position)SendMessageA(hEdit, SCI_BRACEMATCH, (WPARAM)bracePos, 0);
    if (match >= 0) SendMessageA(hEdit, SCI_BRACEHIGHLIGHT, (WPARAM)bracePos, (LPARAM)match);
    else SendMessageA(hEdit, SCI_BRACEBADLIGHT, (WPARAM)bracePos, 0);
}

#define WORD_COMPLETION_MIN_PREFIX 2
#define WORD_COMPLETION_MAX_WORDS 100
/* Caps the scanned span around the caret, each direction, so a trigger near
   the start of a huge document can't turn into a near-full-document
   SCI_SEARCHINTARGET scan on every newly typed word. */
#define WORD_COMPLETION_SCAN_RADIUS (256 * 1024)

static int compare_words_ordinal(const void *a, const void *b) {
    int folded = _stricmp(*(const char *const *)a, *(const char *const *)b);
    return folded ? folded : strcmp(*(const char *const *)a, *(const char *const *)b);
}

static BOOL CALLBACK find_autocomplete_popup_proc(HWND hwnd, LPARAM lParam) {
    WCHAR className[16];
    if (GetClassNameW(hwnd, className, 16) == 0 || wcscmp(className, L"ListBoxX") != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    *(HWND *)lParam = hwnd;
    return FALSE;
}

/* SCI_AUTOCSHOW's popup is a native listbox (Win32 class "ListBoxX", with
   the actual WC_LISTBOX scrollbar-owning control as its one child) that
   Scintilla itself never dark-mode themes - its scrollbar is left to the
   system and paints a plain light scrollbar even in dark mode (unlike
   hEdit's, it can't be swapped for a themed_scrollbar.h one, since Scintilla
   creates and owns this window). Scintilla doesn't hand
   back the popup's HWND, so it's found by class name right after showing it
   (SCI_AUTOCSHOW creates/shows the popup synchronously), scoped to this
   process so a second Scintilla-based app's own popup is never touched.
   The popup frame itself (WS_POPUP | WS_THICKFRAME) also needs
   apply_popup_titlebar_theme()'s DWM dark-mode attribute, same as this
   app's own popups get - without it, Windows 11 paints that frame's rounded
   top strip in the light system color regardless of the listbox's own
   theme, showing as a solid white bar across the top of the list. */
static void theme_autocomplete_popup(void) {
    HWND popup = NULL;
    EnumWindows(find_autocomplete_popup_proc, (LPARAM)&popup);
    /* EnumWindows writes popup through the callback's lParam. */
    /* cppcheck-suppress knownConditionTrueFalse */
    if (!popup) return;
    apply_popup_titlebar_theme(popup, g_editorDarkMode);
    HWND list = GetWindow(popup, GW_CHILD);
    if (list) apply_explorer_theme(list);
}

/* Plain word completion, not lexer/keyword-driven (see the AutoC setup in
   configure_scintilla()): as the caret's word grows to WORD_COMPLETION_MIN_PREFIX
   characters, show every other word elsewhere in the document that starts
   with it (case-insensitively, via SCFIND_WORDSTART - the same search
   primitive ctrl_click_jump_to_occurrence() uses). Called on every
   SCN_CHARADDED; does nothing once the list is already showing, since
   Scintilla narrows the visible entries itself as more of the word is
   typed. SCN_CHARADDED schedules this after a short idle delay so the bounded
   document scan never runs synchronously in the keypress notification. */
static void update_word_completions(HWND hEdit) {
    if (g_largeFile) return;
    if (update_wikilink_completions(hEdit)) return;
    Sci_Position pos = (Sci_Position)SendMessageA(hEdit, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position wordStart = (Sci_Position)SendMessageA(hEdit, SCI_WORDSTARTPOSITION, (WPARAM)pos, (LPARAM)1);
    Sci_Position prefixLen = pos - wordStart;
    if (prefixLen < WORD_COMPLETION_MIN_PREFIX) return;
    if (SendMessageA(hEdit, SCI_AUTOCACTIVE, 0, 0)) return;

    char *prefix = get_text_range(hEdit, wordStart, pos);
    if (!prefix) return;

    Sci_Position docLength = (Sci_Position)SendMessageA(hEdit, SCI_GETLENGTH, 0, 0);
    Sci_Position scanStart = wordStart > WORD_COMPLETION_SCAN_RADIUS ? wordStart - WORD_COMPLETION_SCAN_RADIUS : 0;
    Sci_Position scanEnd = docLength - pos > WORD_COMPLETION_SCAN_RADIUS ? pos + WORD_COMPLETION_SCAN_RADIUS : docLength;
    SendMessageA(hEdit, SCI_SETSEARCHFLAGS, SCFIND_WORDSTART, 0);

    char *words[WORD_COMPLETION_MAX_WORDS];
    int wordCount = 0;
    Sci_Position searchFrom = scanStart;
    while (wordCount < WORD_COMPLETION_MAX_WORDS) {
        SendMessageA(hEdit, SCI_SETTARGETSTART, (WPARAM)searchFrom, 0);
        SendMessageA(hEdit, SCI_SETTARGETEND, (WPARAM)scanEnd, 0);
        Sci_Position matchStart = (Sci_Position)SendMessageA(hEdit, SCI_SEARCHINTARGET, (WPARAM)prefixLen, (LPARAM)prefix);
        if (matchStart < 0) break;
        Sci_Position matchTargetEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETTARGETEND, 0, 0);
        searchFrom = matchTargetEnd;

        if (matchStart == wordStart) continue; /* the word being typed itself */
        Sci_Position wordEnd = (Sci_Position)SendMessageA(hEdit, SCI_WORDENDPOSITION, (WPARAM)matchStart, (LPARAM)1);
        Sci_Position candidateLen = wordEnd - matchStart;
        if (candidateLen <= prefixLen) continue;

        char *candidate = get_text_range(hEdit, matchStart, wordEnd);
        if (!candidate) continue;

        int duplicate = 0;
        for (int w = 0; w < wordCount; w++) {
            if (strcmp(words[w], candidate) == 0) { duplicate = 1; break; }
        }
        if (duplicate) { free(candidate); continue; }
        words[wordCount++] = candidate;
    }
    free(prefix);

    if (wordCount == 0) { SendMessageA(hEdit, SCI_AUTOCCANCEL, 0, 0); return; }

    qsort(words, (size_t)wordCount, sizeof(char *), compare_words_ordinal);

    size_t total = 0;
    for (int w = 0; w < wordCount; w++) total += strlen(words[w]) + 1;
    char *list = (char *)malloc(total);
    if (list) {
        size_t o = 0;
        for (int w = 0; w < wordCount; w++) {
            size_t wordLen = strlen(words[w]);
            memcpy(list + o, words[w], wordLen);
            o += wordLen;
            if (w + 1 < wordCount) list[o++] = '\n';
        }
        list[o] = 0;
        SendMessageA(hEdit, SCI_AUTOCSHOW, (WPARAM)prefixLen, (LPARAM)list);
        theme_autocomplete_popup();
        free(list);
    }
    for (int w = 0; w < wordCount; w++) free(words[w]);
}

/* Sublime/VSCode-style Ctrl+Shift+L: every occurrence of the word under the
   caret (or of the current selection) becomes its own selection at once. */
static void select_all_occurrences(HWND hEdit) {
    SendMessageA(hEdit, SCI_MULTIPLESELECTADDEACH, 0, 0);
}

/* Bookmarks are Scintilla markers on MARKER_BOOKMARK, which live on the
   document buffer itself - each tab keeps its own set automatically as
   SCI_SETDOCPOINTER swaps between them. */
static void toggle_bookmark_at_line(Sci_Position line) {
    int mask = (int)SendMessageA(g_hEdit, SCI_MARKERGET, (WPARAM)line, 0);
    if (mask & (1 << MARKER_BOOKMARK))
        SendMessageA(g_hEdit, SCI_MARKERDELETE, (WPARAM)line, (LPARAM)MARKER_BOOKMARK);
    else
        SendMessageA(g_hEdit, SCI_MARKERADD, (WPARAM)line, (LPARAM)MARKER_BOOKMARK);
}

static void toggle_bookmark_at_cursor(HWND hEdit) {
    Sci_Position pos = (Sci_Position)SendMessageA(hEdit, SCI_GETCURRENTPOS, 0, 0);
    toggle_bookmark_at_line((Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)pos, 0));
}

static void goto_bookmark(HWND hEdit, int forward) {
    Sci_Position pos = (Sci_Position)SendMessageA(hEdit, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position line = (Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)pos, 0);
    Sci_Position found;
    if (forward) {
        found = (Sci_Position)SendMessageA(hEdit, SCI_MARKERNEXT, (WPARAM)(line + 1), (LPARAM)(1 << MARKER_BOOKMARK));
        if (found < 0) found = (Sci_Position)SendMessageA(hEdit, SCI_MARKERNEXT, 0, (LPARAM)(1 << MARKER_BOOKMARK));
    } else {
        found = (Sci_Position)SendMessageA(hEdit, SCI_MARKERPREVIOUS, (WPARAM)(line - 1), (LPARAM)(1 << MARKER_BOOKMARK));
        if (found < 0) {
            Sci_Position lastLine = (Sci_Position)SendMessageA(hEdit, SCI_GETLINECOUNT, 0, 0) - 1;
            found = (Sci_Position)SendMessageA(hEdit, SCI_MARKERPREVIOUS, (WPARAM)lastLine, (LPARAM)(1 << MARKER_BOOKMARK));
        }
    }
    if (found < 0) return;
    Sci_Position gotoPos = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)found, 0);
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)gotoPos, (LPARAM)gotoPos);
    SendMessageA(hEdit, SCI_SCROLLCARET, 0, 0);
}

static void clear_all_bookmarks(void) {
    SendMessageA(g_hEdit, SCI_MARKERDELETEALL, (WPARAM)MARKER_BOOKMARK, 0);
}
