#include "terminal_view.h"

#include <commctrl.h> /* SetWindowSubclass */
#include <windowsx.h> /* GET_X_LPARAM/GET_Y_LPARAM */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terminal_grid.h"
#include "terminal_input.h"
#include "terminal_process.h"

/* Private message the process's reader thread posts chunks with; scoped to
   one control's own window, unlike output_panel.inc's shared WM_APP_OUTPUT. */
#define WM_TERMINAL_CHUNK (WM_APP + 100)

struct TerminalView {
    HWND hwnd;
    TerminalProcess *process;
    TerminalGrid *grid;
    TerminalGrid *savedGrid; /* live shell while a Lua transcript is shown */
    int session;
    int running;
    int sessionMode;
    int sessionTranscript;
    int inputPending;
    int inputDone;
    int inputInterrupted;
    char *inputText;
    size_t inputLength;
    size_t inputCursor;
    size_t inputCapacity;
    int inputDisplayCharacters;
    int inputDisplayCursorCharacters;
    int inputDisplayCapacity;
    char *inputResult;
    char *inputHistory[50];
    int inputHistoryCount;
    int inputHistoryIndex;
    char *inputDraft;
    DWORD exitCode;

    int cols, rows;
    int cellWidth, cellHeight;
    HFONT font;
    TerminalPalette palette;

    /* Cached back buffer; output repaints reuse it instead of recreating a
       compatible bitmap and memory DC for every WM_PAINT. */
    HDC backDc;
    HBITMAP backBitmap;
    HGDIOBJ backPreviousBitmap;
    int backWidth, backHeight;

    int scrollOffset; /* lines scrolled up from the live bottom; 0 = pinned to it */
    int wheelDeltaRemainder; /* preserves partial high-resolution wheel turns */

    /* a plain, non-rectangular (stream) selection, in total-line coordinates */
    int selecting;
    int selecting_any;
    int anchorLine, anchorCol, pointLine, pointCol;

    int suppressTranslatedChar; /* set while the current pretranslated key is dispatched */
};

static int next_session = 1;
#define TERMINAL_SESSION_INPUT_MAX 4096
static void clear_selection(TerminalView *view);

static void restore_shell_grid(TerminalView *view) {
    if (view->savedGrid) {
        terminal_grid_free(view->grid);
        view->grid = view->savedGrid;
        view->savedGrid = NULL;
    }
    view->sessionTranscript = 0;
    view->scrollOffset = 0;
    clear_selection(view);
    InvalidateRect(view->hwnd, NULL, FALSE);
}

/* ---- layout ---- */

static void measure_font(TerminalView *view) {
    HDC hdc = GetDC(view->hwnd);
    HFONT previous = (HFONT)SelectObject(hdc, view->font);
    TEXTMETRICW metrics;
    GetTextMetricsW(hdc, &metrics);
    SIZE size;
    GetTextExtentPoint32W(hdc, L"M", 1, &size);
    view->cellWidth = size.cx > 0 ? size.cx : 1;
    view->cellHeight = metrics.tmHeight > 0 ? metrics.tmHeight : 1;
    SelectObject(hdc, previous);
    ReleaseDC(view->hwnd, hdc);
}

static void resize_to_client(TerminalView *view) {
    RECT client;
    GetClientRect(view->hwnd, &client);
    int cols = (client.right - client.left) / view->cellWidth;
    int rows = (client.bottom - client.top) / view->cellHeight;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols == view->cols && rows == view->rows) return;
    view->cols = cols;
    view->rows = rows;
    terminal_grid_resize(view->grid, cols, rows);
    if (view->savedGrid) terminal_grid_resize(view->savedGrid, cols, rows);
    terminal_process_resize(view->process, cols, rows);
    view->scrollOffset = 0;
}

static int max_scroll_offset(const TerminalView *view) {
    return terminal_grid_scrollback_count(view->grid);
}

/* Positive lines move into older scrollback; negative lines move back toward
   the live tail. Keeping this direction explicit avoids confusing wheel-up's
   positive Windows delta with screen coordinates, which grow downward. */
static void scroll_view(TerminalView *view, long long lines) {
    long long offset = (long long)view->scrollOffset + lines;
    int maximum = max_scroll_offset(view);
    if (offset < 0) offset = 0;
    if (offset > maximum) offset = maximum;
    if ((int)offset == view->scrollOffset) return;
    view->scrollOffset = (int)offset;
    InvalidateRect(view->hwnd, NULL, FALSE);
}

/* ---- selection ---- */

static void clear_selection(TerminalView *view) {
    view->selecting_any = 0;
    view->selecting = 0;
}

static void normalized_selection(const TerminalView *view, int *startLine, int *startCol,
                                 int *endLine, int *endCol) {
    if (view->anchorLine < view->pointLine ||
        (view->anchorLine == view->pointLine && view->anchorCol <= view->pointCol)) {
        *startLine = view->anchorLine; *startCol = view->anchorCol;
        *endLine = view->pointLine; *endCol = view->pointCol;
    } else {
        *startLine = view->pointLine; *startCol = view->pointCol;
        *endLine = view->anchorLine; *endCol = view->anchorCol;
    }
}

/* The total-line index and column under a client point, clamped to the grid. */
static void point_to_cell(const TerminalView *view, int x, int y, int *lineIndex, int *col) {
    int totalLines = terminal_grid_total_lines(view->grid);
    int firstLine = totalLines - view->rows - view->scrollOffset;
    if (firstLine < 0) firstLine = 0;
    int row = y / view->cellHeight;
    if (row < 0) row = 0;
    if (row >= view->rows) row = view->rows - 1;
    *lineIndex = firstLine + row;
    if (*lineIndex >= totalLines) *lineIndex = totalLines - 1;
    int c = x / view->cellWidth;
    if (c < 0) c = 0;
    *col = c;
}

