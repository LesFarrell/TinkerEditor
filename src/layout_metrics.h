#ifndef TINKEREDIT_LAYOUT_METRICS_H
#define TINKEREDIT_LAYOUT_METRICS_H

#include <windows.h>

/* Geometry of the file-explorer panel, kept as plain values with no window
   handles or UI entry points so the persistence code can read and write them
   without depending on the explorer's translation unit. The panel's own
   module (file_explorer.c) still owns everything else about the tree. */

/* Persisted flag: whether the explorer panel is shown. */
extern int g_explorerVisible;
/* Live pixel width of the panel; the -1 sentinel means "derive from the ratio". */
extern int g_explorerWidth;
/* Persisted permille of the client width, like g_splitterRatio. */
extern int g_explorerWidthRatio;
/* Persisted flag: docked at the right edge instead of the left. */
extern int g_explorerOnRight;

/* The pixel x-offset everything right of the explorer panel is shifted by -
   the panel's width plus its splitter, when it's docked at the left and
   occupying space; 0 otherwise (hidden, focus mode, not created yet, or
   docked at the right instead, where nothing to its right needs shifting).
   layout() derives this inline; other call sites that need the same value
   once it has settled (the main splitter's hit-testing/drag math) call this
   instead of re-deriving it, so they cannot drift out of sync with where the
   panel actually is. */
int explorer_content_x(int focusModeActive);
/* The total width (panel plus its splitter) the explorer occupies, on
   whichever side it's docked - 0 when it isn't occupying space. Unlike
   explorer_content_x(), this doesn't depend on which side it's on; callers
   that need to know how much of the client width the panel has claimed
   (rather than where its own content starts) use this instead. */
int explorer_panel_width(int focusModeActive);

/* Same shape of state for the Git panel (git_panel.inc), which docks at
   either edge the same way the explorer does. Kept here, rather than
   alongside the rest of git_panel.inc's state, for the same reason as the
   explorer's: settings.c persists it without depending on git_panel.inc's
   translation unit. */
extern int g_gitPanelVisible;
extern int g_gitPanelWidth;
extern int g_gitPanelOnRight;
/* The Git panel's own occupied width (panel plus its splitter), regardless
   of which side it's docked - 0 when it isn't showing. */
int git_panel_width(int focusModeActive);

#endif
