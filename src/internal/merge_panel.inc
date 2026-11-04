/* ---- the Merge Conflicts panel ----
   Resolves Git's own in-file "<<<<<<< / ======= / >>>>>>>" markers without
   needing an external merge tool (see git_resolve_conflict_in_merge_tool()
   in output_panel.inc, still offered separately in the Git tree's context
   menu for anyone who prefers one). Docked at the right edge, the same
   shape as the Git panel (see layout_merge_panel()): a small file list above
   a Scintilla view of the selected file's raw text, with buttons that
   resolve whichever conflict hunk the caret is in or the next one below it.

   g_hMergeView is its own Scintilla buffer, not one of the editor's own
   document tabs - it reads and writes the real working-tree file directly,
   so if that same file also happens to be open for editing, the existing
   file-watch reload (IDT_FILE_WATCH) is what picks up the change, the same
   way it would for any other external edit. */

static HWND g_hMergePanel, g_hMergeList, g_hMergeView;
static HWND g_hMergeAcceptOurs, g_hMergeAcceptTheirs, g_hMergeAcceptBoth, g_hMergeNext;
static HWND g_hMergeRefresh, g_hMergeSave, g_hMergeClose;
static WCHAR *g_mergeCurrentPath; /* workspace-relative path now loaded into g_hMergeView, or NULL */
static int g_mergeCurrentEncoding;
static int g_mergeCurrentHasBom;
static RECT g_mergePanelSplitterRect = {0}; /* empty while the panel is hidden */

typedef struct { Sci_Position startLine, sepLine, endLine; } MergeHunk;

