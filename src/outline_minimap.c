/*
 * outline_minimap.c - the document outline pane and the minimap overview.
 *
 * The outline listbox is rebuilt from the active Scintilla document's ATX
 * headings. The minimap uses a throttled snapshot of that document and
 * mirrors the main editor's scroll position without joining its live edit
 * notifications. Both are created by tinkeredit.c, so this module drives them
 * through the OutlineMinimapHost table handed in by outline_minimap_init()
 * rather than reaching into tinkeredit.c's translation-unit statics by name.
 */

#include "outline_minimap.h"

#include <commctrl.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windowsx.h>

#include "editor_commands.h" /* MARGIN_LINENUMBERS/BOOKMARKS/FOLD */
#include "editor_language.h"
#include "settings_io.h"
#include "settings_values.h"
#include "text_codec.h"
#include "theme.h"

/* Vertical padding above/below each minimap line, in pixels. Negative pulls
   the lines closer together than Scintilla's natural spacing. */
#define MINIMAP_EXTRA_ASCENT (-2)
#define MINIMAP_EXTRA_DESCENT (-3)

static const OutlineMinimapHost *g_host = NULL;
static int g_minimapPaintDeferred = 0;

void outline_minimap_init(const OutlineMinimapHost *host) {
    g_host = host;
}

/* ---- document outline -------------------------------------------------- */

int outline_parse_atx_heading(const char *line, int length, int *textStart, int *textLength) {
    int i = 0;
    while (i < length && i < 3 && line[i] == ' ') i++;
    int hashes = 0;
    while (i + hashes < length && hashes < 6 && line[i + hashes] == '#') hashes++;
    if (!hashes || i + hashes >= length ||
        (line[i + hashes] != ' ' && line[i + hashes] != '\t')) return 0;
    int start = i + hashes;
    while (start < length && (line[start] == ' ' || line[start] == '\t')) start++;
    int end = length;
    while (end > start && (line[end - 1] == '\r' || line[end - 1] == '\n' ||
                            line[end - 1] == ' ' || line[end - 1] == '\t')) end--;
    int marker = end;
    while (marker > start && line[marker - 1] == '#') marker--;
    if (marker < end && marker > start && (line[marker - 1] == ' ' || line[marker - 1] == '\t')) {
        end = marker - 1;
        while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) end--;
    }
    *textStart = start;
    *textLength = end - start;
    return hashes;
}

/* The run length of a code fence - up to 3 spaces, then 3 or more backticks
   or tildes - or 0. *bare says whether only whitespace follows the run, as
   a closing fence requires. */
static int code_fence_run(const char *line, int length, char *fence, int *bare) {
    int i = 0;
    while (i < length && i < 3 && line[i] == ' ') i++;
    if (i >= length || (line[i] != '`' && line[i] != '~')) return 0;
    char marker = line[i];
    int run = 0;
    while (i + run < length && line[i + run] == marker) run++;
    if (run < 3) return 0;
    int rest = i + run;
    while (rest < length && (line[rest] == ' ' || line[rest] == '\t')) rest++;
    *fence = marker;
    *bare = rest == length;
    return run;
}

int outline_scan_line(OutlineScan *scan, const char *line, int length, int *textStart, int *textLength) {
    char marker = 0;
    int bare = 0;
    int run = code_fence_run(line, length, &marker, &bare);
    if (scan->fence) {
        if (run >= scan->fenceLength && marker == scan->fence && bare) scan->fence = 0;
        return 0;
    }
    if (run) {
        scan->fence = marker;
        scan->fenceLength = run;
        return 0;
    }
    return outline_parse_atx_heading(line, length, textStart, textLength);
}

/* Adds one heading's row: its title, indented two spaces per level. */
static void add_outline_heading(const char *title, int titleLength, int level, Sci_Position lineNumber) {
    WCHAR *wide = to_wide_n(title, (size_t)titleLength, NULL);
    if (!wide) return;
    int indent = (level - 1) * 2;
    size_t capacity = wcslen(wide) + (size_t)indent + 1;
    WCHAR *label = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (label) {
        for (int i = 0; i < indent; i++) label[i] = L' ';
        wcscpy(label + indent, wide);
        SendMessageW(*g_host->outline, LB_ADDSTRING, 0, (LPARAM)label);
        g_host->outlineLines[(*g_host->outlineCount)++] = lineNumber;
        free(label);
    }
    free(wide);
}