static void copy_selection(TerminalView *view) {
    if (!view->selecting_any) return;
    int startLine, startCol, endLine, endCol;
    normalized_selection(view, &startLine, &startCol, &endLine, &endCol);
    size_t capacity = 256;
    WCHAR *text = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!text) return;
    size_t used = 0;
    for (int line = startLine; line <= endLine; line++) {
        int width = 0;
        const TerminalCell *cells = terminal_grid_line_at(view->grid, line, &width);
        int from = line == startLine ? startCol : 0;
        int to = line == endLine ? endCol : width - 1;
        for (int c = from; cells && c <= to && c < width; c++) {
            if (used + 2 >= capacity) {
                capacity *= 2;
                WCHAR *grown = (WCHAR *)realloc(text, capacity * sizeof(WCHAR));
                if (!grown) { free(text); return; }
                text = grown;
            }
            text[used++] = cells[c].ch;
        }
        if (line != endLine) {
            if (used + 2 >= capacity) {
                capacity *= 2;
                WCHAR *grown = (WCHAR *)realloc(text, capacity * sizeof(WCHAR));
                if (!grown) { free(text); return; }
                text = grown;
            }
            text[used++] = L'\r';
            text[used++] = L'\n';
        }
    }
    text[used] = 0;
    if (OpenClipboard(view->hwnd)) {
        EmptyClipboard();
        HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, (used + 1) * sizeof(WCHAR));
        if (block) {
            WCHAR *dest = (WCHAR *)GlobalLock(block);
            if (dest) { memcpy(dest, text, (used + 1) * sizeof(WCHAR)); GlobalUnlock(block); }
            SetClipboardData(CF_UNICODETEXT, block);
        }
        CloseClipboard();
    }
    free(text);
}

static int append_session_input(TerminalView *view, const char *bytes, size_t length);

/* A single trailing line break (common when copying a whole line) doesn't
   make this a multi-line paste; a break with more text after it does. */
static int clipboard_is_multiline(const char *text, size_t length) {
    while (length && (text[length - 1] == '\n' || text[length - 1] == '\r')) length--;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '\n' || text[i] == '\r') return 1;
    }
    return 0;
}

static void paste_clipboard(TerminalView *view) {
    if ((!view->running && !(view->sessionMode && view->inputPending)) ||
        !OpenClipboard(view->hwnd)) return;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data) {
        const WCHAR *wide = (const WCHAR *)GlobalLock(data);
        if (wide) {
            int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
            if (utf8Length > 1) {
                char *utf8 = (char *)malloc((size_t)utf8Length);
                if (utf8) {
                    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, utf8Length, NULL, NULL);
                    size_t length = (size_t)utf8Length - 1;
                    if (view->sessionMode) {
                        size_t lineLength = 0;
                        while (lineLength < length && utf8[lineLength] != '\r' &&
                               utf8[lineLength] != '\n') lineLength++;
                        append_session_input(view, utf8, lineLength);
                    } else if (!clipboard_is_multiline(utf8, length) ||
                               MessageBoxW(view->hwnd,
                                   L"The clipboard has more than one line. Pasting into the "
                                   L"terminal runs each line as a command. Continue?",
                                   L"Tinker Editor", MB_YESNO | MB_ICONWARNING) == IDYES) {
                        terminal_process_write(view->process, utf8, length);
                    }
                    free(utf8);
                }
            }
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
}

/* ---- painting ---- */

/* Keeps one compatible bitmap and memory DC for the life of the window,
   reallocating only when the client size changes. The shell repaints on
   every output chunk, so creating them per WM_PAINT was measurable churn. */
static int ensure_back_buffer(TerminalView *view, HDC hdc, int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    if (view->backBitmap && view->backWidth == width && view->backHeight == height) return 1;
    if (!view->backDc) {
        view->backDc = CreateCompatibleDC(hdc);
        if (!view->backDc) return 0;
    }
    if (view->backBitmap) {
        SelectObject(view->backDc, view->backPreviousBitmap);
        DeleteObject(view->backBitmap);
        view->backBitmap = NULL;
    }
    view->backBitmap = CreateCompatibleBitmap(hdc, width, height);
    if (!view->backBitmap) return 0;
    view->backPreviousBitmap = SelectObject(view->backDc, view->backBitmap);
    view->backWidth = width;
    view->backHeight = height;
    return 1;
}

static void release_back_buffer(TerminalView *view) {
    if (view->backBitmap) {
        SelectObject(view->backDc, view->backPreviousBitmap);
        DeleteObject(view->backBitmap);
        view->backBitmap = NULL;
    }
    if (view->backDc) {
        DeleteDC(view->backDc);
        view->backDc = NULL;
    }
}

