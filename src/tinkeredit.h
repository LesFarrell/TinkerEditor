#ifndef TINKEREDIT_H
#define TINKEREDIT_H

#include <windows.h>

#include "settings_values.h" /* SPLITTER_WIDTH */

/* The output panel's tabs (see output_panel.inc). Declared here, rather than
   there, so debugger.inc - included before output_panel.inc in tinkeredit.c's
   unity build - can name OUTPUT_TAB_DEBUG when starting a debug session.
   Git has its own independently-dockable panel (git_panel.inc), not a tab
   here - see g_hGitPanel. */
enum { OUTPUT_TAB_RUN, OUTPUT_TAB_TERMINAL, OUTPUT_TAB_PROBLEMS, OUTPUT_TAB_DEBUG,
      OUTPUT_TAB_COUNT };

/* ---- child control IDs ---- */
#define ID_EDIT    101
#define ID_PREVIEW 102
#define ID_TABS    103
#define ID_STATUS  104
#define ID_OUTLINE 105
#define ID_MINIMAP 106
#define ID_SPLIT_EDIT 107
#define ID_DEBUG_VARIABLES 109
#define ID_DEBUG_STATUS 110
#define ID_OUTPUT_PANEL 112
#define ID_OUTPUT_VIEW 113
#define ID_TERMINAL_VIEW 114
#define ID_OUTPUT_INPUT 115
#define ID_OUTPUT_TAB_RUN 116
#define ID_OUTPUT_TAB_TERMINAL 117
#define ID_OUTPUT_CLEAR 118
#define ID_OUTPUT_STOP 119
#define ID_OUTPUT_CLOSE 120
#define ID_OUTPUT_TAB_DEBUG 121
#define ID_OUTPUT_TAB_PROBLEMS 122
#define ID_GIT_COMMIT 124
#define ID_GIT_STAGE_ALL 125
#define ID_GIT_REFRESH 126
#define ID_GIT_PANEL 127
#define ID_GIT_TREE 128
#define ID_GIT_CLOSE 129
#define ID_GIT_COMMIT_MESSAGE 130
#define ID_GIT_COMMIT_PUSH 131
#define ID_MERGE_PANEL 132
#define ID_MERGE_LIST 133
#define ID_MERGE_VIEW 134
#define ID_MERGE_ACCEPT_OURS 135
#define ID_MERGE_ACCEPT_THEIRS 136
#define ID_MERGE_ACCEPT_BOTH 137
#define ID_MERGE_NEXT 138
#define ID_MERGE_REFRESH 139
#define ID_MERGE_SAVE 140
#define ID_MERGE_CLOSE 141

/* ---- timer IDs ---- */
#define IDT_PREVIEW_UPDATE 1
#define IDT_FILE_WATCH 2
#define IDT_SCROLL_SYNC 3
#define IDT_SPELL_CHECK 4
#define IDT_AUTOSAVE 5
#define IDT_OUTLINE_UPDATE 6
#define IDT_PRINT_PREVIEW 7
#define IDT_DEBUG_LOCATION 8
#define IDT_STATUS_UPDATE 9
#define IDT_EXPLORER_REFRESH 10
#define IDT_HTML_EXPORT 11
#define IDT_PLUGIN_STATUS 12
#define IDT_PLUGIN_SELECTION 13
#define IDT_MINIMAP_UPDATE 14
#define IDT_WORD_COMPLETION 15
#define IDT_PLUGIN_DEFER_BASE 1000

/* ---- timer intervals, in milliseconds ----
   The debounce delays let a burst of typing, scrolling or file changes
   settle into one update; the intervals poll. */
#define PREVIEW_UPDATE_DELAY_MS 150
#define FILE_WATCH_INTERVAL_MS 1500
#define SCROLL_SYNC_DELAY_MS 35              /* after the editor scrolls */
#define SCROLL_SYNC_AFTER_RENDER_DELAY_MS 40 /* after the preview re-renders */
#define SPELL_CHECK_DELAY_MS 350
#define AUTOSAVE_INTERVAL_MS 5000
#define OUTLINE_UPDATE_DELAY_MS 500
#define PRINT_PREVIEW_DELAY_MS 300           /* lets the preview render before printing */
#define DEBUG_LOCATION_POLL_MS 150
#define STATUS_UPDATE_DELAY_MS 200
#define EXPLORER_REFRESH_DELAY_MS 400
#define HTML_EXPORT_TIMEOUT_MS 30000
#define PLUGIN_STATUS_DURATION_MS 1800       /* how long a plugin reload message shows */
#define PLUGIN_SELECTION_DELAY_MS 100
#define MINIMAP_UPDATE_DELAY_MS 1000
#define WORD_COMPLETION_DELAY_MS 120
#define PLUGIN_DEFER_RETRY_MS 50             /* retry while a plugin is still running */
#define DEBUGGER_EXIT_WAIT_MS 1000           /* for a stopped debugger to release its files */