/* Reads the text rather than asking the lexer which lines are headings:
   Scintilla styles lazily, so lines it has not displayed could be missed. */
void update_outline(void) {
    if (!(*g_host->outline)) return;
    SendMessageW(*g_host->outline, LB_RESETCONTENT, 0, 0);
    *g_host->outlineCount = 0;
    if (g_host->largeFile()) return;
    /* The list is only read back by clicks on it, and toggle_outline()
       rebuilds it when the pane is shown, so a hidden pane skips the scan. */
    if (!g_outlineVisible || *g_host->activeLanguage != LANG_MARKDOWN) return;
    HWND edit = *g_host->edit;
    Sci_Position length = (Sci_Position)SendMessageA(edit, SCI_GETLENGTH, 0, 0);
    char *text = (char *)malloc((size_t)length + 1);
    if (!text) return;
    SendMessageA(edit, SCI_GETTEXT, (WPARAM)(length + 1), (LPARAM)text);
    /* Suspend repainting so re-adding every heading doesn't flicker. */
    SendMessageW(*g_host->outline, WM_SETREDRAW, FALSE, 0);
    OutlineScan scan = {0};
    /* Walk the copy directly. Asking Scintilla for both endpoints of every
       line would pay two window-message round trips per source line, while
       GETCHARACTERPOINTER would close the gap buffer and slow the next edit. */
    Sci_Position position = 0;
    Sci_Position lineNumber = 0;
    while (position < length && *g_host->outlineCount < OUTLINE_MAX_HEADINGS) {
        Sci_Position start = position;
        while (position < length && text[position] != '\r' && text[position] != '\n') position++;
        Sci_Position lineLength = position - start;
        if (lineLength > 65535) lineLength = 65535;
        int titleStart = 0, titleLength = 0;
        int level = outline_scan_line(&scan, text + start, (int)lineLength, &titleStart, &titleLength);
        if (level && titleLength > 0)
            add_outline_heading(text + start + titleStart, titleLength, level, lineNumber);
        if (position < length && text[position] == '\r') position++;
        if (position < length && text[position] == '\n') position++;
        lineNumber++;
    }
    free(text);
    SendMessageW(*g_host->outline, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(*g_host->outline, NULL, TRUE);
}

void toggle_outline(HWND hwnd) {
    g_outlineVisible = !g_outlineVisible;
    update_outline();
    g_host->relayoutAndSave(hwnd);
}

void toggle_outline_side(HWND hwnd) {
    g_outlineOnLeft = !g_outlineOnLeft;
    g_host->relayoutAndSave(hwnd);
}

/* One-time, document-independent setup for the minimap's Scintilla view:
   read-only, no margins or scrollbars of its own, unwrapped so each document
   line maps to exactly one minimap line, and no visible caret. Font, theme,
   and zoom are applied separately (see apply_editor_font()) since those also
   need to be reapplied whenever the editor's font or theme changes. */
void configure_minimap(HWND hMinimap) {
    SendMessageA(hMinimap, SCI_SETCODEPAGE, SC_CP_UTF8, 0);
    SendMessageA(hMinimap, SCI_SETREADONLY, TRUE, 0);
    SendMessageA(hMinimap, SCI_SETUNDOCOLLECTION, FALSE, 0);
    SendMessageA(hMinimap, SCI_SETVSCROLLBAR, FALSE, 0);
    SendMessageA(hMinimap, SCI_SETHSCROLLBAR, FALSE, 0);
    SendMessageA(hMinimap, SCI_SETWRAPMODE, SC_WRAP_NONE, 0);
    SendMessageA(hMinimap, SCI_SETMARGINWIDTHN, MARGIN_LINENUMBERS, 0);
    SendMessageA(hMinimap, SCI_SETMARGINWIDTHN, MARGIN_CHANGES, 0);
    SendMessageA(hMinimap, SCI_SETMARGINWIDTHN, MARGIN_BOOKMARKS, 0);
    SendMessageA(hMinimap, SCI_SETMARGINWIDTHN, MARGIN_FOLD, 0);
    SendMessageA(hMinimap, SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE, 0);
    SendMessageA(hMinimap, SCI_SETCARETLINEVISIBLE, FALSE, 0);
    SendMessageA(hMinimap, SCI_SETMOUSEDOWNCAPTURES, FALSE, 0);
    SendMessageA(hMinimap, SCI_SETCURSOR, SC_CURSORARROW, 0);
    SendMessageA(hMinimap, SCI_SETZOOM, 0, 0); /* sizing is done via STYLESETSIZEFRACTIONAL, see apply_editor_font() */
    SendMessageA(hMinimap, SCI_SETEXTRAASCENT, (WPARAM)MINIMAP_EXTRA_ASCENT, 0);
    SendMessageA(hMinimap, SCI_SETEXTRADESCENT, (WPARAM)MINIMAP_EXTRA_DESCENT, 0);
}

/* Recomputes the minimap's own scroll position (for documents too long to
   fit the minimap at once) and the visible-region box drawn over it, then
   asks for a repaint. Called whenever the main editor scrolls, its content
   changes size, or the minimap is resized/shown. */
void update_minimap_viewport(void) {
    if (!(*g_host->minimap) || !(*g_host->edit) || !g_minimapVisible || !IsWindowVisible(*g_host->minimap)) return;
    HWND view = g_host->trackedEditView();

    Sci_Position totalLines = (Sci_Position)SendMessageA(view, SCI_GETLINECOUNT, 0, 0);
    Sci_Position mainFirstVisible = (Sci_Position)SendMessageA(view, SCI_GETFIRSTVISIBLELINE, 0, 0);
    Sci_Position mainFirstDocLine = (Sci_Position)SendMessageA(view, SCI_DOCLINEFROMVISIBLE, (WPARAM)mainFirstVisible, 0);
    Sci_Position mainLinesOnScreen = (Sci_Position)SendMessageA(view, SCI_LINESONSCREEN, 0, 0);

    int minimapLineHeight = (int)SendMessageA(*g_host->minimap, SCI_TEXTHEIGHT, 0, 0);
    if (minimapLineHeight < 1) minimapLineHeight = 1;
    RECT mmRect;
    GetClientRect(*g_host->minimap, &mmRect);
    int minimapLinesOnScreen = (mmRect.bottom - mmRect.top) / minimapLineHeight;
    if (minimapLinesOnScreen < 1) minimapLinesOnScreen = 1;

    /* Documents that fit entirely within the minimap's height stay pinned at
       the top; longer ones scroll to keep the main editor's viewport roughly
       centered within the minimap. */
    Sci_Position minimapFirstDocLine = 0;
    if (totalLines > minimapLinesOnScreen) {
        minimapFirstDocLine = mainFirstDocLine - (minimapLinesOnScreen - mainLinesOnScreen) / 2;
        Sci_Position maxFirst = totalLines - minimapLinesOnScreen;
        if (minimapFirstDocLine > maxFirst) minimapFirstDocLine = maxFirst;
        if (minimapFirstDocLine < 0) minimapFirstDocLine = 0;
    }
    Sci_Position minimapFirstVisible = (Sci_Position)SendMessageA(*g_host->minimap, SCI_VISIBLEFROMDOCLINE, (WPARAM)minimapFirstDocLine, 0);
    Sci_Position currentMinimapFirstVisible = (Sci_Position)SendMessageA(
        *g_host->minimap, SCI_GETFIRSTVISIBLELINE, 0, 0);
    /* Scintilla repaints after SCI_SETFIRSTVISIBLELINE even if the value did
       not change. The repaint can produce another shared-document content
       update, so only send the scroll command when the minimap really moved. */
    if (minimapFirstVisible != currentMinimapFirstVisible)
        SendMessageA(*g_host->minimap, SCI_SETFIRSTVISIBLELINE, (WPARAM)minimapFirstVisible, 0);

    /* Pixels within the minimap: the clamping above keeps the main view's
       first line within one minimap height of the minimap's own. */
    int newViewTop = (int)(mainFirstDocLine - minimapFirstDocLine) * minimapLineHeight;
    int newViewHeight = (int)mainLinesOnScreen * minimapLineHeight;
    if (newViewHeight < 4) newViewHeight = 4;

    /* Only repaint the visible-region box when it moved or resized. During
       typing the shell has redraw suspended, so this invalidation is folded
       into the single deferred minimap refresh. */
    if (newViewTop == *g_host->minimapViewTop && newViewHeight == *g_host->minimapViewHeight) return;
    *g_host->minimapViewTop = newViewTop;
    *g_host->minimapViewHeight = newViewHeight;
    InvalidateRect(*g_host->minimap, NULL, FALSE);
}

void defer_minimap_paint(void) {
    g_minimapPaintDeferred = 1;
}

void refresh_minimap_after_edit(void) {
    if (!(*g_host->minimap) || !g_minimapVisible ||
        !IsWindowVisible(*g_host->minimap)) return;

    HWND edit = *g_host->edit;
    HWND minimap = *g_host->minimap;
    Sci_Position length = (Sci_Position)SendMessageA(edit, SCI_GETLENGTH, 0, 0);
    char *text = (char *)malloc((size_t)length + 1);
    if (!text) return;
    SendMessageA(edit, SCI_GETTEXT, (WPARAM)(length + 1), (LPARAM)text);

    /* The minimap's independent document is the key edit-path optimisation:
       replacing this snapshot here means Backspace never synchronously makes
       the minimap process the live document change. Redraw remains disabled
       until the complete snapshot and its styling are ready. */
    SendMessageA(minimap, WM_SETREDRAW, FALSE, 0);
    SendMessageA(minimap, SCI_SETREADONLY, FALSE, 0);
    SendMessageA(minimap, SCI_SETTEXT, 0, (LPARAM)text);
    SendMessageA(minimap, SCI_SETREADONLY, TRUE, 0);
    if (!g_host->largeFile()) SendMessageA(minimap, SCI_COLOURISE, 0, (LPARAM)-1);
    SendMessageA(minimap, WM_SETREDRAW, TRUE, 0);
    free(text);

    g_minimapPaintDeferred = 0;
    update_minimap_viewport();
    InvalidateRect(*g_host->minimap, NULL, FALSE);
}

/* Draws the viewport shade and frame onto the same surface as the minimap
   text. The caller presents that completed surface in one BitBlt, avoiding
   the visible text-then-overlay sequence of two direct screen paints. */
static void paint_minimap_viewport(HDC hdc, const RECT *clientRect) {
    RECT box = {
        clientRect->left,
        *g_host->minimapViewTop,
        clientRect->right,
        *g_host->minimapViewTop + *g_host->minimapViewHeight
    };
    if (box.top < clientRect->top) box.top = clientRect->top;
    if (box.bottom > clientRect->bottom) box.bottom = clientRect->bottom;
    if (box.bottom <= box.top) return;

    HBRUSH brush = CreateSolidBrush(theme_palette()->minimapFrame);
    HDC shadeDc = CreateCompatibleDC(hdc);
    HBITMAP shadeBmp = CreateCompatibleBitmap(hdc, 1, 1);
    if (brush && shadeDc && shadeBmp) {
        HGDIOBJ oldBmp = SelectObject(shadeDc, shadeBmp);
        SetPixel(shadeDc, 0, 0, theme_palette()->minimapFrame);
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, THEME_MINIMAP_SHADE_ALPHA, 0 };
        AlphaBlend(hdc, box.left, box.top, box.right - box.left, box.bottom - box.top,
                   shadeDc, 0, 0, 1, 1, blend);
        SelectObject(shadeDc, oldBmp);
        FrameRect(hdc, &box, brush);
    }
    if (shadeBmp) DeleteObject(shadeBmp);
    if (shadeDc) DeleteDC(shadeDc);
    if (brush) DeleteObject(brush);
}