static void paint_view(TerminalView *view, HDC hdc, const RECT *client) {
    if (!ensure_back_buffer(view, hdc, client->right, client->bottom)) return;
    HDC memDC = view->backDc;
    HFONT previousFont = (HFONT)SelectObject(memDC, view->font);
    SetBkMode(memDC, OPAQUE);

    int totalLines = terminal_grid_total_lines(view->grid);
    int firstLine = totalLines - view->rows - view->scrollOffset;
    if (firstLine < 0) firstLine = 0;
    int cursorCol = 0, cursorRow = 0, cursorVisible = 0;
    terminal_grid_cursor(view->grid, &cursorCol, &cursorRow, &cursorVisible);
    int startLine = 0, startCol = 0, endLine = 0, endCol = 0;
    if (view->selecting_any)
        normalized_selection(view, &startLine, &startCol, &endLine, &endCol);

    for (int screenRow = 0; screenRow < view->rows; screenRow++) {
        int lineIndex = firstLine + screenRow;
        int width = 0;
        const TerminalCell *cells = lineIndex < totalLines
            ? terminal_grid_line_at(view->grid, lineIndex, &width) : NULL;
        int y = screenRow * view->cellHeight;
        if (!cells || width <= 0) {
            RECT blankRect = { 0, y, client->right, y + view->cellHeight };
            SetBkColor(memDC, view->palette.defaultBg);
            ExtTextOutW(memDC, 0, y, ETO_OPAQUE, &blankRect, L"", 0, NULL);
            continue;
        }
        int x = 0;
        int runStart = 0;
        int isCursorRow = view->scrollOffset == 0 && cursorVisible && lineIndex == firstLine + cursorRow;
        int firstSelected = 0, lastSelected = -1;
        if (view->selecting_any && lineIndex >= startLine && lineIndex <= endLine) {
            firstSelected = lineIndex == startLine ? startCol : 0;
            lastSelected = lineIndex == endLine ? endCol : INT_MAX;
        }

        /* Stop at the line's own width: a scrollback row can be narrower than
           the current terminal, and reading past it was out of bounds. */
        for (int c = 0; c <= view->cols && c <= width; c++) {
            int atRunEnd = c == view->cols || c == width;
            int breaksRun = atRunEnd;
            if (!atRunEnd && c > runStart) {
                int selected = c >= firstSelected && c <= lastSelected;
                int previousSelected = c - 1 >= firstSelected && c - 1 <= lastSelected;
                int sameAsPrev = cells[c].fg == cells[c - 1].fg && cells[c].bg == cells[c - 1].bg &&
                                 cells[c].underline == cells[c - 1].underline &&
                                 selected == previousSelected &&
                                 (!isCursorRow || (c != cursorCol && c - 1 != cursorCol));
                breaksRun = !sameAsPrev;
            }
            if (breaksRun && c > runStart) {
                int runLength = c - runStart;
                WCHAR text[512];
                int textLength = runLength < 512 ? runLength : 511;
                for (int i = 0; i < textLength; i++) text[i] = cells[runStart + i].ch;
                int runSelected = runStart >= firstSelected && runStart <= lastSelected;
                int isCursorCell = isCursorRow && runStart == cursorCol && runLength == 1;
                COLORREF fg = terminal_grid_resolve_colour(cells[runStart].fg, 0, &view->palette);
                COLORREF bg = terminal_grid_resolve_colour(cells[runStart].bg, 1, &view->palette);
                if (runSelected || isCursorCell) { COLORREF t = fg; fg = bg; bg = t; }
                SetTextColor(memDC, fg);
                SetBkColor(memDC, bg);
                RECT runRect = { x, y, x + runLength * view->cellWidth, y + view->cellHeight };
                ExtTextOutW(memDC, x, y, ETO_OPAQUE, &runRect, text, (UINT)textLength, NULL);
                if (cells[runStart].underline) {
                    HPEN pen = CreatePen(PS_SOLID, 1, fg);
                    HPEN previousPen = (HPEN)SelectObject(memDC, pen);
                    int lineY = y + view->cellHeight - 1;
                    MoveToEx(memDC, x, lineY, NULL);
                    LineTo(memDC, x + runLength * view->cellWidth, lineY);
                    SelectObject(memDC, previousPen);
                    DeleteObject(pen);
                }
                x += runLength * view->cellWidth;
                runStart = c;
            }
        }
        if (x < client->right) {
            RECT tailRect = { x, y, client->right, y + view->cellHeight };
            SetBkColor(memDC, view->palette.defaultBg);
            ExtTextOutW(memDC, x, y, ETO_OPAQUE, &tailRect, L"", 0, NULL);
        }
    }
    int paintedBottom = view->rows * view->cellHeight;
    if (paintedBottom < client->bottom) {
        RECT fillRect = { 0, paintedBottom, client->right, client->bottom };
        SetBkColor(memDC, view->palette.defaultBg);
        ExtTextOutW(memDC, 0, paintedBottom, ETO_OPAQUE, &fillRect, L"", 0, NULL);
    }

    BitBlt(hdc, 0, 0, client->right, client->bottom, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, previousFont);
}

/* ---- keyboard ---- */

static void send_bytes(TerminalView *view, const char *bytes, size_t length) {
    if (view->running) terminal_process_write(view->process, bytes, length);
}

/* Windows may fold held keys into one message with a repeat count. Queue
   those repetitions in compact batches rather than taking the input lock
   once per character or escape sequence. */
static void send_repeated_bytes(TerminalView *view, const char *bytes, size_t length,
                                WORD repeatCount) {
    size_t remaining = repeatCount ? repeatCount : 1;
    char batch[256];
    size_t perBatch = length ? sizeof(batch) / length : 0;
    if (!perBatch) {
        while (remaining--) send_bytes(view, bytes, length);
        return;
    }
    for (size_t i = 0; i < perBatch; i++) memcpy(batch + i * length, bytes, length);
    while (remaining) {
        size_t count = remaining < perBatch ? remaining : perBatch;
        send_bytes(view, batch, count * length);
        remaining -= count;
    }
}

static unsigned int key_modifiers(void) {
    unsigned int modifiers = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= TERMINAL_MOD_SHIFT;
    if (GetKeyState(VK_MENU) & 0x8000) modifiers |= TERMINAL_MOD_ALT;
    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= TERMINAL_MOD_CONTROL;
    return modifiers;
}

/* Virtual keys that need VT sequences instead of their WM_CHAR translation.
   Printable keys and control characters still use TranslateMessage(), which
   preserves the active keyboard layout and dead-key composition.

   repeatCount is WM_KEYDOWN's lParam bits 0-15: normally 1, but Windows folds
   a key's un-retrieved auto-repeats into one message with a higher count
   whenever this thread falls behind the keyboard (a busy shell's own output
   is the usual cause here) - sending the sequence only once would silently
   drop the rest of a held arrow key or Backspace. */
