#ifndef TOOLBAR_ICONS_H
#define TOOLBAR_ICONS_H

#include <windows.h>

/* The formatting toolbar's buttons draw their icons on the fly with GDI
   (lines/shapes/text) rather than loading them from image resources, so they
   redraw crisply at any DPI and recolor for free when the theme changes.
   Each function paints into `rc` (the button's client rect) using `fg` as
   the stroke/fill color; none of them touch any application state. */
typedef void (*ToolbarIconPaintFn)(HDC hdc, const RECT *rc, COLORREF fg);

void toolbar_icon_bold(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_italic(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_strikethrough(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_code(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_h1(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_h2(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_h3(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_quote(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_bullet_list(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_number_list(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_task_list(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_link(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_image(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_table(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_codeblock(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_togglepreview(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_splitview(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_swappanels(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_terminal(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_footnote(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_hrule(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_alert(HDC hdc, const RECT *rc, COLORREF fg);
void toolbar_icon_wikilink(HDC hdc, const RECT *rc, COLORREF fg);

#endif