/* Scrolls the main editor so the document line under minimap-local y is
   centered in its viewport - used for both a single click and drag-to-scroll. */
static void minimap_scroll_main_to_y(int y) {
    if (!(*g_host->edit) || !(*g_host->minimap)) return;
    HWND view = g_host->trackedEditView();
    Sci_Position pos = (Sci_Position)SendMessageA(*g_host->minimap, SCI_POSITIONFROMPOINT, (WPARAM)0, (LPARAM)y);
    if (pos < 0) return;
    Sci_Position docLine = (Sci_Position)SendMessageA(*g_host->minimap, SCI_LINEFROMPOSITION, (WPARAM)pos, 0);
    Sci_Position mainLinesOnScreen = (Sci_Position)SendMessageA(view, SCI_LINESONSCREEN, 0, 0);
    Sci_Position target = docLine - mainLinesOnScreen / 2;
    if (target < 0) target = 0;
    Sci_Position targetVisible = (Sci_Position)SendMessageA(view, SCI_VISIBLEFROMDOCLINE, (WPARAM)target, 0);
    SendMessageA(view, SCI_SETFIRSTVISIBLELINE, (WPARAM)targetVisible, 0);
}

LRESULT CALLBACK MinimapSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR id, DWORD_PTR data) {
    (void)id; (void)data;
    switch (msg) {
        case WM_LBUTTONDOWN:
            *g_host->minimapDragging = 1;
            SetCapture(hwnd);
            minimap_scroll_main_to_y(GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEMOVE:
            if (*g_host->minimapDragging) minimap_scroll_main_to_y(GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONUP:
            if (*g_host->minimapDragging) { *g_host->minimapDragging = 0; ReleaseCapture(); }
            return 0;
        case WM_CAPTURECHANGED:
            *g_host->minimapDragging = 0;
            return 0;
        /* Swallowed rather than forwarded to Scintilla's default handling,
           which would otherwise open its own context menu or place a caret
           in what is meant to be a read-only overview. */
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
        case WM_LBUTTONDBLCLK:
            return 0;
        case WM_MOUSEWHEEL:
            if (*g_host->edit) SendMessageA(g_host->trackedEditView(), WM_MOUSEWHEEL, wParam, lParam);
            return 0;
        case WM_PAINT: {
            if (g_minimapPaintDeferred) {
                /* Scintilla invalidates every view of a shared document on
                   each edit. Preserve the current minimap pixels until the
                   throttled refresh instead of painting every character. */
                ValidateRect(hwnd, NULL);
                return 0;
            }
            PAINTSTRUCT paint;
            HDC paintDc = BeginPaint(hwnd, &paint);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;
            HDC bufferDc = width > 0 && height > 0 ? CreateCompatibleDC(paintDc) : NULL;
            HBITMAP bufferBmp = bufferDc ? CreateCompatibleBitmap(paintDc, width, height) : NULL;
            HGDIOBJ oldBmp = bufferBmp ? SelectObject(bufferDc, bufferBmp) : NULL;
            HDC drawDc = bufferBmp ? bufferDc : paintDc;

            /* Scintilla's WM_PRINTCLIENT paints its complete client into the
               supplied compatible DC without presenting an intermediate
               frame to the screen. */
            DefSubclassProc(hwnd, WM_PRINTCLIENT, (WPARAM)drawDc, PRF_CLIENT);
            paint_minimap_viewport(drawDc, &rc);
            if (bufferBmp) {
                BitBlt(paintDc, rc.left, rc.top, width, height,
                       bufferDc, rc.left, rc.top, SRCCOPY);
                SelectObject(bufferDc, oldBmp);
                DeleteObject(bufferBmp);
            }
            if (bufferDc) DeleteDC(bufferDc);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, MinimapSubclassProc, 1);
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void toggle_minimap(HWND hwnd) {
    g_minimapVisible = !g_minimapVisible;
    g_host->relayoutAndSave(hwnd);
    if (g_minimapVisible) refresh_minimap_after_edit();
}

void toggle_split_editor(HWND hwnd, int vertical) {
    int sameSplit = g_splitEditorVisible && g_splitEditorVertical == vertical;
    if (sameSplit) {
        g_splitEditorVisible = 0;
        *g_host->lastEditView = *g_host->edit;
    } else {
        if (g_splitEditorVertical != vertical) *g_host->splitEditorPos = -1;
        g_splitEditorVertical = vertical;
        g_splitEditorVisible = 1;
    }
    g_host->syncSplitDocument();
    g_host->relayoutAndSave(hwnd);
}

/* Puts the split-editor divider back at an even 50/50 on either axis. */
void reset_split_editor(HWND hwnd) {
    g_splitEditorRatio = 500;
    *g_host->splitEditorPos = -1;
    g_host->layout(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
    save_layout_settings();
}