/* ---- window messages and layout metrics ---- */
#define WM_APP_CLOSE_TAB (WM_APP + 1)
#define WM_APP_DEFERRED_INIT (WM_APP + 2)
/* WM_APP + 3 is TINKEREDIT_WM_EXPLORER_CHANGED (file_explorer.h). */
#define WM_APP_PLUGIN_TAB_CHECK (WM_APP + 4)
#define WM_APP_APPLY_LARGE_FILE_MODE (WM_APP + 5)
#define WM_APP_PLUGIN_DOCUMENT_ACTION (WM_APP + 6)
#define WM_APP_OUTPUT (WM_APP + 7) /* an OutputChunk from output_process.c */
#define WM_APP_GIT_STATUS (WM_APP + 8) /* hidden asynchronous git status */
#define WM_APP_GIT_SYNC (WM_APP + 9) /* a quick git command run_git_sync() is waiting on */
#define WM_APP_GIT_PANEL_ACTION (WM_APP + 10) /* a Git panel stage/unstage, deferred past SCN_DOUBLECLICK */
#define WM_APP_GIT_BRANCHES (WM_APP + 11) /* hidden asynchronous branch listing, for the Git panel */
#define TOOLBAR_HEIGHT 32
#define TAB_HEIGHT 28
#define TAB_ITEM_WIDTH 120 /* each document tab, in 96-DPI pixels like the sizes below */
#define TAB_ITEM_HEIGHT 22
#define TAB_TOOLTIP_MAX_WIDTH 700 /* the full-path tooltip wraps past this */
#define DEFAULT_WINDOW_WIDTH 1100  /* first run, before a size is saved */
#define DEFAULT_WINDOW_HEIGHT 750
#define DEBUG_LINE_ALPHA 110       /* the current-line highlight while debugging (0-255) */
#define MINIMAP_WIDTH 120
#define MINIMAP_SPLITTER_WIDTH 1
#define SPLIT_EDITOR_SPLITTER_SIZE 4
#define STATUS_BAR_HEIGHT 22
/* Right edges of the status bar's Ln/Col, word count and encoding parts;
   the save-state part fills the rest. */
#define STATUS_POSITION_PART_RIGHT 150
#define STATUS_COUNT_PART_RIGHT 345
#define STATUS_ENCODING_PART_RIGHT 510
#define EXPLORER_MIN_WIDTH 120
#define OUTLINE_WIDTH 230      /* at most a third of the content width */
#define MIN_PANE_WIDTH 100     /* the narrowest the editor or preview gets beside the other */
#define DEBUG_BUTTON_WIDTH 112
#define DEBUG_CLOSE_BUTTON_WIDTH 30
#define DEBUG_NAME_COLUMN_WIDTH 220
#define DEBUG_VALUE_COLUMN_WIDTH 640
#define OUTPUT_PANEL_HEIGHT 220     /* until the user drags the splitter above it */
#define OUTPUT_PANEL_MIN_HEIGHT 90
#define OUTPUT_PANEL_MIN_SPACE 200  /* below this much height the panel stays hidden */
#define OUTPUT_PANEL_WIDTH 320      /* until the user drags the splitter beside it, when docked at the side */
#define OUTPUT_PANEL_MIN_WIDTH 180
#define OUTPUT_PANEL_MIN_SPACE_X 360 /* below this much width the side-docked panel stays hidden */
#define OUTPUT_PANEL_PADDING 6
#define OUTPUT_HEADER_HEIGHT 30
#define OUTPUT_INPUT_HEIGHT 24
#define OUTPUT_TAB_WIDTH 90
#define OUTPUT_BUTTON_WIDTH 64
/* The Git panel docks at the left or right edge, like the file explorer -
   see g_gitPanelOnRight. */
