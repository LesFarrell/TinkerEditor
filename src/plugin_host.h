#ifndef TINKEREDIT_PLUGIN_HOST_H
#define TINKEREDIT_PLUGIN_HOST_H

#include <stddef.h>
#include <windows.h>

/* Shared with the built-in formatting Lua chunk, which registers about 50, and
   with functions in the menu definition. Command IDs run from
   IDM_PLUGIN_COMMAND_0 (2000) upwards, so this stays well clear of the
   system command range. */
#define PLUGIN_MAX_COMMANDS 1024
#define PLUGIN_MAX_HANDLERS 100
/* contextmenu.add()'s items; see also EDITOR_CONTEXT_MENU_PLUGIN_ITEM_MAX in
   editor_input.inc, which caps how many of these a single right-click popup
   actually shows. */
#define PLUGIN_MAX_CONTEXT_ITEMS 256
#define PLUGIN_MAX_CONTEXT_DEPTH 8
/* A plugin call (or a script's top-level code) is stopped after this long. */
#define PLUGIN_TIME_LIMIT_MS 5000
/* Total heap available to the sandboxed Lua state. This includes all loaded
   plugins and modules, and prevents a script from exhausting the process. */
#define PLUGIN_MEMORY_LIMIT_BYTES (64u * 1024u * 1024u)

#define TINKEREDIT_PLUGIN_API_VERSION 12

/* One cell of an output.table() grid. text is UTF-8, already validated;
   isNumber marks a cell that should sort/compare numerically rather than as
   text (number is only meaningful then). */
typedef struct {
    const char *text;
    int isNumber;
    double number;
} PluginTableCell;

/* What the plugin API needs from the application. Any member may be NULL,
   in which case the matching Lua function raises an error. All text is UTF-8. */
typedef struct {
    /* Asks for a line of text; returns it (heap, caller frees) or NULL when
       cancelled. */
    char *(*prompt)(const char *title, const char *label, const char *initial);
    /* Returns the 0-based index of the chosen item, or -1 when cancelled. */
    int (*choose)(const char *title, const char *label, const char *const *items, int count);
    /* Path of the document being edited (heap, caller frees), or NULL when
       it has never been saved. */
    char *(*file_path)(void);
    /* Lower-case language name of the document being edited, e.g. "markdown". */
    const char *(*language)(void);
    /* Shows a message; kind is "info", "warning" or "error". */
    void (*message)(const char *title, const char *text, const char *kind);
    /* Reads/writes a value in this plugin's private persistent settings.
       config_get returns a heap string, or NULL when absent. config_set
       receives NULL to delete a value and returns nonzero on success. */
    char *(*config_get)(const char *plugin, const char *key);
    int (*config_set)(const char *plugin, const char *key, const char *value);
    /* Whether a discovered extension should load. scope is supplied by the
       application (for example "bundled" or "user"). */
    int (*plugin_enabled)(const char *scope, const char *name);
    /* Shows a Save As dialog initialized with defaultName, then writes data
       byte-for-byte. When openAfterSave is nonzero, asks Windows to open the
       saved file with its associated application, or with application when
       that UTF-8 path is non-NULL. arguments are placed before the quoted
       saved path. Returns 1 on success, 0 when cancelled, -1 on a write
       error, or -2 when opening fails. */
    int (*save_binary)(const char *title, const char *defaultName,
                       const char *data, size_t length, int openAfterSave,
                       const char *application, const char *arguments);
    /* Optional richer host services. Strings returned by a callback are heap
       allocated and are freed by the plugin host. */
    int (*confirm)(const char *title, const char *text);
    void (*status)(const char *text);
    void (*log)(const char *plugin, const char *text);
    char *(*pick_path)(const char *title, int folder);
    int (*invoke_command)(const char *name);
    int (*document_count)(void);
    int (*document_active)(void); /* zero based */
    int (*document_activate)(int index);
    int (*document_new)(void);
    int (*document_open)(const char *path);
    int (*document_save)(int saveAs);
    int (*document_close)(int index);
    char *(*document_path)(int index);
    int (*document_modified)(int index);
    /* Whether a document reloads changes on disk without asking (while it
       has no unsaved edits). value is 0 or 1 to set it, or -1 to only ask;
       returns the setting afterwards. */
    int (*document_auto_reload)(int index, int value);
    const char *(*document_encoding)(void);
    /* Changes the active document's save encoding. The value is one of
       "utf-8", "utf-8-bom", "utf-16le", "utf-16be" or "ansi". */
    int (*document_set_encoding)(const char *encoding);
    char *(*workspace_path)(void);
    /* Schedules or cancels a one-shot callback on the UI thread. */
    int (*schedule_deferred)(int token, unsigned delayMs, int cancel);
    /* The Windows spelling dictionary's locale name, e.g. "en-GB", or NULL
       when there is none. */
    char *(*spell_language)(void);
    /* Sets misspelled[i] to 1 for each of words[0..count) the dictionary
       rejects, and to 0 otherwise. Returns 0 when checking isn't possible. */
    int (*spell_check)(const char *const *words, int count, unsigned char *misspelled);
    /* The output panel's Output tab: output_write appends UTF-8 text (ANSI
       colour sequences allowed), output_clear empties it, output_show opens
       the panel on it, output_hide closes the panel and output_visible
       reports whether it is open. */
    void (*output_write)(const char *text);
    void (*output_clear)(void);
    void (*output_show)(void);
    void (*output_hide)(void);
    int (*output_visible)(void);
    /* A sortable grid on the output panel's Table tab, opened the same way
       output_show opens the Run tab. cells is rowCount * columnCount,
       row-major; columns/cells are only valid for the call's duration, so
       the callback must copy whatever it keeps. */
    void (*output_table)(const char *const *columns, int columnCount,
                          const PluginTableCell *cells, int rowCount);
    /* An interactive in-process session on the Terminal tab. */
    int (*terminal_begin)(void);
    void (*terminal_write)(const char *text);
    char *(*terminal_read)(const char *prompt, int *interrupted);
    void (*terminal_clear)(void);
    void (*terminal_end)(void);
    /* Replaces a named producer's text in the Problems tab. Empty text
       removes that producer without disturbing other diagnostics. */
    void (*problems_report)(const char *source, const char *text);
    void (*problems_clear_all)(void);
} PluginHostCallbacks;

