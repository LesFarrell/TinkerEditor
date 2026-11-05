/* Lua extension host: owns the sandboxed state, registrations, extension
   discovery and folder-local module loading. Application services arrive
   through callbacks; editor operations are delegated to lua_editor_api.c.
   The state, registrations and discovery live in sibling modules; this
   file keeps the entry points, the menu definition and page rendering. */
#include "plugin_host.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "plugin_host_internal.h"

#include "lua_editor_api.h"
#include "plugin_sqlite.h"
#include "plugin_zip.h"

#include "lauxlib.h"
#include "lualib.h"

PluginHostCallbacks s_callbacks;
/* Set while any plugin code runs, so nothing re-enters the Lua state from
   a message loop (a plugin dialog, say) mid-call. */
int s_busy = 0;
const char *s_runningLabel = NULL;
const char *s_runningOwner = NULL;
/* The script whose top-level code is running, while it loads, and whether
   it is one built into Tinker Editor. */
const char *s_loadingName = NULL;
int s_loadingBuiltIn = 0;
const WCHAR *s_loadingModuleRoot = NULL;

void plugin_host_set_callbacks(const PluginHostCallbacks *callbacks) {
    if (callbacks) s_callbacks = *callbacks;
    else memset(&s_callbacks, 0, sizeof(s_callbacks));
    lua_editor_api_set_callbacks(callbacks);
}

/* ---- menu definitions ---- */

#define PLUGIN_MENU_MAX_DEPTH 8
#define PLUGIN_MENU_MAX_ITEMS 4000

static PluginMenuItem *s_menu = NULL;
static int s_menuCount = 0;
/* While a definition converts: the labels leading to the current item, for
   error messages and function commands' labels, and how many items so far. */
static const char *s_menuPath[PLUGIN_MENU_MAX_DEPTH];
static int s_menuItemsSeen = 0;

static void free_menu_items(PluginMenuItem *items, int count) {
    for (int i = 0; i < count; i++) {
        free(items[i].label);
        free(items[i].name);
        free_menu_items(items[i].items, items[i].itemCount);
    }
    free(items);
}

static void free_menu(void) {
    free_menu_items(s_menu, s_menuCount);
    s_menu = NULL;
    s_menuCount = 0;
}

/* Raises "File > Recent Files: problem" for an item at the given depth. */
static int menu_error(lua_State *L, int depth, const char *problem) {
    luaL_Buffer buffer;
    luaL_buffinit(L, &buffer);
    for (int i = 0; i < depth; i++) {
        if (i) luaL_addstring(&buffer, " > ");
        luaL_addstring(&buffer, s_menuPath[i]);
    }
    luaL_addstring(&buffer, depth ? ": " : "the menu bar: ");
    luaL_addstring(&buffer, problem);
    luaL_pushresult(&buffer);
    return lua_error(L);
}

static char *checked_copy(lua_State *L, const char *value) {
    char *copy = dup_string(value);
    if (!copy) luaL_error(L, "out of memory");
    return copy;
}

/* Registers the function on the stack top as a command placed at the
   current path plus label, e.g. "/File/Insert Date". */
static int register_menu_function(lua_State *L, int depth, const char *label) {
    if (s_commandCount >= PLUGIN_MAX_COMMANDS)
        return menu_error(L, depth, "too many commands");
    luaL_Buffer buffer;
    luaL_buffinit(L, &buffer);
    for (int i = 0; i < depth; i++) {
        luaL_addchar(&buffer, '/');
        luaL_addstring(&buffer, s_menuPath[i]);
    }
    luaL_addchar(&buffer, '/');
    luaL_addstring(&buffer, label);
    luaL_pushresult(&buffer);
    append_command(L, lua_tostring(L, -1), lua_absindex(L, -2), "menus.lua")->placed = 1;
    lua_pop(L, 1);
    return s_commandCount - 1;
}

/* Converts the item array at the stack top. *outItems is set before it is
   filled, so a conversion stopped by an error leaves a tree free_menu()
   can release. Raw access throughout, so no metamethod runs. */
