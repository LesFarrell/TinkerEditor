/* Defined in documents.inc, included after this file. */
static int document_index_for_view(HWND view);

/* ---- window, menu and title-bar theming ---- */

/* Keeps the WebView2 controller's own background in step with the theme. It
   shows through before the shell page paints: at startup, while resizing, and
   for the frame between a theme switch and the page restyling itself. */
static void apply_webview_background_colour(void) {
    if (!g_webviewController) return;
    ICoreWebView2Controller2 *ctrl2 = NULL;
    ICoreWebView2Controller_QueryInterface(g_webviewController, &IID_ICoreWebView2Controller2,
                                           (void **)&ctrl2);
    if (!ctrl2) return;
    /* Image tabs belong visually to the editor side of the application even
       though WebView2 displays them. Match the editor palette during the
       navigation frame too, before the image document has painted. */
    COLORREF colour = active_document_is_image() ? theme_bg_color() : preview_bg_color();
    COREWEBVIEW2_COLOR bg = {255, GetRValue(colour), GetGValue(colour), GetBValue(colour)};
    ICoreWebView2Controller2_put_DefaultBackgroundColor(ctrl2, bg);
    ICoreWebView2Controller2_Release(ctrl2);
}

/* (Re)creates g_previewBkBrush for the content currently occupying the
   WebView host: the preview palette for rendered documents, or the editor
   palette for image tabs. This is the colour visible before WebView2 paints. */
static void apply_preview_host_theme(void) {
    if (g_previewBkBrush) DeleteObject(g_previewBkBrush);
    COLORREF colour = active_document_is_image() ? theme_bg_color() : preview_bg_color();
    g_previewBkBrush = CreateSolidBrush(colour);
    if (g_hPreview) InvalidateRect(g_hPreview, NULL, TRUE);
}

/* (Re)creates g_mainBkBrush to match g_editorDarkMode, and repaints the
   parts of the main window's own background it's used for (the toolbar
   strip and the splitter gap - everywhere else is covered by child windows). */
static void apply_main_theme(HWND hwnd) {
    if (g_mainBkBrush) DeleteObject(g_mainBkBrush);
    g_mainBkBrush = CreateSolidBrush(theme_bg_color());
    if (hwnd) InvalidateRect(hwnd, NULL, TRUE);

    /* The outline's content is owner-drawn (see draw_themed_listbox_item());
       its scrollbar is themed separately (see themed_scrollbar.h), and its
       border is a themed frame (theme_list_frame()), hence RDW_FRAME. */
    if (g_hOutline) {
        themed_scrollbar_refresh(g_hOutline);
        RedrawWindow(g_hOutline, NULL, NULL, RDW_FRAME | RDW_INVALIDATE | RDW_ERASE);
    }
    explorer_apply_theme();
}

/* Popup menus need an explicit uxtheme opt-in to follow Tinker Editor's theme.
   The visible menu bar is owner-drawn below because Windows does not repaint
   that non-client area when this preference changes at runtime. These private
   ordinal exports are available from Windows 10 1903 onward. */
typedef enum { UXTHEME_APPMODE_FORCEDARK = 2, UXTHEME_APPMODE_FORCELIGHT = 3 } UxthemeAppMode;
typedef UxthemeAppMode (WINAPI *SetPreferredAppModeFn)(UxthemeAppMode appMode);
typedef BOOL (WINAPI *AllowDarkModeForWindowFn)(HWND hwnd, BOOL allow);
typedef void (WINAPI *FlushMenuThemesFn)(void);
/* uxtheme.dll exports these by ordinal only. */
enum {
    UXTHEME_ALLOW_DARK_MODE_FOR_WINDOW = 133,
    UXTHEME_SET_PREFERRED_APP_MODE = 135,
    UXTHEME_FLUSH_MENU_THEMES = 136,
};

static SetPreferredAppModeFn g_setPreferredAppMode = NULL;
static AllowDarkModeForWindowFn g_allowDarkModeForWindow = NULL;
static FlushMenuThemesFn g_flushMenuThemes = NULL;

static void init_native_menu_theme_support(void) {
    HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
    if (!uxtheme) return;
    FARPROC procedure = GetProcAddress(uxtheme, MAKEINTRESOURCEA(UXTHEME_ALLOW_DARK_MODE_FOR_WINDOW));
    memcpy(&g_allowDarkModeForWindow, &procedure, sizeof(g_allowDarkModeForWindow));
    procedure = GetProcAddress(uxtheme, MAKEINTRESOURCEA(UXTHEME_SET_PREFERRED_APP_MODE));
    memcpy(&g_setPreferredAppMode, &procedure, sizeof(g_setPreferredAppMode));
    procedure = GetProcAddress(uxtheme, MAKEINTRESOURCEA(UXTHEME_FLUSH_MENU_THEMES));
    memcpy(&g_flushMenuThemes, &procedure, sizeof(g_flushMenuThemes));
}

