#ifndef PLUGIN_HOST_INTERNAL_H
#define PLUGIN_HOST_INTERNAL_H

/* Shared declarations for the plugin host split across plugin_lua_runtime.c,
   plugin_registry.c, plugin_discovery.c and plugin_host.c. Not a public
   header: include it from plugin host sources only. */

#include "plugin_host.h"

#include "lua.h"

#define PLUGIN_MAX_SCRIPT_BYTES (1024 * 1024)
#define PLUGIN_MAX_CHOICES 10000
#define PLUGIN_MAX_SPELL_WORDS 50000
#define PLUGIN_MAX_BINARY_BYTES (64 * 1024 * 1024)
#define PLUGIN_MAX_DEFERRED 64
#define PLUGIN_MAX_OUTPUT_TABLE_COLUMNS 256
#define PLUGIN_MAX_OUTPUT_TABLE_ROWS 10000

typedef struct {
    char *label;
    int ref;          /* the command's function, in the Lua registry */
    int placed;       /* by the menu definition, rather than by its label */
    char *overrides;  /* the built-in command it runs for (commands.register), or NULL */
    int builtIn;      /* from a script built into Tinker Editor: no time limit */
    int separator;    /* menu.separator: label is a menu path, ref is LUA_NOREF */
    char *owner;       /* script name, used to namespace config values */
    char *configKey;   /* menu.checkbox's persisted key, or NULL */
    int checkDefault;
    int enabledRef;   /* menu.add's optional enabled predicate, or LUA_NOREF */
    char *shortcut;
    BYTE shortcutFlags;
    WORD shortcutKey;
} PluginCommand;

typedef struct {
    char *name;
    char *scope;
    int enabled;
} PluginExtension;

typedef struct {
    const char *event; /* one of EVENT_NAMES */
    char *label;       /* "script.lua: event", for dialog titles */
    int ref;
    char *owner;
    int id;
} PluginHandler;

typedef struct {
    int token;
    int ref;
    char *owner;
    char *label;
} PluginDeferred;

/* contextmenu.add()'s items. Distinct from PluginContext (require()'s
   per-script module root) below, despite the similar name - this one is the
   editor's right-click popup. */
typedef struct {
    int id;
    char *label;
    int ref;
    int enabledRef;
    char *owner;
    char *configKey;
    int checkDefault;
} PluginContextMenuItem;

typedef struct {
    char *owner;
    WCHAR *moduleRoot;
} PluginContext;

typedef struct { size_t used; size_t limit; } LuaMemory;

/* ---- shared state ---- */

/* plugin_lua_runtime.c */
extern lua_State *s_lua;
extern LuaMemory s_memory;
extern int s_timeLimited;

/* plugin_host.c */
extern PluginHostCallbacks s_callbacks;
extern int s_busy;
extern const char *s_runningLabel;
extern const char *s_runningOwner;
extern const char *s_loadingName;
extern int s_loadingBuiltIn;
extern const WCHAR *s_loadingModuleRoot;

/* plugin_registry.c */
extern PluginCommand s_commands[PLUGIN_MAX_COMMANDS];
extern int s_commandCount;
extern PluginHandler s_handlers[PLUGIN_MAX_HANDLERS];
extern int s_handlerCount;
extern int s_nextHandlerId;
extern PluginDeferred s_deferred[PLUGIN_MAX_DEFERRED];
extern int s_nextDeferredToken;
extern PluginContextMenuItem s_contextMenuItems[PLUGIN_MAX_CONTEXT_ITEMS];
extern int s_contextMenuItemCount;
extern int s_nextContextMenuItemId;

/* plugin_discovery.c */
extern PluginExtension s_extensions[256];
extern int s_extensionCount;
extern PluginContext s_contexts[256];
extern int s_contextCount;
extern WCHAR *s_firstErrorPath;
extern int s_firstErrorLine;

/* ---- plugin_lua_runtime.c ---- */

char *dup_string(const char *text);
void set_chunk_environment(lua_State *L, int index, const char *owner);
const char *checked_cstring(lua_State *L, int index, const char *what);
const char *optional_cstring(lua_State *L, int index, const char *fallback,
                             const char *what);
char *append_line(char *report, char *line);
char *protected_call(int argumentCount);
void restart_time_limit(void);
lua_State *plugin_lua_new_state(LuaMemory *memory);
lua_State *plugin_lua_start(void);
void plugin_lua_protect_string_metatable(lua_State *L);
void plugin_lua_install_hook(lua_State *L);
void plugin_lua_guard_catching(lua_State *L);
void plugin_lua_mark_stopped(lua_State *L);

/* ---- plugin_registry.c ---- */

void discard_commands_from(int firstIndex);
PluginCommand *append_command(lua_State *L, const char *label, int functionIndex,
                              const char *defaultOwner);
void keep_command_string(lua_State *L, char **field, const char *value);
void discard_handlers_from(int firstIndex);
void discard_context_menu_items_from(int firstIndex);
void plugin_registry_open(lua_State *L);

/* ---- plugin_discovery.c ---- */

int remember_context(const char *name, const WCHAR *moduleRoot);
int plugin_require(lua_State *L);

/* ---- plugin_host.c ---- */

char *run_script(const char *name, const char *code, size_t length,
                 lua_CFunction runner, const WCHAR *moduleRoot);

#endif