static int handle_special_key(TerminalView *view, WPARAM key, WORD repeatCount) {
    unsigned int modifiers = key_modifiers();
    if (key == VK_INSERT && (modifiers & TERMINAL_MOD_SHIFT)) {
        paste_clipboard(view);
        return 1;
    }
    char sequence[24];
    size_t length = terminal_input_sequence((unsigned int)key, modifiers,
                                            terminal_grid_app_cursor_keys(view->grid),
                                            sequence, sizeof(sequence));
    if (!length) return 0;
    send_repeated_bytes(view, sequence, length, repeatCount);
    return 1;
}

static void send_character(TerminalView *view, WCHAR ch, int prefixEscape, WORD repeatCount) {
    char utf8[8];
    int offset = prefixEscape ? 1 : 0;
    if (prefixEscape) utf8[0] = '\x1b';
    int length = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, utf8 + offset,
                                     (int)sizeof(utf8) - offset, NULL, NULL);
    if (length > 0) send_repeated_bytes(view, utf8, (size_t)(length + offset), repeatCount);
}

static void session_write(TerminalView *view, const char *bytes, size_t length) {
    if (!view || !bytes || !length) return;
    terminal_grid_feed(view->grid, bytes, length, NULL, NULL);
    view->scrollOffset = 0;
    if (terminal_grid_take_dirty(view->grid)) InvalidateRect(view->hwnd, NULL, FALSE);
}

static size_t utf8_previous(const char *text, size_t position) {
    if (!position) return 0;
    position--;
    while (position > 0 && ((unsigned char)text[position] & 0xc0) == 0x80) position--;
    return position;
}

static size_t utf8_next(const char *text, size_t length, size_t position) {
    if (position >= length) return length;
    position++;
    while (position < length && ((unsigned char)text[position] & 0xc0) == 0x80) position++;
    return position;
}

static int utf8_characters(const char *text, size_t length) {
    int count = 0;
    for (size_t i = 0; i < length; i++)
        if (((unsigned char)text[i] & 0xc0) != 0x80) count++;
    return count;
}

static size_t utf8_byte_at_character(const char *text, size_t length, int character) {
    size_t position = 0;
    while (position < length && character > 0) {
        position = utf8_next(text, length, position);
        character--;
    }
    return position;
}

static void session_cursor_move(TerminalView *view, int count, char direction) {
    if (count <= 0) return;
    char sequence[32];
    int length = snprintf(sequence, sizeof(sequence), "\x1b[%d%c", count, direction);
    if (length > 0) session_write(view, sequence, (size_t)length);
}

/* Keeps editable input on one terminal row. Long values use a horizontal
   window around the logical cursor, avoiding VT cursor moves across wrapped
   rows (which clamp at the edge and would overwrite the prompt). */
static void redraw_session_input(TerminalView *view) {
    const char *text = view->inputText ? view->inputText : "";
    int totalChars = utf8_characters(text, view->inputLength);
    int cursorChars = utf8_characters(text, view->inputCursor);
    int capacity = view->inputDisplayCapacity;
    if (capacity < 1) capacity = 1;
    int firstChar = cursorChars > capacity ? cursorChars - capacity : 0;
    int visibleChars = totalChars - firstChar;
    if (visibleChars > capacity) visibleChars = capacity;
    int visibleCursor = cursorChars - firstChar;
    size_t firstByte = utf8_byte_at_character(text, view->inputLength, firstChar);
    size_t lastByte = utf8_byte_at_character(text, view->inputLength, firstChar + visibleChars);
    int padding = view->inputDisplayCharacters > visibleChars
        ? view->inputDisplayCharacters - visibleChars : 0;
    session_cursor_move(view, view->inputDisplayCursorCharacters, 'D');
    session_write(view, text + firstByte, lastByte - firstByte);
    for (int i = 0; i < padding; i++) session_write(view, " ", 1);
    session_cursor_move(view, visibleChars - visibleCursor + padding, 'D');
    view->inputDisplayCharacters = visibleChars;
    view->inputDisplayCursorCharacters = visibleCursor;
}

static int append_session_input(TerminalView *view, const char *bytes, size_t length) {
    if (!view->inputPending || !length ||
        view->inputLength + length > TERMINAL_SESSION_INPUT_MAX) return 0;
    size_t needed = view->inputLength + length + 1;
    if (needed > view->inputCapacity) {
        size_t capacity = view->inputCapacity ? view->inputCapacity * 2 : 64;
        while (capacity < needed) capacity *= 2;
        char *grown = (char *)realloc(view->inputText, capacity);
        if (!grown) return 0;
        view->inputText = grown;
        view->inputCapacity = capacity;
    }
    memmove(view->inputText + view->inputCursor + length,
            view->inputText + view->inputCursor,
            view->inputLength - view->inputCursor);
    memcpy(view->inputText + view->inputCursor, bytes, length);
    view->inputLength += length;
    view->inputCursor += length;
    view->inputText[view->inputLength] = 0;
    redraw_session_input(view);
    return 1;
}

static void erase_session_input_character(TerminalView *view) {
    if (!view->inputPending || !view->inputCursor) return;
    size_t start = utf8_previous(view->inputText, view->inputCursor);
    memmove(view->inputText + start, view->inputText + view->inputCursor,
            view->inputLength - view->inputCursor + 1);
    view->inputLength -= view->inputCursor - start;
    view->inputCursor = start;
    redraw_session_input(view);
}

static void delete_session_input_character(TerminalView *view) {
    if (!view->inputPending || view->inputCursor >= view->inputLength) return;
    size_t next = utf8_next(view->inputText, view->inputLength, view->inputCursor);
    memmove(view->inputText + view->inputCursor, view->inputText + next,
            view->inputLength - next + 1);
    view->inputLength -= next - view->inputCursor;
    redraw_session_input(view);
}