/* DarkMode_Explorer themes the menu bar's fill and its dropdowns, but the
   1px separator the non-client area draws along the menu bar's bottom edge
   stays a light system colour regardless - it isn't part of what that theme
   (or the ODT_MENU owner-draw for the item labels) covers. Painted over
   directly with the same dark fill used elsewhere, right after Windows
   finishes its own non-client painting. Only needed in dark mode; light
   mode's own separator already matches the rest of the light chrome. */
static void erase_menu_bottom_seam(HWND hwnd) {
    if (!g_editorDarkMode) return;
    RECT windowRect;
    if (!GetWindowRect(hwnd, &windowRect)) return;
    POINT clientTopLeft = {0, 0};
    if (!ClientToScreen(hwnd, &clientTopLeft)) return;
    RECT clientRect;
    if (!GetClientRect(hwnd, &clientRect)) return;
    int lineX = clientTopLeft.x - windowRect.left;
    int lineY = clientTopLeft.y - windowRect.top - 1;
    int lineWidth = clientRect.right - clientRect.left;
    if (lineX < 0 || lineY < 0 || lineWidth <= 0) return;
    HDC hdc = GetWindowDC(hwnd);
    if (!hdc) return;
    RECT lineRect = {lineX, lineY, lineX + lineWidth, lineY + 1};
    fill_rect_color(hdc, &lineRect, theme_bg_color());
    ReleaseDC(hwnd, hdc);
}

/* Dark/light title bar and border for any top-level window: the per-window
   uxtheme opt-in, the DWM caption attribute (which DWM ignores until the
   frame is told to repaint, hence the SWP_FRAMECHANGED nudge), and the
   scrollbar/explorer theme. Used for the main window by apply_menu_theme()
   below and on its own for the popup windows (Go to Line, the searchable
   picker, the plain text prompt), which have no menu bar. */
static void apply_popup_titlebar_theme(HWND hwnd, int dark) {
    if (!hwnd) return;
    if (g_allowDarkModeForWindow) g_allowDarkModeForWindow(hwnd, dark ? TRUE : FALSE);
    BOOL useDark = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                      &useDark, sizeof(useDark))))
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1,
                               &useDark, sizeof(useDark));
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

/* Paints the menu bar's background with the current theme brush. */
static void set_menu_background(HMENU menu) {
    if (!g_menuBkBrush) return;
    MENUINFO info = {0};
    info.cbSize = sizeof(info);
    info.fMask = MIM_BACKGROUND;
    info.hbrBack = g_menuBkBrush;
    SetMenuInfo(menu, &info);
}

/* Switches the menu bar, its dropdowns, and the window's title bar/border
   between light and dark chrome. FORCEDARK/FORCELIGHT are intentional: the
   app has its own theme selector, so the selected menu colour must not
   depend on the Windows system theme. The per-window opt-in is what lets
   the non-client menu bar follow that process preference. Safe on older
   Windows releases where the private uxtheme exports (or DWMWA_USE_
   IMMERSIVE_DARK_MODE) are absent; those releases retain their system
   chrome. */
static void apply_menu_theme(HWND hwnd, int dark) {
    if (g_setPreferredAppMode)
        g_setPreferredAppMode(dark ? UXTHEME_APPMODE_FORCEDARK : UXTHEME_APPMODE_FORCELIGHT);
    if (hwnd && g_allowDarkModeForWindow)
        g_allowDarkModeForWindow(hwnd, dark ? TRUE : FALSE);
    if (g_flushMenuThemes)
        g_flushMenuThemes();
    if (hwnd) {
        apply_popup_titlebar_theme(hwnd, dark);

        if (g_menuBkBrush) DeleteObject(g_menuBkBrush);
        g_menuBkBrush = CreateSolidBrush(theme_palette_for(dark)->bg);
        HMENU menu = GetMenu(hwnd);
        if (menu) set_menu_background(menu);
        SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
        DrawMenuBar(hwnd);
        erase_menu_bottom_seam(hwnd);
    }
}

/* Fills one splitter gap's rect with the border colour so it reads as a
   draggable divider instead of just more of the pane background on either
   side of it - shared by every splitter's WM_ERASEBKGND painting (main,
   explorer, split-editor). A zeroed/empty rect (the splitter's own "not
   currently shown" state) is a no-op. */
static void paint_splitter_gap(HDC hdc, const RECT *rect) {
    if (rect->right <= rect->left || rect->bottom <= rect->top) return;
    fill_rect_color(hdc, rect, theme_border_color());
}

