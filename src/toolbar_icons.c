#include "toolbar_icons.h"

void toolbar_icon_bold(HDC hdc, const RECT *rc, COLORREF fg) {
    HFONT font = CreateFontA(-(rc->bottom - rc->top) * 6 / 10, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Georgia");
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, "B", -1, (RECT *)rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(font);
}

void toolbar_icon_italic(HDC hdc, const RECT *rc, COLORREF fg) {
    HFONT font = CreateFontA(-(rc->bottom - rc->top) * 6 / 10, 0, 0, 0, FW_NORMAL, TRUE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Georgia");
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, "I", -1, (RECT *)rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(font);
}

void toolbar_icon_strikethrough(HDC hdc, const RECT *rc, COLORREF fg) {
    HFONT font = CreateFontA(-(rc->bottom - rc->top) * 6 / 10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    RECT textRc = *rc;
    DrawTextA(hdc, "S", -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_CALCRECT);
    OffsetRect(&textRc, ((rc->right - rc->left) - (textRc.right - textRc.left)) / 2,
               ((rc->bottom - rc->top) - (textRc.bottom - textRc.top)) / 2);
    DrawTextA(hdc, "S", -1, (RECT *)rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    int midY = (textRc.top + textRc.bottom) / 2;
    MoveToEx(hdc, textRc.left, midY, NULL);
    LineTo(hdc, textRc.right, midY);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    SelectObject(hdc, old);
    DeleteObject(font);
}

void toolbar_icon_code(HDC hdc, const RECT *rc, COLORREF fg) {
    HFONT font = CreateFontA(-(rc->bottom - rc->top) * 5 / 10, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, "</>", -1, (RECT *)rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(font);
}

static void toolbar_icon_heading(HDC hdc, const RECT *rc, COLORREF fg, const char *label) {
    HFONT font = CreateFontA(-(rc->bottom - rc->top) * 55 / 100, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, label, -1, (RECT *)rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(font);
}
void toolbar_icon_h1(HDC hdc, const RECT *rc, COLORREF fg) { toolbar_icon_heading(hdc, rc, fg, "H1"); }
void toolbar_icon_h2(HDC hdc, const RECT *rc, COLORREF fg) { toolbar_icon_heading(hdc, rc, fg, "H2"); }
void toolbar_icon_h3(HDC hdc, const RECT *rc, COLORREF fg) { toolbar_icon_heading(hdc, rc, fg, "H3"); }

void toolbar_icon_quote(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    int barX = rc->left + w * 3 / 10;
    HPEN pen = CreatePen(PS_SOLID, (h + 15) / 16 > 1 ? (h + 15) / 16 : 2, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, barX, rc->top + h * 2 / 10, NULL);
    LineTo(hdc, barX, rc->bottom - h * 2 / 10);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    int lineX0 = barX + w / 8, lineX1 = rc->right - w * 2 / 10;
    HPEN thin = CreatePen(PS_SOLID, 1, fg);
    oldPen = (HPEN)SelectObject(hdc, thin);
    for (int i = 0; i < 3; i++) {
        int y = rc->top + h * 3 / 10 + i * (h * 4 / 10) / 2;
        MoveToEx(hdc, lineX0, y, NULL);
        LineTo(hdc, lineX1, y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(thin);
}

void toolbar_icon_bullet_list(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    HBRUSH brush = CreateSolidBrush(fg);
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);
    int dotR = h / 10 > 1 ? h / 10 : 1;
    int lineX0 = rc->left + w * 4 / 10, lineX1 = rc->right - w * 2 / 10;
    for (int i = 0; i < 3; i++) {
        int y = rc->top + h * 22 / 100 + i * (h * 28 / 100);
        Ellipse(hdc, rc->left + w * 2 / 10 - dotR, y - dotR, rc->left + w * 2 / 10 + dotR, y + dotR);
        MoveToEx(hdc, lineX0, y, NULL);
        LineTo(hdc, lineX1, y);
    }
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void toolbar_icon_number_list(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    HFONT font = CreateFontA(-h * 32 / 100, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    int lineX0 = rc->left + w * 4 / 10, lineX1 = rc->right - w * 2 / 10;
    const char *digits[3] = { "1", "2", "3" };
    for (int i = 0; i < 3; i++) {
        int y = rc->top + h * 22 / 100 + i * (h * 28 / 100);
        RECT digitRc = { rc->left, y - h / 6, rc->left + w * 3 / 10, y + h / 6 };
        DrawTextA(hdc, digits[i], -1, &digitRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        MoveToEx(hdc, lineX0, y, NULL);
        LineTo(hdc, lineX1, y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    SelectObject(hdc, old);
    DeleteObject(font);
}

void toolbar_icon_task_list(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    int boxSize = h * 4 / 10;
    RECT box = { rc->left + w * 15 / 100, rc->top + (h - boxSize) / 2,
                 rc->left + w * 15 / 100 + boxSize, rc->top + (h - boxSize) / 2 + boxSize };
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, box.left, box.top, box.right, box.bottom);
    MoveToEx(hdc, box.left + boxSize / 6, box.top + boxSize / 2, NULL);
    LineTo(hdc, box.left + boxSize * 2 / 5, box.bottom - boxSize / 8);
    LineTo(hdc, box.right - boxSize / 8, box.top + boxSize / 6);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);

    HPEN thin = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen2 = (HPEN)SelectObject(hdc, thin);
    int lineX0 = box.right + w / 10, lineX1 = rc->right - w * 15 / 100;
    int y = (box.top + box.bottom) / 2;
    MoveToEx(hdc, lineX0, y, NULL);
    LineTo(hdc, lineX1, y);
    SelectObject(hdc, oldPen2);
    DeleteObject(thin);
}

void toolbar_icon_link(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    int linkW = w * 45 / 100, linkH = h * 45 / 100;
    HPEN pen = CreatePen(PS_SOLID, (h + 11) / 12 > 1 ? (h + 11) / 12 : 2, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

    RECT r1 = { rc->left + w * 12 / 100, rc->top + h * 12 / 100,
                rc->left + w * 12 / 100 + linkW, rc->top + h * 12 / 100 + linkH };
    RoundRect(hdc, r1.left, r1.top, r1.right, r1.bottom, linkH / 2, linkH / 2);

    RECT r2 = { rc->right - w * 12 / 100 - linkW, rc->bottom - h * 12 / 100 - linkH,
                rc->right - w * 12 / 100, rc->bottom - h * 12 / 100 };
    RoundRect(hdc, r2.left, r2.top, r2.right, r2.bottom, linkH / 2, linkH / 2);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

void toolbar_icon_image(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    RECT frame = { rc->left + w * 12 / 100, rc->top + h * 18 / 100, rc->right - w * 12 / 100, rc->bottom - h * 18 / 100 };
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, frame.left, frame.top, frame.right, frame.bottom);

    HBRUSH sunBrush = CreateSolidBrush(fg);
    HBRUSH oldB2 = (HBRUSH)SelectObject(hdc, sunBrush);
    int sunR = (frame.bottom - frame.top) / 6;
    int sunCx = frame.left + (frame.right - frame.left) * 3 / 10, sunCy = frame.top + (frame.bottom - frame.top) * 3 / 10;
    Ellipse(hdc, sunCx - sunR, sunCy - sunR, sunCx + sunR, sunCy + sunR);
    SelectObject(hdc, oldB2);
    DeleteObject(sunBrush);

    POINT mountain[4] = {
        { frame.left + 1, frame.bottom - 1 },
        { frame.left + (frame.right - frame.left) * 4 / 10, frame.top + (frame.bottom - frame.top) * 4 / 10 },
        { frame.left + (frame.right - frame.left) * 7 / 10, frame.bottom - (frame.bottom - frame.top) * 35 / 100 },
        { frame.right - 1, frame.bottom - 1 },
    };
    HBRUSH mountainBrush = CreateSolidBrush(fg);
    HBRUSH oldB3 = (HBRUSH)SelectObject(hdc, mountainBrush);
    Polygon(hdc, mountain, 4);
    SelectObject(hdc, oldB3);
    DeleteObject(mountainBrush);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

void toolbar_icon_table(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    RECT grid = { rc->left + w * 12 / 100, rc->top + h * 15 / 100, rc->right - w * 12 / 100, rc->bottom - h * 15 / 100 };
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, grid.left, grid.top, grid.right, grid.bottom);
    int colW = (grid.right - grid.left) / 3;
    for (int i = 1; i < 3; i++) {
        MoveToEx(hdc, grid.left + colW * i, grid.top, NULL);
        LineTo(hdc, grid.left + colW * i, grid.bottom);
    }
    int rowH = (grid.bottom - grid.top) / 2;
    MoveToEx(hdc, grid.left, grid.top + rowH, NULL);
    LineTo(hdc, grid.right, grid.top + rowH);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

/* A bordered box (the code fence) with a couple of short interior strokes
   standing in for lines of code - distinct from toolbar_icon_code's inline
   "</>" glyph, which has no border. */
void toolbar_icon_codeblock(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    RECT box = { rc->left + w * 8 / 100, rc->top + h * 12 / 100, rc->right - w * 8 / 100, rc->bottom - h * 12 / 100 };
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, box.left, box.top, box.right, box.bottom);

    int lineH = (box.bottom - box.top) / 4;
    int inset = (box.right - box.left) * 15 / 100;
    MoveToEx(hdc, box.left + inset, box.top + lineH, NULL);
    LineTo(hdc, box.right - inset * 2, box.top + lineH);
    MoveToEx(hdc, box.left + inset, box.top + lineH * 2, NULL);
    LineTo(hdc, box.right - inset, box.top + lineH * 2);
    MoveToEx(hdc, box.left + inset, box.top + lineH * 3, NULL);
    LineTo(hdc, box.right - inset * 3, box.top + lineH * 3);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

/* A bordered box split into two panes, the right one filled - standing in
   for the editor/preview split with the preview pane highlighted. Used for
   the toolbar's preview-toggle button, mirroring the View menu's "Show
   Preview" command (IDM_VIEW_TOGGLEPREVIEW). */
void toolbar_icon_togglepreview(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    RECT box = { rc->left + w * 8 / 100, rc->top + h * 12 / 100, rc->right - w * 8 / 100, rc->bottom - h * 12 / 100 };
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, box.left, box.top, box.right, box.bottom);

    int midX = box.left + (box.right - box.left) / 2;
    MoveToEx(hdc, midX, box.top, NULL);
    LineTo(hdc, midX, box.bottom);

    RECT rightPane = { midX + 1, box.top + 1, box.right, box.bottom - 1 };
    HBRUSH fillBrush = CreateSolidBrush(fg);
    FillRect(hdc, &rightPane, fillBrush);
    DeleteObject(fillBrush);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

/* A box with a horizontal divider and the bottom half filled for the
   horizontal split-editor command. */
void toolbar_icon_splitview(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    RECT box = { rc->left + w * 8 / 100, rc->top + h * 12 / 100, rc->right - w * 8 / 100, rc->bottom - h * 12 / 100 };
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, box.left, box.top, box.right, box.bottom);

    int midY = box.top + (box.bottom - box.top) / 2;
    MoveToEx(hdc, box.left, midY, NULL);
    LineTo(hdc, box.right, midY);

    RECT bottomPane = { box.left + 1, midY + 1, box.right - 1, box.bottom };
    HBRUSH fillBrush = CreateSolidBrush(fg);
    FillRect(hdc, &bottomPane, fillBrush);
    DeleteObject(fillBrush);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

/* Two opposing arrows represent exchanging the left and right panes. */
void toolbar_icon_swappanels(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    int left = rc->left + w * 18 / 100;
    int right = rc->right - w * 18 / 100;
    int topY = rc->top + h * 35 / 100;
    int bottomY = rc->top + h * 65 / 100;
    int arrow = h * 14 / 100;

    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);

    MoveToEx(hdc, left, topY, NULL);
    LineTo(hdc, right, topY);
    MoveToEx(hdc, right, topY, NULL);
    LineTo(hdc, right - arrow, topY - arrow);
    MoveToEx(hdc, right, topY, NULL);
    LineTo(hdc, right - arrow, topY + arrow);

    MoveToEx(hdc, right, bottomY, NULL);
    LineTo(hdc, left, bottomY);
    MoveToEx(hdc, left, bottomY, NULL);
    LineTo(hdc, left + arrow, bottomY - arrow);
    MoveToEx(hdc, left, bottomY, NULL);
    LineTo(hdc, left + arrow, bottomY + arrow);

    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

/* A terminal window frame with a ">_" prompt inside. */
void toolbar_icon_terminal(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    RECT box = { rc->left + w * 8 / 100, rc->top + h * 12 / 100, rc->right - w * 8 / 100, rc->bottom - h * 12 / 100 };
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, box.left, box.top, box.right, box.bottom);

    int boxW = box.right - box.left, boxH = box.bottom - box.top;
    int promptX = box.left + boxW * 20 / 100;
    int promptTopY = box.top + boxH * 30 / 100;
    int promptMidY = box.top + boxH * 50 / 100;
    int promptBottomY = box.top + boxH * 70 / 100;
    int chevronDx = boxW * 16 / 100;

    MoveToEx(hdc, promptX, promptTopY, NULL);
    LineTo(hdc, promptX + chevronDx, promptMidY);
    LineTo(hdc, promptX, promptBottomY);

    int underscoreLeft = promptX + chevronDx + boxW * 6 / 100;
    int underscoreRight = box.right - boxW * 20 / 100;
    MoveToEx(hdc, underscoreLeft, promptBottomY, NULL);
    LineTo(hdc, underscoreRight, promptBottomY);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

static void toolbar_icon_text(HDC hdc, const RECT *rc, COLORREF fg, const char *label, int size) {
    HFONT font = CreateFontA(-size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, "Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, font);
    RECT textRc = *rc;
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, label, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(font);
}

void toolbar_icon_footnote(HDC hdc, const RECT *rc, COLORREF fg) {
    toolbar_icon_text(hdc, rc, fg, "fn", 14);
}

void toolbar_icon_hrule(HDC hdc, const RECT *rc, COLORREF fg) {
    HPEN pen = CreatePen(PS_SOLID, 2, fg);
    HPEN old = (HPEN)SelectObject(hdc, pen);
    int y = (rc->top + rc->bottom) / 2;
    MoveToEx(hdc, rc->left + 3, y, NULL);
    LineTo(hdc, rc->right - 3, y);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

void toolbar_icon_alert(HDC hdc, const RECT *rc, COLORREF fg) {
    toolbar_icon_text(hdc, rc, fg, "!", 18);
}

/* The double-bracket wikilink syntax, mirroring toolbar_icon_code's "</>"
   glyph approach for a plugin-defined command rather than a shape. */
void toolbar_icon_wikilink(HDC hdc, const RECT *rc, COLORREF fg) {
    HFONT font = CreateFontA(-(rc->bottom - rc->top) * 42 / 100, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, "[[ ]]", -1, (RECT *)rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(font);
}