static void convert_menu_items(lua_State *L, PluginMenuItem **outItems, int *outCount, int depth) {
    if (depth >= PLUGIN_MENU_MAX_DEPTH) menu_error(L, depth, "menus are nested too deeply");
    lua_Integer count = (lua_Integer)lua_rawlen(L, -1);
    if (count == 0) return;
    if (count > PLUGIN_MENU_MAX_ITEMS - s_menuItemsSeen) menu_error(L, depth, "too many menu items");
    s_menuItemsSeen += (int)count;
    PluginMenuItem *items = (PluginMenuItem *)calloc((size_t)count, sizeof(*items));
    if (!items) luaL_error(L, "out of memory");
    *outItems = items;
    *outCount = (int)count;
    for (lua_Integer i = 0; i < count; i++) {
        PluginMenuItem *item = &items[i];
        int type = lua_rawgeti(L, -1, i + 1);
        if (type == LUA_TSTRING) {
            const char *separator = checked_cstring(L, -1, "menu item");
            if (strcmp(separator, "-") != 0 || depth == 0)
                menu_error(L, depth, depth ? "a text item must be \"-\" (a separator)"
                                           : "each item must be a menu: { label, { items } }");
            item->kind = PLUGIN_MENU_SEPARATOR;
            lua_pop(L, 1);
            continue;
        }
        if (type != LUA_TTABLE) menu_error(L, depth, "each item must be a table or \"-\"");
        lua_pushliteral(L, "list");
        if (lua_rawget(L, -2) == LUA_TSTRING) {
            if (depth == 0) menu_error(L, depth, "a list must be inside a menu");
            item->kind = PLUGIN_MENU_LIST;
            item->name = checked_copy(L, checked_cstring(L, -1, "menu list name"));
            lua_pop(L, 2);
            continue;
        }
        lua_pop(L, 1);
        if (lua_rawgeti(L, -1, 1) != LUA_TSTRING)
            menu_error(L, depth, "an item needs a text label first: { label, action }");
        const char *label = checked_cstring(L, -1, "menu label"); /* kept alive by the item table */
        item->label = checked_copy(L, label);
        lua_pop(L, 1);
        int actionType = lua_rawgeti(L, -1, 2);
        if (depth == 0 && actionType != LUA_TTABLE) {
            s_menuPath[0] = label;
            menu_error(L, 1, "a top-level item must be a menu: { label, { items } }");
        }
        if (actionType == LUA_TSTRING) {
            item->kind = PLUGIN_MENU_COMMAND;
            item->name = checked_copy(L, checked_cstring(L, -1, "menu command name"));
        } else if (actionType == LUA_TFUNCTION) {
            item->kind = PLUGIN_MENU_FUNCTION;
            item->command = register_menu_function(L, depth, label);
        } else if (actionType == LUA_TTABLE) {
            item->kind = PLUGIN_MENU_SUBMENU;
            s_menuPath[depth] = label;
            convert_menu_items(L, &item->items, &item->itemCount, depth + 1);
        } else {
            s_menuPath[depth] = label;
            menu_error(L, depth + 1, "the action must be a command name, a function or a table of items");
        }
        lua_pop(L, 2);
    }
}

/* Called with the compiled definition: runs it, then converts its result. */
static int run_menu_definition(lua_State *L) {
    lua_call(L, 0, 1);
    if (!lua_istable(L, -1)) return luaL_error(L, "a menu definition must return a table of menus");
    s_menuItemsSeen = 0;
    convert_menu_items(L, &s_menu, &s_menuCount, 0);
    return 0;
}

/* Opens `libraries` (which must include the base library) with the base
   library's file and bytecode access taken away. Every Lua state that runs a
   user-editable script goes through this: plugins, themes.lua and the
   keyboard-shortcuts page. Call it in protected mode, since any of this can
   fail for lack of memory. */
static void open_safe_libraries(lua_State *L, const luaL_Reg *libraries, size_t count) {
    for (size_t i = 0; i < count; i++) {
        luaL_requiref(L, libraries[i].name, libraries[i].func, 1);
        lua_pop(L, 1);
    }
    /* dofile/loadfile read files; load is limited to source text, since
       malformed precompiled chunks can corrupt memory. env is passed on as
       varargs: load() tells an absent env (use the globals) from a nil one
       (no globals at all), so it must stay absent when the caller omits it. */
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    static const char textOnlyLoad[] =
        "local load = load\n"
        "_G.load = function(chunk, name, _, ...) return load(chunk, name, 't', ...) end\n";
    if (luaL_loadbufferx(L, textOnlyLoad, sizeof(textOnlyLoad) - 1, "=tinkeredit", "t") != LUA_OK)
        lua_error(L);
    lua_call(L, 0, 0);
    plugin_lua_guard_catching(L);
}

