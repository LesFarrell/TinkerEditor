#ifndef TINKEREDIT_FILE_EXPLORER_H
#define TINKEREDIT_FILE_EXPLORER_H

#include <windows.h>

#include "layout_metrics.h" /* explorer geometry values + explorer_content_x() */

/* Posted (with no payload - the receiver drains file_explorer.c's own
   pending-directories queue) by the watcher thread whenever it observes a
   change under the current root. tinkeredit.c's WndProc handles this by
   (re)starting a short debounce timer rather than refreshing immediately,
   so a burst of changes (e.g. a git operation) collapses into one refresh. */
#define TINKEREDIT_WM_EXPLORER_CHANGED (WM_APP + 3)

/* Called with the absolute path of a file the user double-clicked/pressed
   Enter on/chose "Open" for in the tree. tinkeredit.c wires this to
   open_file_in_tab(), which is static to tinkeredit.c's own translation unit
   and so cannot be called by name from here. */
typedef void (*ExplorerOpenFileFn)(const WCHAR *path);

/* Matches show_text_input()'s exact signature (also static to tinkeredit.c) so
   tinkeredit.c can hand explorer_init() that function's address directly, with
   no wrapper glue, for the New File/New Folder/Rename name prompts. */
typedef int (*ExplorerPromptTextFn)(const WCHAR *title, const WCHAR *prompt,
                                    const WCHAR *initial, WCHAR *output, int outputCapacity);

/* Shows the given path selected in Windows Explorer - reused from
   tinkeredit.c's own "Show in Explorer" command (reveal_path_in_explorer())
   rather than file_explorer.c reimplementing the same ShellExecuteW call. */
typedef void (*ExplorerRevealFn)(const WCHAR *path);

/* Reports a successful filesystem rename so open tabs and Markdown links can
   be retargeted by the application layer. */
typedef void (*ExplorerRenameFn)(const WCHAR *oldPath, const WCHAR *newPath, int isDirectory);

/* Requests that the application adopt a different workspace root. The
   explorer uses this for its parent-folder action; the application owns the
   persistence and recent-workspace bookkeeping associated with that change. */
typedef void (*ExplorerChangeRootFn)(const WCHAR *path);

/* Called when a file dragged out of the explorer tree is released over some
   other window, with the text file_explorer.c built for it (currently always
   a "[[Note Name]]" wikilink - see wikilink_text_for_path() in file_explorer.c)
   and the drop point in screen coordinates. tinkeredit.c decides whether that
   point is actually over the editor and, if so, inserts the text there. */
typedef void (*ExplorerDropTextFn)(const WCHAR *text, POINT screenPt);

/* Creates the explorer's TreeView child control (initially hidden) and
   records the callbacks used to reach tinkeredit.c's static functions. Call
   once, from initialize_main_window(). */
void explorer_init(HWND hParent, ExplorerOpenFileFn openFile, ExplorerPromptTextFn promptText,
                   ExplorerRevealFn revealPath, ExplorerRenameFn renamedPath,
                   ExplorerChangeRootFn changeRoot, ExplorerDropTextFn dropText);

/* Re-applies dark/light native theming (scrollbars, background/text colour)
   to the tree control. Call whenever the editor theme (g_editorDarkMode) changes. */
void explorer_apply_theme(void);

/* (Re)points the tree at a new root folder, repopulating it and restarting
   the live file-system watcher. Safe to call whether or not a watcher is
   already running for a previous root. */
void explorer_set_root(const WCHAR *rootPath);

/* Stops the background watcher thread (if any) without discarding the tree's
   current contents/expansion state. Call when the panel is hidden. */
void explorer_stop_watch(void);

/* Full teardown: stops+joins the watcher thread and frees all tree item
   data. Call once from destroy_main_window(). */
void explorer_shutdown(void);

/* Routes a WM_NOTIFY from the tree control. tinkeredit.c calls this from its
   own WM_NOTIFY handler whenever notification->hwndFrom == g_hExplorerTree. */
void explorer_handle_notify(NMHDR *notification);

/* Handles explorer-specific keys before the application's accelerator table
   sees them. This keeps conventional TreeView actions such as F2 (rename)
   and F5 (refresh) local to the explorer while it has focus. */
int explorer_pretranslate_message(const MSG *message);

/* Drains the pending-changed-directories queue built up by the watcher
   thread and refreshes just the affected, currently-loaded tree nodes.
   tinkeredit.c calls this from its TINKEREDIT_WM_EXPLORER_CHANGED debounce
   timer (IDT_EXPLORER_REFRESH). */
void explorer_handle_change_message(void);

/* The explorer geometry values and explorer_content_x() now live in
   layout_metrics.h (included above), which file_explorer.h re-exports so
   existing users keep seeing them through this header. */

extern HWND g_hExplorerTree;
/* The close button above the tree (see explorer_init() in file_explorer.c).
   tinkeredit.c owner-draws it (handle_owner_draw()) and positions it
   (layout_explorer() in ui_shell.inc); clicking it sends WM_COMMAND
   IDM_VIEW_EXPLORER, the same as the View menu's toggle. */
extern HWND g_hExplorerClose;

#endif