/* Copied; call before running commands. */
void plugin_host_set_callbacks(const PluginHostCallbacks *callbacks);
/* Enables Ctrl+C interruption while an in-process terminal command owns the
   plugin runtime. The application bridge brackets terminal sessions with it. */
void plugin_host_set_ctrl_c_interrupt(int enabled);

/* ---- menu definitions (menus.lua) ---- */

typedef enum {
    PLUGIN_MENU_SEPARATOR = 0,
    PLUGIN_MENU_COMMAND,  /* a built-in command, by name */
    PLUGIN_MENU_FUNCTION, /* a Lua function, run as plugin command `command` */
    PLUGIN_MENU_SUBMENU,
    PLUGIN_MENU_LIST      /* generated items, by name, e.g. "recent_files" */
} PluginMenuKind;

typedef struct PluginMenuItem {
    PluginMenuKind kind;
    char *label;  /* UTF-8; NULL for separators and lists */
    char *name;   /* the command or list name */
    int command;
    struct PluginMenuItem *items; /* a submenu's contents */
    int itemCount;
} PluginMenuItem;

/* Runs a menu definition: a script returning the menu bar as a table of
   menus (see src/menus.lua for the format). Its functions become commands
   that plugin_host_command_is_placed() reports as already placed. Command
   and list names are not checked here. Call after plugin_host_shutdown(),
   before loading plugins. */
char *plugin_host_load_menu(const char *name, const char *code, size_t length);
/* The loaded definition's top-level menus, or NULL (count 0) if none. */
const PluginMenuItem *plugin_host_menu(int *count);

/* Functions returning char * give a heap-allocated UTF-8 error report
   (caller frees), or NULL on success. */

/* ---- generated pages (src/help/keyboard-shortcuts.lua) ---- */

/* A built-in command's shortcut text, for a page script. */
typedef struct {
    const char *name;     /* e.g. "file.new" */
    const char *shortcut; /* e.g. "Ctrl+N" */
} PluginShortcut;

/* Runs a built-in page script in a Lua state of its own (so plugins can't
   affect it) and sets *page to the Markdown it returns (heap, caller frees;
   NULL on error). The script's one argument is a table: `menus`, the loaded
   menu definition, and `shortcuts`, name -> shortcut text. */
char *plugin_host_render_page(const char *name, const char *code, size_t length,
                              const PluginShortcut *shortcuts, int shortcutCount, char **page);

/* Called for each colour a themes.lua sets: dark is 0 for the light theme.
   Returns NULL when the colour was applied, or why it wasn't. */
typedef const char *(*PluginThemeColourFn)(void *context, int dark, const char *name, const char *value);

/* Runs a themes.lua in a Lua state of its own. The file returns
   { light = { name = "#RRGGBB", ... }, dark = { ... } }; each colour goes to
   setColour. Returns NULL, or a report of every problem (heap, caller frees).
   Colours before a Lua error may already have been applied. */
char *plugin_host_load_theme(const char *name, const char *code, size_t length,
                             PluginThemeColourFn setColour, void *context);

/* Runs every top-level *.lua file and folder containing init.lua in name
   order, adding to what is loaded. scope identifies the source to the
   enable-state callback. A missing directory is not an error. */
char *plugin_host_load_directory(const WCHAR *directory, const char *scope);
/* Runs one plugin script's top-level code, starting the Lua state if needed. */
char *plugin_host_load_chunk(const char *name, const char *code, size_t length);
/* The same, for a script built into Tinker Editor: its commands have no time limit. */
char *plugin_host_load_builtin(const char *name, const char *code, size_t length);
/* Discards every command, handler and the menu definition. Does nothing
   while plugin code is running. */