#define GIT_PANEL_WIDTH 280         /* until the user drags its splitter */
#define GIT_PANEL_MIN_WIDTH 180
#define GIT_COMMIT_MESSAGE_HEIGHT 72 /* the commit message box, a few lines tall */
/* The Merge Conflicts panel always docks at the right edge (see
   layout_merge_panel() in merge_panel.inc). */
#define MERGE_PANEL_WIDTH 340
#define MERGE_PANEL_MIN_WIDTH 240
#define MERGE_LIST_HEIGHT 90       /* the conflicted-file list, above the hunk view */

/* Larger custom preview stylesheets are refused rather than loaded. */
#define CUSTOM_CSS_MAX_BYTES (4 * 1024 * 1024)

#include "command_ids.h"

/* ---- resource IDs (tinkeredit.rc) ---- */
#define IDI_APPICON 1
#define IDR_DEFAULT_MENUS 2 /* src/menus.lua, the menu bar used without a user copy */
#define IDR_BUILTIN_FORMATTING_BASE 3 /* src/lua/formatting_base.lua */
#define IDR_HELP_MARKDOWN 4 /* src/help/markdown-reference.md, Help > Markdown Reference */
#define IDR_HELP_SHORTCUTS 5 /* src/help/keyboard-shortcuts.lua, builds Help > Keyboard Shortcuts */
#define IDR_BUILTIN_FORMATTING_TABLES 7
#define IDR_BUILTIN_FORMATTING_TEXT 8
#define IDR_ZIPTOOL_LUA 9 /* src/lua/ziptool.lua, used to package File > Export as Word (.docx) */

/* ---- forward declarations for functions defined in tinkeredit.c ----
   Grouped roughly by area: preview/theme, menu/layout, spell check,
   status/outline, recovery, dialogs, and document/tab management. */
/* Nonzero on success; zero on cancellation or write failure. */
static int do_save(void);
static void update_preview(void);
static void show_image_file_preview(void);
static void push_html_to_webview(const char *htmlFragment);
static void apply_preview_theme(void);
static void apply_preview_theme(void);
static void apply_editor_theme(HWND hEdit);
static const WCHAR *split_document_path(void);
static void apply_preview_host_theme(void);
static void refresh_menu(HWND hwnd);
static void layout(HWND hwnd);
static void update_view_menu(HWND hwnd);
static void schedule_spell_check(void);
static void schedule_visible_spell_check(void);
static void perform_spell_check(void);
static void update_status_position(void);
static void update_status_bar(void);
static void show_plugin_reload_status(const WCHAR *text);
static void refresh_git_status(void);
static void refresh_plugin_performance_problems(void);
static void write_recovery_snapshot(void);
static void close_recovery_session(void);
static int restore_recovery_snapshot(void);
static int show_choice_dialog(const WCHAR *title, const WCHAR *prompt,
                              WCHAR **labels, int count, const WCHAR *okLabel);
static void fire_plugin_event(const char *event, const WCHAR *path);
static int show_text_input(const WCHAR *title, const WCHAR *prompt,
                           const WCHAR *initial, WCHAR *output, int outputCapacity);
static void toggle_preview(HWND hwnd);
static void update_document_mapping(void);
static void update_title(void);
static void set_dirty(int dirty);
static void update_tab_label(int index);
struct Document;
static void format_document_label(const struct Document *doc, WCHAR *label, size_t capacity);
static int focus_outside_editor_area(void);
static void switch_document(int index);
static int split_document_index(void);
static void show_split_document_dialog(void);
static void show_command_palette(void);
static void do_save_tab_at(int index);
static void do_saveas_tab_at(int index);
static int create_document_tab(void);
static int active_document_is_reusable(void);
static void check_external_file_change(void);
static void prompt_reload_active_external_change(void);
static void prompt_reload_external_change(int index);
static void do_close_document_at(int index);
static void do_reopen_closed_tab(void);
static void save_session_documents(void);
static void restore_session_documents(void);
static void sync_preview_to_editor_scroll(void);
static void apply_custom_preview_css(int reportErrors);
static int paste_image_from_clipboard(void);
static const char *builtin_script(int id, size_t *length);
static void apply_large_file_mode_after_growth(int documentIndex);
static void invalidate_markdown_workspace_cache(void);
static WCHAR *resolve_workspace_wikilink(const WCHAR *name, int chooseAmbiguous,
                                         int *ambiguous);
static int update_wikilink_completions(HWND hEdit);

#endif