/* Stops a themes.lua or shortcuts page that never finishes, after about ten
   million instructions - far more than building colours or a page needs. */
enum { SCRIPT_HOOK_INTERVAL = 10000, SCRIPT_HOOK_LIMIT = 1000 };
static int s_scriptHookCalls;

static void script_instruction_limit(lua_State *L, lua_Debug *ar) {
    (void)ar;
    if (++s_scriptHookCalls > SCRIPT_HOOK_LIMIT) {
        plugin_lua_mark_stopped(L);
        luaL_error(L, "stopped because it did not finish");
    }
}

static void install_instruction_limit(lua_State *L) {
    s_scriptHookCalls = 0;
    lua_sethook(L, script_instruction_limit, LUA_MASKCOUNT, SCRIPT_HOOK_INTERVAL);
}

/* Only libraries without file system or process access are opened. */
static int open_environment(lua_State *L) {
    static const luaL_Reg libraries[] = {
        {LUA_GNAME, luaopen_base},
        {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    open_safe_libraries(L, libraries, sizeof(libraries) / sizeof(libraries[0]));
    plugin_lua_protect_string_metatable(L);

    lua_editor_api_push(L);
    lua_setglobal(L, "editor");
    plugin_registry_open(L);
    plugin_zip_install(L); /* installs `zip`, or leaves it undefined - see plugin_zip.c */
    plugin_sqlite_install(L); /* installs `sqlite`, or leaves it undefined - see plugin_sqlite.c */
    lua_pushcfunction(L, plugin_require);
    lua_setglobal(L, "require");
    plugin_lua_install_hook(L);
    return 0;
}

static char *ensure_state(void) {
    if (s_lua) return NULL;
    if (!plugin_lua_start()) return dup_string("Lua could not start: out of memory");
    lua_pushcfunction(s_lua, open_environment);
    char *error = protected_call(0);
    if (error) {
        lua_close(s_lua);
        s_lua = NULL;
    }
    return error;
}

/* Gives the compiled chunk below the owner's name its environment, then runs
   it (or the runner below it, with the chunk). Done under protection, since
   building a plugin's environment can run out of memory. */
static int run_in_environment(lua_State *L) {
    const char *name = (const char *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    set_chunk_environment(L, -1, name);
    lua_call(L, lua_gettop(L) - 1, 0);
    return 0;
}

/* Runs a script's top-level code, or with runner given, calls runner with
   the compiled script instead. */
char *run_script(const char *name, const char *code, size_t length,
                 lua_CFunction runner, const WCHAR *moduleRoot) {
    if (s_busy) return dup_string("Plugins can't be loaded while a plugin is running.");
    char *error = ensure_state();
    if (error) return error;
    int firstCommand = s_commandCount, firstHandler = s_handlerCount;
    int firstContextMenuItem = s_contextMenuItemCount;
    char chunkName[260];
    snprintf(chunkName, sizeof(chunkName), "=%s", name);
    int pushed = runner ? 3 : 2; /* run_in_environment, the runner and the chunk */
    lua_pushcfunction(s_lua, run_in_environment);
    if (runner) lua_pushcfunction(s_lua, runner);
    if (luaL_loadbufferx(s_lua, code, length, chunkName, "t") != LUA_OK) {
        error = dup_string(lua_tostring(s_lua, -1));
        lua_pop(s_lua, pushed);
    } else {
        if (!remember_context(name, moduleRoot)) {
            lua_pop(s_lua, pushed);
            return dup_string("out of memory recording plugin context");
        }
        lua_pushlightuserdata(s_lua, (void *)name);
        s_busy = 1;
        s_loadingName = name;
        s_loadingModuleRoot = moduleRoot;
        error = protected_call(pushed);
        s_loadingModuleRoot = NULL;
        s_loadingName = NULL;
        s_busy = 0;
    }
    /* A script that fails part-way contributes none of its commands or handlers. */
    if (error) {
        discard_commands_from(firstCommand);
        discard_handlers_from(firstHandler);
        discard_context_menu_items_from(firstContextMenuItem);
    }
    return error;
}

char *plugin_host_load_chunk(const char *name, const char *code, size_t length) {
    return run_script(name, code, length, NULL, NULL);
}

char *plugin_host_load_builtin(const char *name, const char *code, size_t length) {
    s_loadingBuiltIn = 1;
    char *error = run_script(name, code, length, NULL, NULL);
    s_loadingBuiltIn = 0;
    return error;
}

char *plugin_host_load_menu(const char *name, const char *code, size_t length) {
    /* run_script() would refuse too, but only after the current definition
       had already been thrown away. */
    if (s_busy) return dup_string("Plugins can't be loaded while a plugin is running.");
    free_menu();
    char *error = run_script(name, code, length, run_menu_definition, NULL);
    if (error) free_menu();
    return error;
}

const PluginMenuItem *plugin_host_menu(int *count) {
    *count = s_menuCount;
    return s_menu;
}


/* ---- generated pages ---- */

typedef struct {
    const char *name;
    const char *code;
    size_t length;
    const PluginShortcut *shortcuts;
    int shortcutCount;
} PageJob;

/* Pushes menu definition items as a Lua list of { kind = ..., ... } tables
   (the shape src/help/keyboard-shortcuts.lua describes). */
static void push_menu_items(lua_State *L, const PluginMenuItem *items, int count) {
    static const char *const KINDS[] = { "separator", "command", "function", "submenu", "list" };
    luaL_checkstack(L, 4, "menu definition nested too deeply");
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        const PluginMenuItem *item = &items[i];
        lua_createtable(L, 0, 3);
        lua_pushstring(L, KINDS[item->kind]);
        lua_setfield(L, -2, "kind");
        if (item->label) {
            lua_pushstring(L, item->label);
            lua_setfield(L, -2, "label");
        }
        if (item->name) {
            lua_pushstring(L, item->name);
            lua_setfield(L, -2, item->kind == PLUGIN_MENU_LIST ? "list" : "command");
        }
        if (item->kind == PLUGIN_MENU_SUBMENU) {
            push_menu_items(L, item->items, item->itemCount);
            lua_setfield(L, -2, "items");
        }
        lua_rawseti(L, -2, i + 1);
    }
}

/* Run in protected mode, since any of it can fail for lack of memory:
   opens the libraries, compiles the script and calls it with its data. */
static int render_page(lua_State *L) {
    const PageJob *job = (const PageJob *)lua_touserdata(L, 1);
    static const luaL_Reg libraries[] = {
        {LUA_GNAME, luaopen_base},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    open_safe_libraries(L, libraries, sizeof(libraries) / sizeof(libraries[0]));
    char chunkName[260];
    snprintf(chunkName, sizeof(chunkName), "=%s", job->name);
    if (luaL_loadbufferx(L, job->code, job->length, chunkName, "t") != LUA_OK) return lua_error(L);

    lua_createtable(L, 0, 2);
    push_menu_items(L, s_menu, s_menuCount);
    lua_setfield(L, -2, "menus");
    lua_createtable(L, 0, job->shortcutCount);
    for (int i = 0; i < job->shortcutCount; i++) {
        lua_pushstring(L, job->shortcuts[i].shortcut);
        lua_setfield(L, -2, job->shortcuts[i].name);
    }
    lua_setfield(L, -2, "shortcuts");

    lua_call(L, 1, 1);
    if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "%s must return the page as a string", job->name);
    return 1;
}

char *plugin_host_render_page(const char *name, const char *code, size_t length,
                              const PluginShortcut *shortcuts, int shortcutCount, char **page) {
    *page = NULL;
    LuaMemory memory;
    lua_State *L = plugin_lua_new_state(&memory);
    if (!L) return dup_string("Lua could not start: out of memory");
    PageJob job = { name, code, length, shortcuts, shortcutCount };
    install_instruction_limit(L);
    lua_pushcfunction(L, render_page);
    lua_pushlightuserdata(L, &job);
    char *error = NULL;
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        error = dup_string(message ? message : "the page script failed");
    } else {
        size_t size;
        const char *text = lua_tolstring(L, -1, &size);
        *page = (char *)malloc(size + 1);
        if (*page) {
            memcpy(*page, text, size);
            (*page)[size] = 0;
        } else {
            error = dup_string("out of memory");
        }
    }
    lua_close(L);
    return error;
}

/* ---- themes.lua ----
   Runs in its own Lua state, like the shortcuts page: a colour file needs
   nothing but tables and strings, and must not see or change plugin state.
   Both states have the plugins' memory limit. */

typedef struct ThemeJob {
    const char *name, *code;
    size_t length;
    PluginThemeColourFn setColour;
    void *context;
    char *report;
} ThemeJob;

static void add_theme_problem(ThemeJob *job, const char *theme, const char *key, const char *problem) {
    char line[256];
    snprintf(line, sizeof(line), "%s: %s%s%s: %s", job->name, theme, key ? "." : "", key ? key : "", problem);
    size_t used = job->report ? strlen(job->report) : 0;
    char *grown = (char *)realloc(job->report, used + strlen(line) + 2);
    if (!grown) return;
    if (used) grown[used++] = '\n';
    strcpy(grown + used, line);
    job->report = grown;
}

/* Passes each colour of one theme's table (at the top of the stack) to the
   callback, noting anything that isn't a "name = string" pair. */
static void read_theme_colours(lua_State *L, ThemeJob *job, const char *theme, int dark) {
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            add_theme_problem(job, theme, NULL, "colour names must be strings");
        } else if (lua_type(L, -1) != LUA_TSTRING) {
            add_theme_problem(job, theme, lua_tostring(L, -2), "the colour must be a string like \"#1F2328\"");
        } else {
            const char *problem = job->setColour(job->context, dark, lua_tostring(L, -2), lua_tostring(L, -1));
            if (problem) add_theme_problem(job, theme, lua_tostring(L, -2), problem);
        }
        lua_pop(L, 1);
    }
}