static void replace_session_input(TerminalView *view, const char *text) {
    size_t length = strlen(text);
    if (length > TERMINAL_SESSION_INPUT_MAX) length = TERMINAL_SESSION_INPUT_MAX;
    if (length + 1 > view->inputCapacity) {
        char *grown = (char *)realloc(view->inputText, length + 1);
        if (!grown) return;
        view->inputText = grown;
        view->inputCapacity = length + 1;
    }
    memcpy(view->inputText, text, length);
    view->inputText[length] = 0;
    view->inputLength = length;
    view->inputCursor = length;
    redraw_session_input(view);
}

static void browse_session_history(TerminalView *view, int delta) {
    int index = view->inputHistoryIndex + delta;
    if (index < 0 || index > view->inputHistoryCount || index == view->inputHistoryIndex) return;
    if (view->inputHistoryIndex == view->inputHistoryCount) {
        free(view->inputDraft);
        view->inputDraft = _strdup(view->inputText ? view->inputText : "");
    }
    view->inputHistoryIndex = index;
    replace_session_input(view, index < view->inputHistoryCount
        ? view->inputHistory[index] : (view->inputDraft ? view->inputDraft : ""));
}

static void remember_session_input(TerminalView *view) {
    if (!view->inputText || !view->inputText[0]) return;
    if (view->inputHistoryCount &&
        strcmp(view->inputHistory[view->inputHistoryCount - 1], view->inputText) == 0) return;
    if (view->inputHistoryCount == 50) {
        free(view->inputHistory[0]);
        memmove(view->inputHistory, view->inputHistory + 1, 49 * sizeof(char *));
        view->inputHistoryCount--;
    }
    char *copy = _strdup(view->inputText);
    if (copy) view->inputHistory[view->inputHistoryCount++] = copy;
}

static void finish_session_input(TerminalView *view, int cancelled) {
    if (!view->inputPending) return;
    free(view->inputResult);
    view->inputResult = cancelled ? NULL : _strdup(view->inputText ? view->inputText : "");
    if (!cancelled) {
        remember_session_input(view);
        session_write(view, "\r\n", 2);
    }
    view->inputPending = 0;
    view->inputDone = 1;
}

/* ---- the process's output ---- */

static void grid_reply(void *context, const char *bytes, size_t length) {
    TerminalView *view = (TerminalView *)context;
    if (view->running) terminal_process_write(view->process, bytes, length);
}

static void send_notification(TerminalView *view, UINT code) {
    NMHDR notification = { view->hwnd, (UINT_PTR)GetDlgCtrlID(view->hwnd), code };
    SendMessageW(GetParent(view->hwnd), WM_NOTIFY, (WPARAM)notification.idFrom, (LPARAM)&notification);
}

static void handle_chunk(TerminalView *view, TerminalChunk *chunk) {
    if (chunk->session == view->session) {
        /* A preserved shell keeps producing output while the Lua grid is
           active or its finished transcript remains on screen. */
        TerminalGrid *target = view->savedGrid ? view->savedGrid : view->grid;
        int visible = target == view->grid;
        int oldScrollback = terminal_grid_scrollback_count(target);
        if (chunk->length) terminal_grid_feed(target, chunk->data, chunk->length, grid_reply, view);
        if (chunk->finished) {
            view->running = 0;
            view->exitCode = chunk->exitCode;
            char note[128];
            snprintf(note, sizeof(note),
                    "\r\n\x1b[90m[The shell exited with code %lu. Press any key to start it again.]\x1b[0m\r\n",
                    (unsigned long)chunk->exitCode);
            terminal_grid_feed(target, note, strlen(note), NULL, NULL);
            send_notification(view, TVN_EXITED);
        }
        /* New output normally keeps the live tail pinned. If the user has
           scrolled back, compensate for newly archived rows so the text
           currently under the pointer stays in place instead of drifting. */
        if (visible && view->scrollOffset > 0) {
            int added = terminal_grid_scrollback_count(target) - oldScrollback;
            if (added > 0) {
                long long offset = (long long)view->scrollOffset + added;
                int maximum = max_scroll_offset(view);
                view->scrollOffset = offset > maximum ? maximum : (int)offset;
            }
        }
        if (terminal_grid_take_dirty(target) && visible)
            InvalidateRect(view->hwnd, NULL, FALSE);
    }
    free(chunk);
}

/* ---- window procedure ---- */