/* ---- owner-drawn tabs and listboxes ---- */

static RECT tab_close_rectangle(const RECT *tabRect) {
    RECT close = *tabRect;
    close.left = close.right - 23;
    close.right -= 5;
    close.top += 3;
    close.bottom -= 3;
    return close;
}

static void draw_tab_item(const DRAWITEMSTRUCT *dis) {
    int index = (int)dis->itemID;
    if (index < 0 || index >= g_documentStore.count) return;
    int selected = TabCtrl_GetCurSel(g_hTabs) == index;
    /* The split pane can hold a different document than the main tab
       selection (Show Document in Split...); while it actually has the
       keyboard focus, highlight its tab instead. */
    if (g_splitEditorVisible && g_hSplitEdit && GetFocus() == g_hSplitEdit) {
        int splitIndex = document_index_for_view(g_hSplitEdit);
        if (splitIndex >= 0) selected = splitIndex == index;
    }
    const ThemePalette *p = theme_palette();
    fill_rect_color(dis->hDC, &dis->rcItem, p->tabBg);
    HPEN borderPen = CreatePen(PS_SOLID, 1, theme_border_color());
    HPEN oldPen = (HPEN)SelectObject(dis->hDC, borderPen);
    MoveToEx(dis->hDC, dis->rcItem.right - 1, dis->rcItem.top + 2, NULL);
    LineTo(dis->hDC, dis->rcItem.right - 1, dis->rcItem.bottom - 2);

    RECT textRect = dis->rcItem;
    textRect.left += 9;
    textRect.right -= 33;
    WCHAR label[MAX_PATH + 32];
    format_document_label(&g_documentStore.documents[index], label, sizeof(label) / sizeof(label[0]));
    HFONT font = (HFONT)SendMessageW(g_hTabs, WM_GETFONT, 0, 0);
    HFONT oldFont = font ? (HFONT)SelectObject(dis->hDC, font) : NULL;
    SetTextColor(dis->hDC, p->tabFg);
    SetBkMode(dis->hDC, TRANSPARENT);
    /* Measured without DT_VCENTER first, so the underline below can match
       the label's actual (possibly ellipsized) width instead of guessing. */
    RECT labelRect = textRect;
    DrawTextW(dis->hDC, label, -1, &labelRect,
              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_CALCRECT);
    DrawTextW(dis->hDC, label, -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (oldFont) SelectObject(dis->hDC, oldFont);

    if (selected) {
        int lineHeight = labelRect.bottom - labelRect.top;
        int textTop = textRect.top + ((textRect.bottom - textRect.top) - lineHeight) / 2;
        int lineY = textTop + lineHeight + 2;
        if (lineY > dis->rcItem.bottom - 3) lineY = dis->rcItem.bottom - 3;
        HPEN underlinePen = CreatePen(PS_SOLID, 2, p->accent);
        HPEN oldUnderlinePen = (HPEN)SelectObject(dis->hDC, underlinePen);
        MoveToEx(dis->hDC, labelRect.left, lineY, NULL);
        LineTo(dis->hDC, labelRect.right, lineY);
        SelectObject(dis->hDC, oldUnderlinePen);
        DeleteObject(underlinePen);
    }

    /* tab_close_rectangle() is wider than it is tall (a roomier click
       target), so inset-padding it directly for the X stretched the cross
       into a near-flat bowtie instead of a diagonal. Draw the glyph in a
       square centered on that rect instead, sized to its shorter side, so
       the strokes stay a proper 45-degree X regardless of the hit box's
       aspect ratio. */
    RECT close = tab_close_rectangle(&dis->rcItem);
    int side = (close.right - close.left < close.bottom - close.top)
        ? (close.right - close.left) : (close.bottom - close.top);
    int cx = (close.left + close.right) / 2, cy = (close.top + close.bottom) / 2;
    int pad = 3;
    if (pad * 2 >= side) pad = side / 2 - 1;
    if (pad < 1) pad = 1;
    RECT glyph = { cx - side / 2, cy - side / 2, cx + side / 2, cy + side / 2 };
    int padTop = pad + 1, padBottom = pad;
    HPEN closePen = CreatePen(PS_SOLID, 2, p->tabFg);
    SelectObject(dis->hDC, closePen);
    if (g_documentStore.documents[index].pinned) {
        /* A compact push-pin in the close-button slot. Pinned tabs have no X
           and that hit target is inert until Unpin Tab is chosen. */
        int left = glyph.left + pad, right = glyph.right - pad;
        int top = glyph.top + padTop, bottom = glyph.bottom - padBottom;
        int middle = (left + right) / 2;
        MoveToEx(dis->hDC, left, top, NULL); LineTo(dis->hDC, right, top);
        MoveToEx(dis->hDC, left + 2, top, NULL); LineTo(dis->hDC, left + 2, top + 5);
        MoveToEx(dis->hDC, right - 2, top, NULL); LineTo(dis->hDC, right - 2, top + 5);
        MoveToEx(dis->hDC, left, top + 5, NULL); LineTo(dis->hDC, right, top + 5);
        MoveToEx(dis->hDC, middle, top + 5, NULL); LineTo(dis->hDC, middle, bottom);
    } else {
        MoveToEx(dis->hDC, glyph.left + pad, glyph.top + padTop, NULL);
        LineTo(dis->hDC, glyph.right - pad, glyph.bottom - padBottom);
        MoveToEx(dis->hDC, glyph.right - pad, glyph.top + padTop, NULL);
        LineTo(dis->hDC, glyph.left + pad, glyph.bottom - padBottom);
    }
    SelectObject(dis->hDC, oldPen);
    DeleteObject(closePen);
    DeleteObject(borderPen);

    if (g_documentStore.documents[index].externalChanged) {
        COLORREF changedColor = p->changedMarker;
        HBRUSH changed = CreateSolidBrush(changedColor);
        HPEN changedPen = CreatePen(PS_SOLID, 1, changedColor);
        HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, changed);
        HPEN changedOldPen = (HPEN)SelectObject(dis->hDC, changedPen);
        Ellipse(dis->hDC, close.left - 9, (close.top + close.bottom) / 2 - 3,
                close.left - 3, (close.top + close.bottom) / 2 + 3);
        SelectObject(dis->hDC, changedOldPen);
        SelectObject(dis->hDC, oldBrush);
        DeleteObject(changedPen);
        DeleteObject(changed);
    }
}