static int run_theme_file(lua_State *L) {
    ThemeJob *job = (ThemeJob *)lua_touserdata(L, 1);
    static const luaL_Reg libraries[] = {
        {LUA_GNAME, luaopen_base},
        {LUA_STRLIBNAME, luaopen_string},
    };
    open_safe_libraries(L, libraries, sizeof(libraries) / sizeof(libraries[0]));
    char chunkName[260];
    snprintf(chunkName, sizeof(chunkName), "=%s", job->name);
    if (luaL_loadbufferx(L, job->code, job->length, chunkName, "t") != LUA_OK) return lua_error(L);
    lua_call(L, 0, 1);
    if (!lua_istable(L, -1)) return luaL_error(L, "%s must return a table of themes", job->name);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        const char *theme = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
        int dark = theme && strcmp(theme, "dark") == 0;
        if (!theme || (!dark && strcmp(theme, "light") != 0))
            add_theme_problem(job, theme ? theme : "?", NULL, "only \"light\" and \"dark\" themes exist");
        else if (!lua_istable(L, -1))
            add_theme_problem(job, theme, NULL, "must be a table of colours");
        else
            read_theme_colours(L, job, theme, dark);
        lua_pop(L, 1);
    }
    return 0;
}

char *plugin_host_load_theme(const char *name, const char *code, size_t length,
                             PluginThemeColourFn setColour, void *context) {
    LuaMemory memory;
    lua_State *L = plugin_lua_new_state(&memory);
    if (!L) return dup_string("Lua could not start: out of memory");
    ThemeJob job = { name, code, length, setColour, context, NULL };
    install_instruction_limit(L);
    lua_pushcfunction(L, run_theme_file);
    lua_pushlightuserdata(L, &job);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        free(job.report);
        job.report = dup_string(message ? message : "the theme file failed");
    }
    lua_close(L);
    return job.report;
}

