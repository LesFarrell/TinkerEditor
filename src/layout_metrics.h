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

/* The pixel x-offset everything right of the explorer panel (always docked
   at the left) is shifted by - the panel's width plus its splitter, when
   it's occupying space; 0 otherwise (hidden, focus mode, or not created
   yet). Also includes the Git panel's width when that one is currently
   docked at the left too. layout() derives this inline; other call sites
   that need the same value once it has settled (the main splitter's
   hit-testing/drag math) call this instead of re-deriving it, so they
   cannot drift out of sync with where the panels actually are. */
int explorer_content_x(int focusModeActive);
/* The explorer's own occupied width (panel plus its splitter) - 0 when it
   isn't occupying space. Unlike explorer_content_x(), this doesn't include
   the Git panel's; callers that need to know how much of the client width
   the explorer alone has claimed use this instead. */
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

/* Same shape again for the Merge Conflicts panel (merge_panel.inc), which
   always docks at the right edge - no "on right" flag needed. */
extern int g_mergePanelVisible;
extern int g_mergePanelWidth;
/* The Merge panel's own occupied width (panel plus its splitter) - 0 when
   it isn't showing. */
int merge_panel_width(int focusModeActive);

#endif