/* Shared owner-draw for the app's plain, single-column LISTBOX controls (the
   outline panel and the searchable picker's result list) so both follow the
   app's light/dark theme - a plain LISTBOX always paints in system colors
   regardless of SetWindowTheme. Parameterized by HWND since both listboxes
   need identical drawing. */
static void measure_themed_listbox_item(HWND listbox, MEASUREITEMSTRUCT *mis) {
    HFONT font = (HFONT)SendMessageW(listbox, WM_GETFONT, 0, 0);
    HDC hdc = GetDC(listbox);
    HFONT oldFont = font ? (HFONT)SelectObject(hdc, font) : NULL;
    TEXTMETRICW tm;
    GetTextMetricsW(hdc, &tm);
    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(listbox, hdc);
    mis->itemHeight = (UINT)(tm.tmHeight + tm.tmExternalLeading + MulDiv(6, GetDpiForWindow(g_hMainWnd), 96));
}

/* A listbox row's text, or NULL when it has none. LB_GETTEXT overflows
   whatever buffer it's given rather than truncating to it, so the buffer is
   sized from LB_GETTEXTLEN first - a heading line (or a long file path in
   the picker) can run well past any fixed stack buffer. Caller frees. */
static WCHAR *listbox_item_text(HWND listbox, int item) {
    int textLen = (int)SendMessageW(listbox, LB_GETTEXTLEN, (WPARAM)item, 0);
    if (textLen <= 0) return NULL;
    WCHAR *label = (WCHAR *)malloc(((size_t)textLen + 1) * sizeof(WCHAR));
    if (label && SendMessageW(listbox, LB_GETTEXT, (WPARAM)item, (LPARAM)label) <= 0) {
        free(label);
        label = NULL;
    }
    return label;
}

/* Draws `text` left-aligned in `textRect` in the themed text colour shared
   by every owner-drawn listbox; fill_themed_listbox_row() paints behind it. */
static void draw_themed_listbox_text(const DRAWITEMSTRUCT *dis, const WCHAR *text, RECT textRect) {
    SetTextColor(dis->hDC, theme_fg_color());
    SetBkMode(dis->hDC, TRANSPARENT);
    DrawTextW(dis->hDC, text, -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void fill_themed_listbox_row(const DRAWITEMSTRUCT *dis) {
    int selected = (dis->itemState & ODS_SELECTED) != 0;
    fill_rect_color(dis->hDC, &dis->rcItem, selected ? theme_palette()->selectionBg : theme_bg_color());
}

static void draw_themed_listbox_item(HWND listbox, const DRAWITEMSTRUCT *dis) {
    if ((int)dis->itemID < 0) return;
    fill_themed_listbox_row(dis);
    WCHAR *label = listbox_item_text(listbox, (int)dis->itemID);
    if (!label) return;
    RECT textRect = dis->rcItem;
    textRect.left += 4;
    draw_themed_listbox_text(dis, label, textRect);
    free(label);
}