void plugin_host_shutdown(void) {
    if (s_busy) return;
    for (int i = 0; i < PLUGIN_MAX_DEFERRED; i++) {
        if (!s_deferred[i].token) continue;
        if (s_callbacks.schedule_deferred)
            s_callbacks.schedule_deferred(s_deferred[i].token, 0, 1);
        if (s_lua) luaL_unref(s_lua, LUA_REGISTRYINDEX, s_deferred[i].ref);
        free(s_deferred[i].owner); free(s_deferred[i].label);
        memset(&s_deferred[i], 0, sizeof(s_deferred[i]));
    }
    free_menu();
    discard_commands_from(0);
    discard_handlers_from(0);
    discard_context_menu_items_from(0);
    for (int i = 0; i < s_extensionCount; i++) {
        free(s_extensions[i].name);
        free(s_extensions[i].scope);
    }
    s_extensionCount = 0;
    for (int i = 0; i < s_contextCount; i++) {
        free(s_contexts[i].owner);
        free(s_contexts[i].moduleRoot);
    }
    s_contextCount = 0;
    free(s_firstErrorPath);
    s_firstErrorPath = NULL;
    s_firstErrorLine = 0;
    if (s_lua) {
        lua_close(s_lua);
        s_lua = NULL;
    }
    s_memory.used = 0;
    plugin_host_clear_performance();
}

int plugin_host_is_busy(void) {
    return s_busy;
}