static LRESULT CALLBACK TerminalViewSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                 UINT_PTR id, DWORD_PTR data) {
    (void)id;
    TerminalView *view = (TerminalView *)data;
    switch (msg) {
        case WM_TERMINAL_CHUNK:
            handle_chunk(view, (TerminalChunk *)lParam);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC hdc = BeginPaint(hwnd, &paint);
            RECT client;
            GetClientRect(hwnd, &client);
            paint_view(view, hdc, &client);
            EndPaint(hwnd, &paint);
            return 0; 
        }
        case WM_SIZE: {
            int previousCols = view->cols;
            resize_to_client(view);
            if (view->inputPending && view->cols != previousCols) {
                /* Resizing invalidates the single-row input viewport's start
                   column. Continue it on a fresh row at the new width. */
                session_write(view, "\r\n", 2);
                view->inputDisplayCharacters = 0;
                view->inputDisplayCursorCharacters = 0;
                view->inputDisplayCapacity = view->cols > 1 ? view->cols - 1 : 1;
                redraw_session_input(view);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_SETFOCUS: case WM_KILLFOCUS:
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_GETDLGCODE:
            /* The pretranslation path normally bypasses IsDialogMessage(),
               but these flags also protect keys delivered directly or by a
               future dialog host: the terminal, not dialog navigation, owns
               Enter, Escape, Tab, characters, and cursor keys. */
            return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB | DLGC_WANTALLKEYS;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            SetCapture(hwnd);
            view->selecting = 1;
            point_to_cell(view, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &view->anchorLine, &view->anchorCol);
            view->pointLine = view->anchorLine;
            view->pointCol = view->anchorCol;
            view->selecting_any = 1;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (view->selecting) {
                point_to_cell(view, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &view->pointLine, &view->pointCol);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (view->selecting) {
                view->selecting = 0;
                ReleaseCapture();
                /* A plain click, without dragging, selects nothing. */
                if (view->anchorLine == view->pointLine && view->anchorCol == view->pointCol) {
                    clear_selection(view);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        case WM_RBUTTONUP:
            if (view->sessionTranscript) restore_shell_grid(view);
            paste_clipboard(view);
            return 0;
        case WM_MOUSEWHEEL: {
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            view->wheelDeltaRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
            int notches = view->wheelDeltaRemainder / WHEEL_DELTA;
            view->wheelDeltaRemainder -= notches * WHEEL_DELTA;
            if (notches) {
                long long amount = lines == WHEEL_PAGESCROLL
                    ? (long long)notches * view->rows
                    : (long long)notches * lines;
                scroll_view(view, amount);
            }
            return 0;
        }
        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl && shift && (wParam == 'C')) {
                copy_selection(view);
                view->suppressTranslatedChar = 1;
                return 0;
            }
            if (view->sessionTranscript) restore_shell_grid(view);
            if (ctrl && shift && (wParam == 'V')) {
                paste_clipboard(view);
                view->suppressTranslatedChar = 1;
                return 0;
            }
            if (view->sessionMode) {
                if (!view->inputPending) return 0;
                if (ctrl && wParam == 'C') {
                    view->inputInterrupted = 1;
                    finish_session_input(view, 1);
                    view->suppressTranslatedChar = 1;
                } else if (shift && wParam == VK_INSERT) {
                    paste_clipboard(view);
                    view->suppressTranslatedChar = 1;
                } else if (wParam == VK_RETURN) {
                    finish_session_input(view, 0);
                    view->suppressTranslatedChar = 1;
                } else if (wParam == VK_ESCAPE) {
                    finish_session_input(view, 1);
                    view->suppressTranslatedChar = 1;
                } else if (wParam == VK_BACK) {
                    erase_session_input_character(view);
                    view->suppressTranslatedChar = 1;
                } else if (wParam == VK_DELETE) {
                    delete_session_input_character(view);
                    view->suppressTranslatedChar = 1;
                } else if (wParam == VK_LEFT) {
                    if (view->inputCursor > 0) {
                        view->inputCursor = utf8_previous(view->inputText, view->inputCursor);
                        redraw_session_input(view);
                    }
                    view->suppressTranslatedChar = 1;
                } else if (wParam == VK_RIGHT) {
                    if (view->inputCursor < view->inputLength) {
                        view->inputCursor = utf8_next(view->inputText, view->inputLength,
                                                      view->inputCursor);
                        redraw_session_input(view);
                    }
                    view->suppressTranslatedChar = 1;
                } else if (wParam == VK_HOME) {
                    view->inputCursor = 0;
                    redraw_session_input(view);
                    view->suppressTranslatedChar = 1;
                } else if (wParam == VK_END) {
                    view->inputCursor = view->inputLength;
                    redraw_session_input(view);
                    view->suppressTranslatedChar = 1;
                } else if (wParam == VK_UP) {
                    browse_session_history(view, -1);
                    view->suppressTranslatedChar = 1;
                } else if (wParam == VK_DOWN) {
                    browse_session_history(view, 1);
                    view->suppressTranslatedChar = 1;
                }
                return 0;
            }
            if (!view->running) {
                if (wParam != VK_SHIFT && wParam != VK_CONTROL && wParam != VK_MENU) {
                    view->suppressTranslatedChar = 1;
                    send_notification(view, TVN_RESTART_REQUESTED);
                }
                return 0;
            }
            if (view->scrollOffset != 0 && wParam != VK_SHIFT && wParam != VK_CONTROL && wParam != VK_MENU)
                view->scrollOffset = 0; /* typing returns the view to the live tail */
            if (handle_special_key(view, wParam, LOWORD(lParam))) {
                view->suppressTranslatedChar = 1;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_CHAR: case WM_SYSCHAR: {
            if (view->sessionMode) {
                if (view->inputPending && wParam >= ' ') {
                    char utf8[8];
                    WCHAR character = (WCHAR)wParam;
                    int length = WideCharToMultiByte(CP_UTF8, 0, &character, 1,
                                                     utf8, sizeof(utf8), NULL, NULL);
                    if (length > 0) append_session_input(view, utf8, (size_t)length);
                }
                return 0;
            }
            if (!view->running) return 0;
            /* WM_SYSCHAR represents Alt/Meta and gets an Escape prefix. Right
               Alt plus Control is AltGr, so it remains a layout character. */
            int altGr = (GetKeyState(VK_RMENU) & 0x8000) &&
                        (GetKeyState(VK_CONTROL) & 0x8000);
            /* lParam's repeat count (see handle_special_key's comment) covers
               held printable keys the same way. */
            send_character(view, (WCHAR)wParam, msg == WM_SYSCHAR && !altGr, LOWORD(lParam));
            return 0;
        }
        case WM_DEADCHAR:
        case WM_SYSDEADCHAR:
        case WM_SYSKEYUP:
            return 0; /* keep Alt input from activating the application menu */
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, TerminalViewSubclassProc, 1);
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

/* Removes any WM_(SYS)CHAR/WM_(SYS)DEADCHAR TranslateMessage just queued for
   this window. TranslateMessage's return value cannot tell us whether it
   actually queued one: MSDN documents it as nonzero for WM_KEYDOWN/WM_KEYUP/
   WM_SYSKEYDOWN/WM_SYSKEYUP regardless of whether translation produced a
   character, so arrow keys, Home/End, Delete and the like report "translated"
   even though they never queue anything - peeking the queue directly is the
   only reliable way to find out, and to clear it when there is one. */
static void discard_translated_char(HWND hwnd) {
    MSG discard;
    while (PeekMessageW(&discard, hwnd, WM_CHAR, WM_CHAR, PM_REMOVE) ||
           PeekMessageW(&discard, hwnd, WM_SYSCHAR, WM_SYSCHAR, PM_REMOVE) ||
           PeekMessageW(&discard, hwnd, WM_DEADCHAR, WM_DEADCHAR, PM_REMOVE) ||
           PeekMessageW(&discard, hwnd, WM_SYSDEADCHAR, WM_SYSDEADCHAR, PM_REMOVE)) {}
}

int terminal_view_pretranslate_message(TerminalView *view, const MSG *message) {
    if (!view || !view->hwnd || !message ||
        (message->hwnd != view->hwnd && !IsChild(view->hwnd, message->hwnd)))
        return 0;
    switch (message->message) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            /* Translate first so layout-aware WM_(SYS)CHAR is queued, then let
               the key handler emit any non-text VT sequence. If it did,
               discard whatever character translation queued for this key -
               it must not also be delivered as typed text. */
            view->suppressTranslatedChar = 0;
            TranslateMessage(message);
            DispatchMessage(message);
            if (view->suppressTranslatedChar) discard_translated_char(view->hwnd);
            return 1;
        case WM_KEYUP: case WM_SYSKEYUP:
        case WM_CHAR: case WM_SYSCHAR:
        case WM_DEADCHAR: case WM_SYSDEADCHAR:
            DispatchMessage(message);
            return 1;
        default:
            return 0;
    }
}

/* ---- lifecycle ---- */

TerminalView *terminal_view_create(HWND parent, int id) {
    TerminalView *view = (TerminalView *)calloc(1, sizeof(*view));
    if (!view) return NULL;
    /* SS_NOTIFY: without it, a STATIC control answers WM_NCHITTEST with
       HTTRANSPARENT, so real mouse clicks fall through to whatever is behind
       it instead of reaching this window's WM_LBUTTONDOWN - the control
       still takes focus and keystrokes fine via SetFocus() (the keyboard
       pane-cycling shortcut, Ctrl+F6, isn't affected), but a user clicking
       directly on the terminal can't focus or type into it. */
    view->hwnd = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | SS_NOTIFY,
                                 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    if (!view->hwnd) { free(view); return NULL; }
    view->font = (HFONT)GetStockObject(SYSTEM_FIXED_FONT);
    view->cols = 80;
    view->rows = 24;
    view->grid = terminal_grid_create(view->cols, view->rows);
    if (!view->grid) { DestroyWindow(view->hwnd); free(view); return NULL; }
    measure_font(view);
    SetWindowSubclass(view->hwnd, TerminalViewSubclassProc, 1, (DWORD_PTR)view);
    return view;
}

HWND terminal_view_window(const TerminalView *view) { return view ? view->hwnd : NULL; }

void terminal_view_free(TerminalView *view) {
    if (!view) return;
    terminal_process_free(view->process);
    /* terminal_process_free() joins the reader/watcher threads, but a
       WM_TERMINAL_CHUNK they posted just before the join can still be
       sitting in this thread's queue (it wasn't being pumped while blocked
       on the join). Drain and free any such chunk now, since once the
       window below is destroyed DispatchMessage will just drop it. */
    MSG msg;
    while (PeekMessageW(&msg, view->hwnd, WM_TERMINAL_CHUNK, WM_TERMINAL_CHUNK, PM_REMOVE))
        free((TerminalChunk *)msg.lParam);
    terminal_grid_free(view->grid);
    terminal_grid_free(view->savedGrid);
    free(view->inputText);
    free(view->inputResult);
    free(view->inputDraft);
    for (int i = 0; i < view->inputHistoryCount; i++) free(view->inputHistory[i]);
    release_back_buffer(view);
    if (view->hwnd) DestroyWindow(view->hwnd);
    free(view);
}

int terminal_view_start(TerminalView *view, const WCHAR *commandLine, const WCHAR *directory) {
    if (view->savedGrid) restore_shell_grid(view);
    view->sessionTranscript = 0;
    terminal_process_free(view->process);
    view->process = NULL;
    view->running = 0;
    clear_selection(view);
    view->scrollOffset = 0;
    terminal_grid_reset(view->grid);
    resize_to_client(view);
    view->session = next_session++;
    view->process = terminal_process_start(commandLine, directory, view->cols, view->rows,
                                           view->hwnd, WM_TERMINAL_CHUNK, view->session);
    view->running = view->process != NULL;
    InvalidateRect(view->hwnd, NULL, FALSE);
    return view->running;
}

void terminal_view_show_message(TerminalView *view, const WCHAR *message) {
    int utf8Length = WideCharToMultiByte(CP_UTF8, 0, message, -1, NULL, 0, NULL, NULL);
    if (utf8Length <= 0) return;
    static const char prefix[] = "\x1b[90m", suffix[] = "\x1b[0m\r\n";
    size_t capacity = sizeof(prefix) - 1 + (size_t)utf8Length - 1 + sizeof(suffix) - 1;
    char *text = (char *)malloc(capacity);
    if (!text) return;
    memcpy(text, prefix, sizeof(prefix) - 1);
    WideCharToMultiByte(CP_UTF8, 0, message, -1, text + sizeof(prefix) - 1, utf8Length, NULL, NULL);
    memcpy(text + sizeof(prefix) - 1 + utf8Length - 1, suffix, sizeof(suffix) - 1);
    terminal_grid_feed(view->grid, text, capacity, NULL, NULL);
    free(text);
    InvalidateRect(view->hwnd, NULL, FALSE);
}

int terminal_view_running(const TerminalView *view) {
    return view && (view->running || view->sessionMode);
}

int terminal_view_session_active(const TerminalView *view) {
    return view && view->sessionMode;
}

void terminal_view_stop(TerminalView *view) {
    if (view && view->sessionMode) {
        finish_session_input(view, 1);
        view->sessionMode = 0;
        view->sessionTranscript = 1;
        return;
    }
    if (view->process) terminal_process_stop(view->process);
}

void terminal_view_clear_scrollback(TerminalView *view) {
    terminal_grid_clear(view->grid);
    clear_selection(view);
    view->scrollOffset = 0;
    InvalidateRect(view->hwnd, NULL, FALSE);
}

int terminal_view_begin_session(TerminalView *view) {
    if (!view) return 0;
    if (view->savedGrid) restore_shell_grid(view);
    if (view->running) {
        TerminalGrid *sessionGrid = terminal_grid_create(view->cols, view->rows);
        if (!sessionGrid) return 0;
        view->savedGrid = view->grid;
        view->grid = sessionGrid;
    } else {
        terminal_grid_clear(view->grid);
    }
    view->sessionMode = 1;
    view->sessionTranscript = 0;
    view->inputPending = 0;
    view->inputDone = 0;
    view->inputLength = 0;
    free(view->inputResult);
    view->inputResult = NULL;
    clear_selection(view);
    view->scrollOffset = 0;
    terminal_grid_clear(view->grid);
    InvalidateRect(view->hwnd, NULL, FALSE);
    return 1;
}

void terminal_view_write(TerminalView *view, const char *text, size_t length) {
    if (view && view->sessionMode) session_write(view, text, length);
}

char *terminal_view_read_line(TerminalView *view, HWND owner, const char *prompt,
                              int *interrupted) {
    if (interrupted) *interrupted = 0;
    if (!view || !view->sessionMode || view->inputPending) return NULL;
    if (prompt) session_write(view, prompt, strlen(prompt));
    int cursorCol = 0;
    terminal_grid_cursor(view->grid, &cursorCol, NULL, NULL);
    if (view->cols > 1 && cursorCol >= view->cols - 1) {
        session_write(view, "\r\n", 2);
        cursorCol = 0;
    }
    view->inputLength = 0;
    view->inputCursor = 0;
    view->inputDisplayCharacters = 0;
    view->inputDisplayCursorCharacters = 0;
    view->inputDisplayCapacity = view->cols - cursorCol - 1;
    if (view->inputDisplayCapacity < 1) view->inputDisplayCapacity = 1;
    if (view->inputText) view->inputText[0] = 0;
    free(view->inputResult);
    view->inputResult = NULL;
    view->inputDone = 0;
    view->inputInterrupted = 0;
    view->inputHistoryIndex = view->inputHistoryCount;
    free(view->inputDraft);
    view->inputDraft = NULL;
    view->inputPending = 1;
    SetFocus(view->hwnd);

    MSG message = {0};
    int result = 1;
    while (!view->inputDone && (result = GetMessageW(&message, NULL, 0, 0)) > 0) {
        if (message.hwnd == owner && message.message == WM_CLOSE) {
            finish_session_input(view, 1);
            continue;
        }
        if (terminal_view_pretranslate_message(view, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (result == 0) PostQuitMessage((int)message.wParam);
    view->inputPending = 0;
    view->inputDone = 0;
    char *answer = view->inputResult;
    view->inputResult = NULL;
    if (interrupted) *interrupted = view->inputInterrupted;
    return answer;
}

void terminal_view_end_session(TerminalView *view) {
    if (!view || !view->sessionMode) return;
    finish_session_input(view, 1);
    view->sessionMode = 0;
    view->sessionTranscript = 1;
    session_write(view,
        "\r\n\x1b[90m[Lua finished. Press any key to return to PowerShell.]\x1b[0m\r\n",
        sizeof("\r\n\x1b[90m[Lua finished. Press any key to return to PowerShell.]\x1b[0m\r\n") - 1);
}

/* The rendered screen's text - for tests, the same way terminal_grid.h's own
   terminal_grid_line_text() is; the grid itself stays private otherwise. */
int terminal_view_total_lines(const TerminalView *view) {
    return terminal_grid_total_lines(view->grid);
}

void terminal_view_line_text(const TerminalView *view, int lineIndex, char *utf8, size_t capacity) {
    terminal_grid_line_text(view->grid, lineIndex, utf8, capacity);
}

void terminal_view_apply_theme(TerminalView *view, const COLORREF ansi[16],
                               COLORREF defaultFg, COLORREF defaultBg) {
    memcpy(view->palette.ansi, ansi, sizeof(view->palette.ansi));
    view->palette.defaultFg = defaultFg;
    view->palette.defaultBg = defaultBg;
    InvalidateRect(view->hwnd, NULL, FALSE);
}

void terminal_view_apply_font(TerminalView *view, const char *faceNameUtf8, int pointSize, int weight) {
    WCHAR faceName[64];
    if (MultiByteToWideChar(CP_UTF8, 0, faceNameUtf8, -1, faceName, 64) == 0)
        wcscpy(faceName, L"Consolas");
    HDC hdc = GetDC(view->hwnd);
    int height = -MulDiv(pointSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(view->hwnd, hdc);
    HFONT font = CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, faceName);
    if (!font) return;
    if (view->font && view->font != (HFONT)GetStockObject(SYSTEM_FIXED_FONT)) DeleteObject(view->font);
    view->font = font;
    measure_font(view);
    resize_to_client(view);
    InvalidateRect(view->hwnd, NULL, FALSE);
}