void plugin_host_shutdown(void);
/* Whether plugin code is running (it may be waiting in a dialog), so
   plugin_host_run() and plugin_host_fire() would do nothing now. */
int plugin_host_is_busy(void);

int plugin_host_command_count(void);
/* UTF-8 menu label of a registered command. For one from the menu
   definition it is the item's path, e.g. "/File/Insert Date". */
const char *plugin_host_command_label(int index);
/* Optional shortcut registered by menu.add/menu.checkbox, e.g. "Ctrl+Alt+J". */
const char *plugin_host_command_shortcut(int index);
/* Parsed accelerator for that shortcut. Returns zero when it has none. */
int plugin_host_command_accelerator(int index, BYTE *flags, WORD *key);
/* Whether the menu definition already placed the command, so it shouldn't
   be placed again by its label. */
int plugin_host_command_is_placed(int index);
/* Whether the entry is a menu.separator(path) rather than a command: its
   label is the menu path and it has nothing to run. Separators share the
   command list so they keep their order
   among the commands placed around them. */
int plugin_host_command_is_separator(int index);
/* Whether a command was registered with menu.checkbox, and its persisted
   state. A missing/unavailable setting uses the checkbox's default. */
int plugin_host_command_is_checkable(int index);
int plugin_host_command_is_checked(int index);
/* Whether menu.add supplied an enabled predicate, and its current value.
   Predicates run with editor APIs bound to hEdit. A failing one is disabled
   and returns an allocated error for the caller to report and free. */
int plugin_host_command_has_enabled(int index);
char *plugin_host_command_enabled(int index, HWND hEdit, int *enabled);
/* Owner/key metadata for menu.checkbox settings; NULL for other commands. */
const char *plugin_host_command_checkbox_owner(int index);
const char *plugin_host_command_checkbox_key(int index);
/* Finds the menu.checkbox command that owns a plugin config key, or -1. */
int plugin_host_find_checkbox(const char *owner, const char *key);
/* The built-in command a command was registered to run for, or NULL. */
const char *plugin_host_command_overrides(int index);

/* ---- editor right-click context-menu items (contextmenu.add/remove) ---- */
int plugin_host_context_menu_item_count(void);
/* UTF-8 label of the item at index, 0 <= index < plugin_host_context_menu_item_count(). */
const char *plugin_host_context_menu_item_label(int index);
/* The item's stable id, as returned to Lua by contextmenu.add() - unlike
   index, it stays valid across a later add/remove (even one from a deferred
   callback firing while a popup built from an earlier snapshot is still
   open: see editor_input.inc's show_editor_context_menu()). */
int plugin_host_context_menu_item_id(int index);
/* Whether the item came from contextmenu.checkbox, and its current persisted state. */
int plugin_host_context_menu_item_is_checkable(int index);
int plugin_host_context_menu_item_is_checked(int index);
/* Evaluates the optional enabled predicate with editor APIs bound to hEdit.
   The item is enabled when it has no predicate. On failure it is disabled
   and an allocated error is returned for the caller to display and free. */
char *plugin_host_context_menu_item_enabled(int index, HWND hEdit, int *enabled);
/* Runs the item with this id, the same way a menu command runs (a single
   undo step, rolled back on failure). Does nothing and returns NULL if no
   item with this id is registered any more. */
char *plugin_host_run_context_menu_item(int id, HWND hEdit);

/* Extensions discovered by the directory loaders, including disabled ones. */
int plugin_host_extension_count(void);
const char *plugin_host_extension_name(int index);
const char *plugin_host_extension_scope(int index);
int plugin_host_extension_is_enabled(int index);

/* First script which failed during the current load, for actionable UI. */
const WCHAR *plugin_host_error_path(void);
int plugin_host_error_line(void);
/* The command registered (last) to run for the built-in command called
   name, or -1 when there is none. */
int plugin_host_find_override(const char *name);
/* Runs a command against hEdit as a single undo step; edits it made are
   undone if it fails. */
char *plugin_host_run(int index, HWND hEdit);
/* Runs every handler registered with events.on(event, fn), in registration
   order, each the way a command runs. argument is passed to them (nil when
   NULL). Does nothing while plugin code is already running, since events
   can arrive from a message loop a plugin dialog is running. */
char *plugin_host_fire(const char *event, const char *argument, HWND hEdit);
/* Runs a one-shot callback scheduled by app.defer(). */
char *plugin_host_run_deferred(int token, HWND hEdit);

/* Aggregated wall-free CPU time and retained-memory deltas for Lua callbacks.
   The returned report is heap allocated. warningsOnly keeps callbacks that
   failed, used >=50 ms once, or retained >=1 MiB in one call. */
char *plugin_host_performance_report(int warningsOnly);
void plugin_host_clear_performance(void);

#endif
