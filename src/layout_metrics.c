#include "layout_metrics.h"

#include "settings_values.h" /* SPLITTER_WIDTH */

/* Defined in file_explorer.c. Used only so explorer_content_x() reports zero
   before the panel's tree control has been created; the value itself carries
   no explorer behaviour. */
extern HWND g_hExplorerTree;

int g_explorerVisible = 0;
int g_explorerWidth = -1;
int g_explorerWidthRatio = 220;

int g_gitPanelVisible = 0;
int g_gitPanelWidth = -1;
int g_gitPanelOnRight = 1;

int explorer_panel_width(int focusModeActive)
{
    if (!g_hExplorerTree || !g_explorerVisible || focusModeActive)
        return 0;
    return (g_explorerWidth >= 0 ? g_explorerWidth : 0) + SPLITTER_WIDTH;
}

int git_panel_width(int focusModeActive)
{
    if (!g_gitPanelVisible || focusModeActive)
        return 0;
    return (g_gitPanelWidth >= 0 ? g_gitPanelWidth : 0) + SPLITTER_WIDTH;
}

/* The x-offset everything beside these panels is shifted by: the explorer's
   own width (it's always docked at the left) plus the Git panel's, when
   that one is also currently docked at the left (0 when it's docked at the
   right instead, hidden, or in focus mode). */
int explorer_content_x(int focusModeActive)
{
    int width = explorer_panel_width(focusModeActive);
    if (!g_gitPanelOnRight) width += git_panel_width(focusModeActive);
    return width;
}