static int merge_line_starts_with(HWND view, Sci_Position line, const char *prefix) {
    Sci_Position pos = (Sci_Position)SendMessageA(view, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
    for (const char *p = prefix; *p; p++, pos++)
        if ((char)SendMessageA(view, SCI_GETCHARAT, (WPARAM)pos, 0) != *p) return 0;
    return 1;
}

/* The conflict hunk containing `caretLine`, or failing that the next one
   below it (so "Next Conflict" and the Accept buttons share one search). A
   "<<<<<<<" without a matching "=======" and ">>>>>>>" after it - a file
   mid-edit, not a real conflict - ends the search rather than misreading
   whatever follows. */
static int merge_find_hunk(HWND view, Sci_Position caretLine, MergeHunk *out) {
    Sci_Position lineCount = (Sci_Position)SendMessageA(view, SCI_GETLINECOUNT, 0, 0);
    int haveNext = 0;
    MergeHunk next = {0};
    for (Sci_Position line = 0; line < lineCount; line++) {
        if (!merge_line_starts_with(view, line, "<<<<<<<")) continue;
        Sci_Position sep = -1, end = -1;
        for (Sci_Position l2 = line + 1; l2 < lineCount; l2++) {
            if (sep < 0) { if (merge_line_starts_with(view, l2, "=======")) sep = l2; continue; }
            if (merge_line_starts_with(view, l2, ">>>>>>>")) { end = l2; break; }
        }
        if (sep < 0 || end < 0) break;
        if (caretLine >= line && caretLine <= end) { *out = (MergeHunk){ line, sep, end }; return 1; }
        if (!haveNext && line > caretLine) { haveNext = 1; next = (MergeHunk){ line, sep, end }; }
        line = end;
    }
    if (haveNext) { *out = next; return 1; }
    return 0;
}

static Sci_Position merge_line_pos(HWND view, Sci_Position line) {
    return (Sci_Position)SendMessageA(view, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
}

static void merge_select_hunk(HWND view, const MergeHunk *hunk) {
    Sci_Position start = merge_line_pos(view, hunk->startLine);
    Sci_Position end = (Sci_Position)SendMessageA(view, SCI_GETLINEENDPOSITION, (WPARAM)hunk->endLine, 0);
    SendMessageA(view, SCI_SETSEL, (WPARAM)start, (LPARAM)end);
    SendMessageA(view, SCI_SCROLLCARET, 0, 0);
}

/* Replaces the whole hunk (its "<<<<<<<" line through its ">>>>>>>" line)
   with `side`, which is the caller's own choice of the ours text, the
   theirs text, or both concatenated - already fetched with get_text_range(),
   since fetching it has to happen before the replace shifts positions. */
static void merge_apply_resolution(HWND view, const MergeHunk *hunk, const char *side, Sci_Position sideLength) {
    Sci_Position hunkStart = merge_line_pos(view, hunk->startLine);
    Sci_Position hunkEnd = merge_line_pos(view, hunk->endLine + 1);
    SendMessageA(view, SCI_SETTARGETSTART, (WPARAM)hunkStart, 0);
    SendMessageA(view, SCI_SETTARGETEND, (WPARAM)hunkEnd, 0);
    SendMessageA(view, SCI_REPLACETARGET, (WPARAM)sideLength, (LPARAM)side);
    MergeHunk following;
    if (merge_find_hunk(view, (Sci_Position)SendMessageA(view, SCI_LINEFROMPOSITION, (WPARAM)hunkStart, 0),
                        &following))
        merge_select_hunk(view, &following);
    else
        SendMessageA(view, SCI_GOTOPOS, (WPARAM)hunkStart, 0);
}

enum { MERGE_ACCEPT_OURS, MERGE_ACCEPT_THEIRS, MERGE_ACCEPT_BOTH };

static void merge_accept(int which) {
    if (!g_hMergeView || !g_mergeCurrentPath) return;
    Sci_Position caretLine = (Sci_Position)SendMessageA(g_hMergeView, SCI_LINEFROMPOSITION,
        (WPARAM)SendMessageA(g_hMergeView, SCI_GETCURRENTPOS, 0, 0), 0);
    MergeHunk hunk;
    if (!merge_find_hunk(g_hMergeView, caretLine, &hunk)) {
        MessageBoxW(g_hMainWnd, L"No conflict markers found.", L"Merge", MB_OK | MB_ICONINFORMATION);
        return;
    }
    Sci_Position oursBegin = merge_line_pos(g_hMergeView, hunk.startLine + 1);
    Sci_Position oursEnd = merge_line_pos(g_hMergeView, hunk.sepLine);
    Sci_Position theirsBegin = merge_line_pos(g_hMergeView, hunk.sepLine + 1);
    Sci_Position theirsEnd = merge_line_pos(g_hMergeView, hunk.endLine);
    char *ours = which != MERGE_ACCEPT_THEIRS ? get_text_range(g_hMergeView, oursBegin, oursEnd) : NULL;
    char *theirs = which != MERGE_ACCEPT_OURS ? get_text_range(g_hMergeView, theirsBegin, theirsEnd) : NULL;
    if (which == MERGE_ACCEPT_OURS) {
        merge_apply_resolution(g_hMergeView, &hunk, ours ? ours : "", (Sci_Position)(ours ? strlen(ours) : 0));
    } else if (which == MERGE_ACCEPT_THEIRS) {
        merge_apply_resolution(g_hMergeView, &hunk, theirs ? theirs : "", (Sci_Position)(theirs ? strlen(theirs) : 0));
    } else {
        size_t oursLen = ours ? strlen(ours) : 0, theirsLen = theirs ? strlen(theirs) : 0;
        char *both = (char *)malloc(oursLen + theirsLen + 1);
        if (both) {
            if (ours) memcpy(both, ours, oursLen);
            if (theirs) memcpy(both + oursLen, theirs, theirsLen);
            both[oursLen + theirsLen] = 0;
            merge_apply_resolution(g_hMergeView, &hunk, both, (Sci_Position)(oursLen + theirsLen));
            free(both);
        }
    }
    free(ours);
    free(theirs);
    SetFocus(g_hMergeView);
}

static void merge_go_to_next_conflict(void) {
    if (!g_hMergeView || !g_mergeCurrentPath) return;
    Sci_Position caretLine = (Sci_Position)SendMessageA(g_hMergeView, SCI_LINEFROMPOSITION,
        (WPARAM)SendMessageA(g_hMergeView, SCI_GETCURRENTPOS, 0, 0), 0);
    MergeHunk hunk;
    /* +1 so Next moves past the hunk the caret (or selection) is already in,
       rather than reselecting it. */
    if (merge_find_hunk(g_hMergeView, caretLine + 1, &hunk)) merge_select_hunk(g_hMergeView, &hunk);
    else MessageBoxW(g_hMainWnd, L"No more conflict markers in this file.", L"Merge", MB_OK | MB_ICONINFORMATION);
}

/* The Git tree marks a file conflicted the same way git_status_file_is_conflict()
   (git_panel.inc, included just before this file) does; this list is just
   that same filter, kept in the order g_gitStatusFiles already has them. */
static void render_merge_panel_list(void) {
    if (!g_hMergeList) return;
    WCHAR *selected = g_mergeCurrentPath ? _wcsdup(g_mergeCurrentPath) : NULL;
    SendMessageW(g_hMergeList, LB_RESETCONTENT, 0, 0);
    int reselect = -1;
    int row = 0;
    for (int i = 0; i < g_gitStatusFileCount; i++) {
        GitStatusFile *file = &g_gitStatusFiles[i];
        if (!git_status_is_conflict((char)file->index, (char)file->worktree)) continue;
        SendMessageW(g_hMergeList, LB_ADDSTRING, 0, (LPARAM)file->path);
        if (selected && _wcsicmp(selected, file->path) == 0) reselect = row;
        row++;
    }
    if (reselect >= 0) {
        SendMessageW(g_hMergeList, LB_SETCURSEL, (WPARAM)reselect, 0);
    } else {
        /* The file that was open is resolved, staged, or gone - nothing to
           keep showing. */
        free(g_mergeCurrentPath);
        g_mergeCurrentPath = NULL;
        if (g_hMergeView) SendMessageA(g_hMergeView, SCI_CLEARALL, 0, 0);
    }
    free(selected);
}

static void merge_load_file(const WCHAR *relativePath) {
    WCHAR *fullPath = workspace_file_path(relativePath);
    if (!fullPath) return;
    WCHAR *ioPath = extended_path(fullPath);
    int length = 0, encoding = 0, hasBom = 0;
    char *text = read_text_file(ioPath ? ioPath : fullPath, &length, &encoding, &hasBom);
    free(ioPath);
    free(fullPath);
    if (!text) {
        show_error(L"Could not read this file.");
        return;
    }
    free(g_mergeCurrentPath);
    g_mergeCurrentPath = _wcsdup(relativePath);
    g_mergeCurrentEncoding = encoding;
    g_mergeCurrentHasBom = hasBom;
    SendMessageA(g_hMergeView, SCI_SETTEXT, 0, (LPARAM)text);
    SendMessageA(g_hMergeView, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessageA(g_hMergeView, SCI_SETSAVEPOINT, 0, 0);
    free(text);
    MergeHunk hunk;
    if (merge_find_hunk(g_hMergeView, 0, &hunk)) merge_select_hunk(g_hMergeView, &hunk);
}

static int merge_confirm_discard_if_dirty(void) {
    if (!g_hMergeView || !SendMessageA(g_hMergeView, SCI_GETMODIFY, 0, 0)) return 1;
    return MessageBoxW(g_hMainWnd, L"Discard the unsaved changes to this file's resolution?",
                       L"Merge", MB_YESNO | MB_ICONWARNING) == IDYES;
}

static void handle_merge_list_selection(void) {
    int index = (int)SendMessageW(g_hMergeList, LB_GETCURSEL, 0, 0);
    if (index == LB_ERR) return;
    WCHAR *path = listbox_item_text(g_hMergeList, index);
    if (!path) return;
    if (g_mergeCurrentPath && _wcsicmp(path, g_mergeCurrentPath) == 0) { free(path); return; }
    if (!merge_confirm_discard_if_dirty()) {
        /* Put the selection back on the file that's still loaded. */
        for (int i = 0; i < (int)SendMessageW(g_hMergeList, LB_GETCOUNT, 0, 0); i++) {
            WCHAR *candidate = listbox_item_text(g_hMergeList, i);
            int match = candidate && g_mergeCurrentPath && _wcsicmp(candidate, g_mergeCurrentPath) == 0;
            free(candidate);
            if (match) { SendMessageW(g_hMergeList, LB_SETCURSEL, (WPARAM)i, 0); break; }
        }
        free(path);
        return;
    }
    merge_load_file(path);
    free(path);
}

static void merge_save_and_stage(void) {
    if (!g_hMergeView || !g_mergeCurrentPath) return;
    MergeHunk hunk;
    if (merge_find_hunk(g_hMergeView, 0, &hunk) &&
        MessageBoxW(g_hMainWnd, L"This file still has unresolved conflict markers. Save it anyway?",
                    L"Merge", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;
    Sci_Position length = 0;
    char *text = get_edit_text(g_hMergeView, &length);
    if (!text) { show_error(L"Not enough memory to save this file."); return; }
    WCHAR *fullPath = workspace_file_path(g_mergeCurrentPath);
    WCHAR *ioPath = fullPath ? extended_path(fullPath) : NULL;
    int ok = fullPath && write_file_atomic(ioPath ? ioPath : fullPath, text, (size_t)length,
                                          g_mergeCurrentEncoding, g_mergeCurrentHasBom);
    free(ioPath);
    free(text);
    if (!ok) {
        free(fullPath);
        show_error(L"Could not save this file.");
        return;
    }
    SendMessageA(g_hMergeView, SCI_SETSAVEPOINT, 0, 0);
    WCHAR *relativePath = g_mergeCurrentPath; /* git_stage_file()/refresh_git_status() may rebuild the list */
    g_mergeCurrentPath = NULL;
    git_stage_file(relativePath);
    free(relativePath);
    free(fullPath);
}

/* ---- the panel's own window: list, action buttons, view, footer buttons,
   the same shape git_panel.inc's "Git panel's own window" section below
   uses for its own tree/commit box. ---- */

typedef struct MergePanelLayout {
    int pad, header, row;
    RECT list, actions, view, footer;
} MergePanelLayout;

static MergePanelLayout merge_panel_layout(HWND panel) {
    RECT rc;
    GetClientRect(panel, &rc);
    int dpi = GetDpiForWindow(panel);
    MergePanelLayout layout;
    layout.pad = MulDiv(OUTPUT_PANEL_PADDING, dpi, 96);
    layout.header = MulDiv(OUTPUT_HEADER_HEIGHT, dpi, 96);
    layout.row = layout.header - layout.pad;
    int listHeight = MulDiv(MERGE_LIST_HEIGHT, dpi, 96);
    layout.list = (RECT){ layout.pad, layout.header, rc.right - layout.pad, layout.header + listHeight };
    int actionsTop = layout.list.bottom + layout.pad;
    layout.actions = (RECT){ layout.pad, actionsTop, rc.right - layout.pad, actionsTop + layout.row };
    int footerTop = rc.bottom - layout.pad - layout.row;
    layout.footer = (RECT){ layout.pad, footerTop, rc.right - layout.pad, footerTop + layout.row };
    layout.view = (RECT){ layout.pad, layout.actions.bottom + layout.pad,
                          rc.right - layout.pad, footerTop - layout.pad };
    if (layout.view.bottom < layout.view.top) layout.view.bottom = layout.view.top;
    return layout;
}

static void size_merge_panel(HWND panel) {
    RECT rc;
    GetClientRect(panel, &rc);
    int dpi = GetDpiForWindow(panel);
    MergePanelLayout layout = merge_panel_layout(panel);
    int closeWidth = MulDiv(DEBUG_CLOSE_BUTTON_WIDTH, dpi, 96);
    MoveWindow(g_hMergeClose, rc.right - layout.pad - closeWidth, layout.pad / 2,
              closeWidth, layout.row, TRUE);
    MoveWindow(g_hMergeList, layout.list.left + 1, layout.list.top + 1,
              layout.list.right - layout.list.left - 2, layout.list.bottom - layout.list.top - 2, TRUE);
    int actionWidth = (layout.actions.right - layout.actions.left - 3 * layout.pad) / 4;
    if (actionWidth < 0) actionWidth = 0;
    HWND actionButtons[] = { g_hMergeAcceptOurs, g_hMergeAcceptTheirs, g_hMergeAcceptBoth, g_hMergeNext };
    for (int i = 0; i < 4; i++) {
        int x = layout.actions.left + i * (actionWidth + layout.pad);
        int width = i == 3 ? layout.actions.right - x : actionWidth;
        MoveWindow(actionButtons[i], x, layout.actions.top, width, layout.row, TRUE);
        InvalidateRect(actionButtons[i], NULL, TRUE);
    }
    MoveWindow(g_hMergeView, layout.view.left + 1, layout.view.top + 1,
              layout.view.right - layout.view.left - 2, layout.view.bottom - layout.view.top - 2, TRUE);
    int footerWidth = (layout.footer.right - layout.footer.left - layout.pad) / 2;
    if (footerWidth < 0) footerWidth = 0;
    MoveWindow(g_hMergeRefresh, layout.footer.left, layout.footer.top, footerWidth, layout.row, TRUE);
    MoveWindow(g_hMergeSave, layout.footer.left + footerWidth + layout.pad, layout.footer.top,
              layout.footer.right - (layout.footer.left + footerWidth + layout.pad), layout.row, TRUE);
    InvalidateRect(g_hMergeRefresh, NULL, TRUE);
    InvalidateRect(g_hMergeSave, NULL, TRUE);
}

static void apply_merge_panel_font(const char *faceUtf8) {
    if (!g_hMergeView) return;
    SendMessageA(g_hMergeView, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)faceUtf8);
    SendMessageA(g_hMergeView, SCI_STYLESETSIZE, STYLE_DEFAULT, g_fontPointSize);
    SendMessageA(g_hMergeView, SCI_STYLESETWEIGHT, STYLE_DEFAULT, g_fontWeight);
    apply_output_view_theme(g_hMergeView);
}

static void apply_merge_panel_theme(void) {
    if (!g_hMergeList) return;
    apply_explorer_theme(g_hMergeList);
    themed_scrollbar_refresh(g_hMergeList);
    InvalidateRect(g_hMergeList, NULL, TRUE);
    if (g_hMergePanel) InvalidateRect(g_hMergePanel, NULL, TRUE);
    apply_output_view_theme(g_hMergeView);
    HWND buttons[] = { g_hMergeAcceptOurs, g_hMergeAcceptTheirs, g_hMergeAcceptBoth, g_hMergeNext,
                      g_hMergeRefresh, g_hMergeSave, g_hMergeClose };
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        if (buttons[i]) InvalidateRect(buttons[i], NULL, TRUE);
}

static void paint_merge_panel(HWND panel, HDC hdc) {
    RECT rc;
    GetClientRect(panel, &rc);
    fill_rect_color(hdc, &rc, theme_bg_color());
    MergePanelLayout layout = merge_panel_layout(panel);
    frame_rect(hdc, &layout.list, theme_border_color());
    frame_rect(hdc, &layout.view, theme_border_color());
}

static void hide_merge_panel(void);

static LRESULT CALLBACK MergePanelSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                               LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
    (void)id; (void)data;
    switch (msg) {
        case WM_SIZE:
            size_merge_panel(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORBTN:
            if (g_mainBkBrush) return (LRESULT)g_mainBkBrush;
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            paint_merge_panel(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            paint_merge_panel(hwnd, (HDC)wParam);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_MERGE_LIST:
                    if (HIWORD(wParam) == LBN_SELCHANGE) handle_merge_list_selection();
                    return 0;
                case ID_MERGE_ACCEPT_OURS: merge_accept(MERGE_ACCEPT_OURS); return 0;
                case ID_MERGE_ACCEPT_THEIRS: merge_accept(MERGE_ACCEPT_THEIRS); return 0;
                case ID_MERGE_ACCEPT_BOTH: merge_accept(MERGE_ACCEPT_BOTH); return 0;
                case ID_MERGE_NEXT: merge_go_to_next_conflict(); return 0;
                case ID_MERGE_REFRESH: refresh_git_status(); return 0;
                case ID_MERGE_SAVE: merge_save_and_stage(); return 0;
                case ID_MERGE_CLOSE: hide_merge_panel(); return 0;
            }
            break;
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) { draw_debug_panel_button(dis); return TRUE; }
            if (dis->CtlType == ODT_LISTBOX && dis->hwndItem == g_hMergeList) {
                draw_themed_listbox_item(g_hMergeList, dis);
                return TRUE;
            }
            break;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
            if (mis->CtlType == ODT_LISTBOX) { measure_themed_listbox_item(g_hMergeList, mis); return TRUE; }
            break;
        }
        case WM_CTLCOLORSTATIC:
            if (g_mainBkBrush) {
                HDC hdc = (HDC)wParam;
                SetTextColor(hdc, theme_fg_color());
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)g_mainBkBrush;
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, MergePanelSubclassProc, 1);
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void create_merge_panel(HWND hwnd) {
    g_hMergePanel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_CLIPCHILDREN,
                                    0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)ID_MERGE_PANEL,
                                    GetModuleHandleW(NULL), NULL);
    SetWindowSubclass(g_hMergePanel, MergePanelSubclassProc, 1, 0);
    g_hMergeList = CreateWindowExW(0, L"LISTBOX", L"",
        WS_CHILD | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        0, 0, 0, 0, g_hMergePanel, (HMENU)(UINT_PTR)ID_MERGE_LIST, GetModuleHandleW(NULL), NULL);
    theme_list_frame(g_hMergeList);
    themed_scrollbar_attach(g_hMergeList, THEMED_SCROLL_LISTBOX);
    SendMessageW(g_hMergeList, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    g_hMergeAcceptOurs = create_output_button(g_hMergePanel, L"Accept Ours", ID_MERGE_ACCEPT_OURS);
    g_hMergeAcceptTheirs = create_output_button(g_hMergePanel, L"Accept Theirs", ID_MERGE_ACCEPT_THEIRS);
    g_hMergeAcceptBoth = create_output_button(g_hMergePanel, L"Accept Both", ID_MERGE_ACCEPT_BOTH);
    g_hMergeNext = create_output_button(g_hMergePanel, L"Next Conflict", ID_MERGE_NEXT);
    g_hMergeView = create_output_scintilla(g_hMergePanel, ID_MERGE_VIEW, 1);
    SendMessageA(g_hMergeView, SCI_SETHSCROLLBAR, TRUE, 0);
    themed_scrollbar_attach(g_hMergeView, THEMED_SCROLL_SCINTILLA);
    g_hMergeRefresh = create_output_button(g_hMergePanel, L"Refresh", ID_MERGE_REFRESH);
    g_hMergeSave = create_output_button(g_hMergePanel, L"Save && Stage", ID_MERGE_SAVE);
    g_hMergeClose = create_output_button(g_hMergePanel, L"X", ID_MERGE_CLOSE);
    ShowWindow(g_hMergePanel, g_mergePanelVisible ? SW_SHOW : SW_HIDE);
    /* Same reasoning as create_git_panel()'s own apply_git_panel_theme()/
       apply_git_panel_font() calls: this runs before the startup sequence's
       own theme/font application, so without this the panel would start in
       the system's default colours and font. */
    apply_merge_panel_theme();
    char *faceUtf8 = to_utf8(g_fontFaceName);
    if (!faceUtf8) faceUtf8 = _strdup("Consolas");
    if (faceUtf8) {
        apply_merge_panel_font(faceUtf8);
        free(faceUtf8);
    }
}

/* Always docked at the right, carved out of whatever area{} the Git panel
   (if also docked right) left behind - see layout_git_panel() just above
   this file's #include in tinkeredit.c. */
static void layout_merge_panel(HWND hwnd, LayoutArea *area) {
    if (!g_hMergePanel || !g_mergePanelVisible || g_focusMode) {
        g_mergePanelSplitterRect = (RECT){0, 0, 0, 0};
        if (g_hMergePanel) ShowWindow(g_hMergePanel, SW_HIDE);
        return;
    }
    int dpi = GetDpiForWindow(hwnd);
    if (g_mergePanelWidth < 0)
        g_mergePanelWidth = MulDiv(MERGE_PANEL_WIDTH, dpi, 96);
    int minWidth = MulDiv(MERGE_PANEL_MIN_WIDTH, dpi, 96);
    int maxWidth = area->width * 2 / 3;
    if (g_mergePanelWidth < minWidth) g_mergePanelWidth = minWidth;
    if (maxWidth > minWidth && g_mergePanelWidth > maxWidth) g_mergePanelWidth = maxWidth;
    int panelX = area->x + area->width - g_mergePanelWidth;
    g_mergePanelSplitterRect = (RECT){ panelX - SPLITTER_WIDTH, area->y,
                                      panelX, area->y + area->height };
    MoveWindow(g_hMergePanel, panelX, area->y, g_mergePanelWidth, area->height, TRUE);
    area->width -= g_mergePanelWidth + SPLITTER_WIDTH;
    if (area->width < 0) area->width = 0;
    ShowWindow(g_hMergePanel, SW_SHOW);
}

static int merge_panel_splitter_contains_x(int x) {
    return g_mergePanelVisible && !g_focusMode &&
           x >= g_mergePanelSplitterRect.left && x <= g_mergePanelSplitterRect.right;
}

static void hide_merge_panel(void) {
    if (!g_mergePanelVisible) return;
    g_mergePanelVisible = 0;
    relayout_and_save(g_hMainWnd);
}

/* Git > Merge Conflicts Panel. */
static void show_merge_panel(void) {
    refresh_git_status();
    render_merge_panel_list();
    if (!g_mergePanelVisible) {
        g_mergePanelVisible = 1;
        relayout_and_save(g_hMainWnd);
    }
    if (g_hMergeList) SetFocus(g_hMergeList);
}

static void toggle_merge_panel(void) {
    if (g_mergePanelVisible) hide_merge_panel();
    else show_merge_panel();
}
