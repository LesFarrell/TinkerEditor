/* Lua extension host: the API plugins see (ui.*, menu.*, events.*,
   commands.*, config.*, app.*, documents.*; editor.* lives in
   lua_editor_api.c), the command and handler registries, and running a
   registered command or event. */
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
#include "text_codec.h"
#include "../third_party/scintilla/include/Scintilla.h"

#include "lauxlib.h"
#include "lualib.h"

static const char *const EVENT_NAMES[] = {
    "before_save", "save", "after_save", "open", "close", "tab_changed",
    "text_changed", "selection_changed", "workspace_changed", "theme_changed", "shutdown",
    "reload"
};

static const char *known_event(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(EVENT_NAMES) / sizeof(EVENT_NAMES[0]); i++)
        if (strcmp(name, EVENT_NAMES[i]) == 0) return EVENT_NAMES[i];
    return NULL;
}

PluginCommand s_commands[PLUGIN_MAX_COMMANDS];
int s_commandCount = 0;
PluginHandler s_handlers[PLUGIN_MAX_HANDLERS];
int s_handlerCount = 0;
int s_nextHandlerId = 1;
PluginDeferred s_deferred[PLUGIN_MAX_DEFERRED];
int s_nextDeferredToken = 1;
PluginContextMenuItem s_contextMenuItems[PLUGIN_MAX_CONTEXT_ITEMS];
int s_contextMenuItemCount = 0;
int s_nextContextMenuItemId = 1;

static const char *current_owner(lua_State *L);

/* Whether a stored config value reads as true: "1", "true", "yes" or "on". */
static int config_value_is_true(const char *value) {
    return strcmp(value, "1") == 0 || _stricmp(value, "true") == 0 ||
           _stricmp(value, "yes") == 0 || _stricmp(value, "on") == 0;
}

void discard_commands_from(int first) {
    for (int i = first; i < s_commandCount; i++) {
        if (s_lua) luaL_unref(s_lua, LUA_REGISTRYINDEX, s_commands[i].ref);
        if (s_lua && s_commands[i].enabledRef != LUA_NOREF)
            luaL_unref(s_lua, LUA_REGISTRYINDEX, s_commands[i].enabledRef);
        free(s_commands[i].label);
        free(s_commands[i].overrides);
        free(s_commands[i].owner);
        free(s_commands[i].configKey);
        free(s_commands[i].shortcut);
        memset(&s_commands[i], 0, sizeof(s_commands[i]));
    }
    if (first < s_commandCount) s_commandCount = first;
}

/* Appends a command called label, owned by the loading script (or
   defaultOwner), that runs the function at functionIndex - or nothing, for
   a separator, when functionIndex is 0. Raises a Lua error, appending
   nothing, when there's no room or memory. Callers set the remaining fields,
   copying strings with keep_command_string(). */
PluginCommand *append_command(lua_State *L, const char *label, int functionIndex,
                              const char *defaultOwner) {
    if (s_commandCount >= PLUGIN_MAX_COMMANDS)
        luaL_error(L, "too many plugin commands (the limit is %d)", PLUGIN_MAX_COMMANDS);
    int ref = LUA_NOREF;
    if (functionIndex) {
        lua_pushvalue(L, functionIndex);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    char *copy = dup_string(label);
    char *owner = dup_string(s_loadingName ? s_loadingName : defaultOwner);
    if (!copy || !owner) {
        free(copy);
        free(owner);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        luaL_error(L, "out of memory");
    }
    PluginCommand *command = &s_commands[s_commandCount++];
    memset(command, 0, sizeof(*command));
    command->label = copy;
    command->ref = ref;
    command->enabledRef = LUA_NOREF;
    command->owner = owner;
    command->builtIn = s_loadingBuiltIn;
    command->separator = !functionIndex;
    return command;
}

/* Sets one of the newest command's strings to a copy of value, discarding
   the command when memory runs out. */
void keep_command_string(lua_State *L, char **field, const char *value) {
    *field = dup_string(value);
    if (!*field) {
        discard_commands_from(s_commandCount - 1);
        luaL_error(L, "out of memory");
    }
}

void discard_handlers_from(int first) {
    for (int i = first; i < s_handlerCount; i++) {
        if (s_lua) luaL_unref(s_lua, LUA_REGISTRYINDEX, s_handlers[i].ref);
        free(s_handlers[i].label);
        free(s_handlers[i].owner);
        memset(&s_handlers[i], 0, sizeof(s_handlers[i]));
    }
    if (first < s_handlerCount) s_handlerCount = first;
}

static void free_context_menu_item(PluginContextMenuItem *item) {
    if (s_lua) luaL_unref(s_lua, LUA_REGISTRYINDEX, item->ref);
    if (s_lua && item->enabledRef != LUA_NOREF)
        luaL_unref(s_lua, LUA_REGISTRYINDEX, item->enabledRef);
    free(item->label);
    free(item->owner);
    free(item->configKey);
    memset(item, 0, sizeof(*item));
}

void discard_context_menu_items_from(int first) {
    for (int i = first; i < s_contextMenuItemCount; i++)
        free_context_menu_item(&s_contextMenuItems[i]);
    if (first < s_contextMenuItemCount) s_contextMenuItemCount = first;
}

static const char *current_title(void) {
    return s_runningLabel ? s_runningLabel : "Tinker Editor Plugin";
}

static int ui_message(lua_State *L) {
    const char *text = checked_cstring(L, 1, "message text");
    const char *title = optional_cstring(L, 2, current_title(), "message title");
    static const char *const kinds[] = { "info", "warning", "error", NULL };
    const char *kind = kinds[luaL_checkoption(L, 3, "info", kinds)];
    if (!s_callbacks.message) return luaL_error(L, "ui.message is not available here");
    s_callbacks.message(title, text, kind);
    restart_time_limit();
    return 0;
}

static int ui_prompt(lua_State *L) {
    lua_editor_api_require_editor(L);
    const char *label = checked_cstring(L, 1, "prompt label");
    const char *initial = optional_cstring(L, 2, "", "prompt default");
    const char *title = optional_cstring(L, 3, current_title(), "prompt title");
    if (!s_callbacks.prompt) return luaL_error(L, "ui.prompt is not available here");
    char *answer = s_callbacks.prompt(title, label, initial);
    restart_time_limit();
    if (answer) lua_pushstring(L, answer);
    else lua_pushnil(L);
    free(answer);
    return 1;
}

static int ui_choose(lua_State *L) {
    lua_editor_api_require_editor(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    const char *label = optional_cstring(L, 2, "Choose an item:", "choice label");
    const char *title = optional_cstring(L, 3, current_title(), "choice title");
    lua_Integer count = (lua_Integer)lua_rawlen(L, 1);
    if (count < 1 || count > PLUGIN_MAX_CHOICES)
        return luaL_error(L, "ui.choose needs between 1 and %d items", PLUGIN_MAX_CHOICES);
    if (!s_callbacks.choose) return luaL_error(L, "ui.choose is not available here");
    /* Userdata, so the array is freed even if a bad item raises an error.
       The pointers stay valid because the table keeps the strings alive, which
       is why the items are read raw: an __index result could be collected. */
    const char **items = (const char **)lua_newuserdatauv(L, (size_t)count * sizeof(*items), 0);
    for (lua_Integer i = 0; i < count; i++) {
        if (lua_rawgeti(L, 1, i + 1) != LUA_TSTRING)
            return luaL_error(L, "ui.choose item %d is not a string", (int)(i + 1));
        items[i] = checked_cstring(L, -1, "ui.choose item");
        lua_pop(L, 1);
    }
    int chosen = s_callbacks.choose(title, label, items, (int)count);
    restart_time_limit();
    if (chosen < 0 || chosen >= count) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, chosen + 1);
    lua_rawgeti(L, 1, chosen + 1);
    return 2;
}

static int ui_save_binary(lua_State *L) {
    lua_editor_api_require_editor(L);
    const char *defaultName = checked_cstring(L, 1, "default file name");
    size_t length = 0;
    const char *data = luaL_checklstring(L, 2, &length);
    const char *title = optional_cstring(L, 3, current_title(), "dialog title");
    int openAfterSave = 0;
    if (!lua_isnoneornil(L, 4)) {
        luaL_checktype(L, 4, LUA_TBOOLEAN);
        openAfterSave = lua_toboolean(L, 4);
    }
    const char *application = optional_cstring(L, 5, NULL, "application path");
    const char *arguments = optional_cstring(L, 6, NULL, "application arguments");
    if (application && !application[0]) application = NULL;
    if (arguments && !arguments[0]) arguments = NULL;
    if ((application || arguments) && !openAfterSave)
        return luaL_argerror(L, 4, "must be true when an application is supplied");
    if (arguments && !application)
        return luaL_argerror(L, 6, "application arguments require an application path");
    if (!defaultName[0]) return luaL_argerror(L, 1, "default file name must not be empty");
    if (length > PLUGIN_MAX_BINARY_BYTES)
        return luaL_error(L, "binary output exceeds the %d MiB limit",
                          PLUGIN_MAX_BINARY_BYTES / (1024 * 1024));
    if (!s_callbacks.save_binary)
        return luaL_error(L, "ui.save_binary is not available here");
    int result = s_callbacks.save_binary(title, defaultName, data, length, openAfterSave,
                                         application, arguments);
    restart_time_limit();
    if (result == -2)
        return luaL_error(L, "the binary file was saved but could not be opened with its associated application");
    if (result < 0) return luaL_error(L, "the binary file could not be written");
    lua_pushboolean(L, result > 0);
    return 1;
}

static int ui_confirm(lua_State *L) {
    lua_editor_api_require_editor(L);
    const char *text = checked_cstring(L, 1, "confirmation text");
    const char *title = optional_cstring(L, 2, current_title(), "confirmation title");
    if (!s_callbacks.confirm) return luaL_error(L, "ui.confirm is not available here");
    int answer = s_callbacks.confirm(title, text);
    restart_time_limit();
    lua_pushboolean(L, answer);
    return 1;
}

static int ui_status(lua_State *L) {
    const char *value = checked_cstring(L, 1, "status text");
    if (!s_callbacks.status) return luaL_error(L, "ui.status is not available here");
    s_callbacks.status(value);
    return 0;
}

static int ui_log(lua_State *L) {
    const char *value = checked_cstring(L, 1, "log text");
    if (s_callbacks.log) s_callbacks.log(current_owner(L), value);
    else OutputDebugStringA(value);
    return 0;
}

static int ui_pick_path(lua_State *L, int folder) {
    lua_editor_api_require_editor(L);
    const char *title = optional_cstring(L, 1,
        folder ? "Choose Folder" : "Open File", "dialog title");
    if (!s_callbacks.pick_path) return luaL_error(L, "path picker is not available here");
    char *path = s_callbacks.pick_path(title, folder);
    restart_time_limit();
    if (path) lua_pushstring(L, path); else lua_pushnil(L);
    free(path);
    return 1;
}

static int ui_pick_file(lua_State *L) { return ui_pick_path(L, 0); }
static int ui_pick_folder(lua_State *L) { return ui_pick_path(L, 1); }

static int parse_shortcut(const char *value, BYTE *flags, WORD *key) {
    char copy[64];
    strcpy(copy, value);
    *flags = FVIRTKEY;
    *key = 0;
    int ctrlOrAlt = 0;
    char *context = NULL;
    for (char *part = strtok_s(copy, "+", &context); part;
         part = strtok_s(NULL, "+", &context)) {
        if (_stricmp(part, "Ctrl") == 0) { *flags |= FCONTROL; ctrlOrAlt = 1; }
        else if (_stricmp(part, "Alt") == 0) { *flags |= FALT; ctrlOrAlt = 1; }
        else if (_stricmp(part, "Shift") == 0) *flags |= FSHIFT;
        else if (!*key && strlen(part) == 1 && isalnum((unsigned char)part[0]))
            *key = (WORD)toupper((unsigned char)part[0]);
        else if (!*key && (part[0] == 'F' || part[0] == 'f')) {
            char *end = NULL;
            long number = strtol(part + 1, &end, 10);
            if (!end || *end || number < 1 || number > 24) return 0;
            *key = (WORD)(VK_F1 + number - 1);
        } else if (!*key && _stricmp(part, "Tab") == 0) *key = VK_TAB;
        else if (!*key && _stricmp(part, "Space") == 0) *key = VK_SPACE;
        else if (!*key && _stricmp(part, "Insert") == 0) *key = VK_INSERT;
        else if (!*key && _stricmp(part, "Delete") == 0) *key = VK_DELETE;
        else return 0;
    }
    return ctrlOrAlt && *key;
}

/* The validated shortcut text at index, or NULL when it's omitted. */
static const char *checked_shortcut(lua_State *L, int index, BYTE *flags, WORD *key) {
    if (lua_isnoneornil(L, index)) return NULL;
    const char *value = checked_cstring(L, index, "shortcut");
    size_t length = strlen(value);
    if (!length || length > 48) luaL_argerror(L, index, "shortcut must be 1 to 48 characters");
    if (!parse_shortcut(value, flags, key))
        luaL_argerror(L, index, "shortcut needs Ctrl or Alt, optional Shift, and one supported key");
    return value;
}

/* A command placed by its label, running the function at functionIndex,
   with the optional shortcut at shortcutIndex. */
static PluginCommand *add_labelled_command(lua_State *L, const char *label, int functionIndex,
                                           int shortcutIndex) {
    BYTE shortcutFlags = 0;
    WORD shortcutKey = 0;
    const char *shortcut = checked_shortcut(L, shortcutIndex, &shortcutFlags, &shortcutKey);
    PluginCommand *command = append_command(L, label, functionIndex, "plugin");
    if (shortcut) {
        keep_command_string(L, &command->shortcut, shortcut);
        command->shortcutFlags = shortcutFlags;
        command->shortcutKey = shortcutKey;
    }
    return command;
}

static int menu_add(lua_State *L) {
    const char *label = checked_cstring(L, 1, "menu label");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!lua_isnoneornil(L, 4)) luaL_checktype(L, 4, LUA_TFUNCTION);
    PluginCommand *command = add_labelled_command(L, label, 2, 3);
    if (!lua_isnoneornil(L, 4)) {
        lua_pushvalue(L, 4);
        command->enabledRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

static const char *checked_config_key(lua_State *L, int index) {
    size_t length = 0;
    const char *key = luaL_checklstring(L, index, &length);
    luaL_argcheck(L, length > 0 && length <= 64, index, "config key must be 1 to 64 characters");
    for (size_t i = 0; i < length; i++)
        luaL_argcheck(L, isalnum((unsigned char)key[i]) || key[i] == '_' ||
                      key[i] == '-' || key[i] == '.', index,
                      "config key may contain only letters, digits, '.', '_' and '-'");
    return key;
}

/* menu.checkbox(label, key, default, fn): a checkable command whose state is
   stored automatically. fn receives the new boolean state after each click. */
static int menu_checkbox(lua_State *L) {
    const char *label = checked_cstring(L, 1, "menu label");
    const char *key = checked_config_key(L, 2);
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    luaL_checktype(L, 4, LUA_TFUNCTION);
    PluginCommand *command = add_labelled_command(L, label, 4, 5);
    keep_command_string(L, &command->configKey, key);
    command->checkDefault = lua_toboolean(L, 3);
    return 0;
}

/* menu.separator([path]): a separator after whatever this plugin and those
   before it have put in the menu path names so far - the Plugins menu when
   path is omitted, "Lines" its Lines submenu, "/Snippets/Markdown" a
   submenu of a menu-bar menu. */
static int menu_separator(lua_State *L) {
    const char *path = optional_cstring(L, 1, "", "menu path");
    append_command(L, path, 0, "plugin");
    return 0;
}

/* contextmenu.add(label, fn): adds an item to the editor's right-click menu
   and returns a stable handle accepted by contextmenu.remove(). */
static PluginContextMenuItem *append_context_menu_item(lua_State *L,
                                                       const char *label,
                                                       int functionIndex,
                                                       int enabledIndex) {
    int parts = 0, inPart = 0;
    for (const char *c = label; ; c++) {
        if (*c && *c != '/') inPart = 1;
        else {
            if (inPart) parts++;
            inPart = 0;
            if (!*c) break;
        }
    }
    if (!parts) {
        luaL_argerror(L, 1, "context menu label must contain a name");
        return NULL;
    }
    if (parts > PLUGIN_MAX_CONTEXT_DEPTH) {
        luaL_argerror(L, 1, "context menu label has too many parts");
        return NULL;
    }
    if (s_contextMenuItemCount >= PLUGIN_MAX_CONTEXT_ITEMS) {
        luaL_error(L, "too many context menu items (the limit is %d)",
                   PLUGIN_MAX_CONTEXT_ITEMS);
        return NULL;
    }

    const char *owner = current_owner(L);
    char *labelCopy = dup_string(label);
    char *ownerCopy = dup_string(owner);
    if (!labelCopy || !ownerCopy) {
        free(labelCopy);
        free(ownerCopy);
        luaL_error(L, "out of memory");
        return NULL;
    }
    lua_pushvalue(L, functionIndex);
    PluginContextMenuItem *item = &s_contextMenuItems[s_contextMenuItemCount++];
    memset(item, 0, sizeof(*item));
    item->id = s_nextContextMenuItemId;
    item->label = labelCopy;
    item->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    item->enabledRef = LUA_NOREF;
    if (!lua_isnoneornil(L, enabledIndex)) {
        lua_pushvalue(L, enabledIndex);
        item->enabledRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    item->owner = ownerCopy;
    s_nextContextMenuItemId = s_nextContextMenuItemId == INT_MAX
        ? 1 : s_nextContextMenuItemId + 1;
    return item;
}

static int contextmenu_add(lua_State *L) {
    const char *label = checked_cstring(L, 1, "context menu label");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!lua_isnoneornil(L, 3)) luaL_checktype(L, 3, LUA_TFUNCTION);
    PluginContextMenuItem *item = append_context_menu_item(L, label, 2, 3);
    lua_pushinteger(L, item->id);
    return 1;
}

/* contextmenu.checkbox(label, key, default, fn): the context-menu equivalent
   of menu.checkbox, including automatic persistence and rollback on error. */
static int contextmenu_checkbox(lua_State *L) {
    const char *label = checked_cstring(L, 1, "context menu label");
    const char *key = checked_config_key(L, 2);
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    luaL_checktype(L, 4, LUA_TFUNCTION);
    if (!lua_isnoneornil(L, 5)) luaL_checktype(L, 5, LUA_TFUNCTION);
    PluginContextMenuItem *item = append_context_menu_item(L, label, 4, 5);
    item->configKey = dup_string(key);
    if (!item->configKey) {
        discard_context_menu_items_from(s_contextMenuItemCount - 1);
        return luaL_error(L, "out of memory");
    }
    item->checkDefault = lua_toboolean(L, 3);
    lua_pushinteger(L, item->id);
    return 1;
}

static int contextmenu_remove(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    const char *owner = current_owner(L);
    for (int i = 0; i < s_contextMenuItemCount; i++) {
        PluginContextMenuItem *item = &s_contextMenuItems[i];
        if (item->id != id || strcmp(item->owner, owner) != 0) continue;
        free_context_menu_item(item);
        memmove(item, item + 1,
                (size_t)(s_contextMenuItemCount - i - 1) * sizeof(*item));
        s_contextMenuItemCount--;
        memset(&s_contextMenuItems[s_contextMenuItemCount], 0, sizeof(*item));
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

/* commands.register(name, fn [, title]): fn runs in place of the built-in
   command called name, wherever it is invoked from (menu, toolbar
   or shortcut). The last registration for a name wins. title names it in
   dialogs. Whether name is a real command is up to the application. */
static int commands_register(lua_State *L) {
    const char *name = checked_cstring(L, 1, "command name");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const char *title = optional_cstring(L, 3, "Tinker Editor", "command title");
    PluginCommand *command = append_command(L, title, 2, "plugin");
    command->placed = 1;
    keep_command_string(L, &command->overrides, name);
    return 0;
}

/* events.on(name, fn): fn(path) runs on each such event. */
static int events_on(lua_State *L) {
    const char *name = checked_cstring(L, 1, "event name");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const char *event = known_event(name);
    if (!event) return luaL_error(L, "unknown event '%s'", name);
    if (s_handlerCount >= PLUGIN_MAX_HANDLERS)
        return luaL_error(L, "too many event handlers (the limit is %d)", PLUGIN_MAX_HANDLERS);
    const char *owner = s_loadingName ? s_loadingName
                                      : s_runningOwner ? s_runningOwner : current_title();
    char *label = (char *)malloc(strlen(owner) + strlen(event) + 3);
    if (!label) return luaL_error(L, "out of memory");
    sprintf(label, "%s: %s", owner, event);
    lua_pushvalue(L, 2);
    s_handlers[s_handlerCount].event = event;
    s_handlers[s_handlerCount].label = label;
    s_handlers[s_handlerCount].ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s_handlers[s_handlerCount].owner = dup_string(owner);
    if (!s_handlers[s_handlerCount].owner) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_handlers[s_handlerCount].ref);
        free(s_handlers[s_handlerCount].label);
        return luaL_error(L, "out of memory");
    }
    s_handlers[s_handlerCount].id = s_nextHandlerId;
    s_nextHandlerId = s_nextHandlerId == INT_MAX ? 1 : s_nextHandlerId + 1;
    lua_pushinteger(L, s_handlers[s_handlerCount].id);
    s_handlerCount++;
    return 1;
}

static int events_off(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    const char *owner = current_owner(L);
    for (int i = 0; i < s_handlerCount; i++) {
        if (s_handlers[i].id != id || strcmp(s_handlers[i].owner, owner) != 0) continue;
        luaL_unref(L, LUA_REGISTRYINDEX, s_handlers[i].ref);
        free(s_handlers[i].label);
        free(s_handlers[i].owner);
        memmove(&s_handlers[i], &s_handlers[i + 1],
                (size_t)(s_handlerCount - i - 1) * sizeof(s_handlers[0]));
        s_handlerCount--;
        memset(&s_handlers[s_handlerCount], 0, sizeof(s_handlers[0]));
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

static const char *current_owner(lua_State *L) {
    const char *owner = s_loadingName ? s_loadingName : s_runningOwner;
    if (!owner) luaL_error(L, "config is only available while loading or running a plugin");
    return owner;
}

#define CONFIG_TABLE_PREFIX "@ME1:"
#define CONFIG_MAX_DEPTH 32

/* The encoder walks the table with lua_next/lua_rawgeti, which push values,
   so it can't write into a luaL_Buffer as it goes: a buffer requires the
   stack to stay balanced between its calls, and once it grows its box sits
   at the top. Each piece is stored in a Lua table instead (GC-owned, so a
   luaL_error mid-walk leaks nothing) and joined once at the end. */
typedef struct {
    lua_State *L;
    int parts;          /* absolute stack index of the pieces table */
    lua_Integer count;
} ConfigEncoder;

static void add_config_piece(ConfigEncoder *encoder, const char *text, size_t length) {
    lua_pushlstring(encoder->L, text, length);
    lua_rawseti(encoder->L, encoder->parts, ++encoder->count);
}

static void encode_config_value(ConfigEncoder *encoder, int index, int depth) {
    lua_State *L = encoder->L;
    if (depth > CONFIG_MAX_DEPTH) luaL_error(L, "config table is nested too deeply");
    luaL_checkstack(L, 4, "config table is nested too deeply");
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
        case LUA_TNIL: add_config_piece(encoder, "N", 1); return;
        case LUA_TBOOLEAN:
            add_config_piece(encoder, lua_toboolean(L, index) ? "B1" : "B0", 2); return;
        case LUA_TNUMBER: {
            char number[64];
            if (lua_isinteger(L, index))
                snprintf(number, sizeof(number), "I%lld;", (long long)lua_tointeger(L, index));
            else {
                double value = (double)lua_tonumber(L, index);
                if (!isfinite(value)) luaL_error(L, "config numbers must be finite");
                snprintf(number, sizeof(number), "D%.17g;", value);
            }
            add_config_piece(encoder, number, strlen(number)); return;
        }
        case LUA_TSTRING: {
            static const char HEX[] = "0123456789abcdef";
            size_t length;
            const unsigned char *value = (const unsigned char *)lua_tolstring(L, index, &length);
            char size[32]; snprintf(size, sizeof(size), "S%zu:", length);
            add_config_piece(encoder, size, strlen(size));
            /* Nothing else is pushed while this buffer is open. */
            luaL_Buffer buffer;
            char *hex = luaL_buffinitsize(L, &buffer, length * 2);
            for (size_t i = 0; i < length; i++) {
                hex[i * 2] = HEX[value[i] >> 4];
                hex[i * 2 + 1] = HEX[value[i] & 15];
            }
            luaL_pushresultsize(&buffer, length * 2);
            lua_rawseti(L, encoder->parts, ++encoder->count);
            return;
        }
        case LUA_TTABLE: {
            size_t arrayLength = lua_rawlen(L, index), count = 0;
            int array = 1;
            lua_pushnil(L);
            while (lua_next(L, index)) {
                count++;
                if (!lua_isinteger(L, -2)) array = 0;
                else {
                    lua_Integer key = lua_tointeger(L, -2);
                    if (key < 1 || (size_t)key > arrayLength) array = 0;
                }
                lua_pop(L, 1);
                if (count > 10000) luaL_error(L, "config table has too many entries");
            }
            if (array && count == arrayLength) {
                char header[32]; snprintf(header, sizeof(header), "A%zu;", arrayLength);
                add_config_piece(encoder, header, strlen(header));
                for (size_t i = 1; i <= arrayLength; i++) {
                    lua_rawgeti(L, index, (lua_Integer)i);
                    encode_config_value(encoder, -1, depth + 1);
                    lua_pop(L, 1);
                }
            } else {
                char header[32]; snprintf(header, sizeof(header), "O%zu;", count);
                add_config_piece(encoder, header, strlen(header));
                lua_pushnil(L);
                while (lua_next(L, index)) {
                    if (lua_type(L, -2) != LUA_TSTRING)
                        luaL_error(L, "config object keys must be strings");
                    encode_config_value(encoder, -2, depth + 1);
                    encode_config_value(encoder, -1, depth + 1);
                    lua_pop(L, 1);
                }
            }
            return;
        }
        default: luaL_error(L, "config values cannot contain %s", luaL_typename(L, index));
    }
}

static int decode_size(const char **cursor, const char *end, char delimiter, size_t *value) {
    size_t result = 0;
    const char *p = *cursor;
    if (p >= end || !isdigit((unsigned char)*p)) return 0;
    while (p < end && isdigit((unsigned char)*p)) {
        size_t digit = (size_t)(*p++ - '0');
        if (result > (SIZE_MAX - digit) / 10) return 0;
        result = result * 10 + digit;
    }
    if (p >= end || *p++ != delimiter) return 0;
    *cursor = p; *value = result; return 1;
}

static int decode_config_value(lua_State *L, const char **cursor, const char *end, int depth) {
    if (depth > CONFIG_MAX_DEPTH || *cursor >= end) return 0;
    /* Each level holds its table and a key, more than LUA_MINSTACK allows
       across CONFIG_MAX_DEPTH levels. */
    luaL_checkstack(L, 3, "config table is nested too deeply");
    char type = *(*cursor)++;
    if (type == 'N') { lua_pushnil(L); return 1; }
    if (type == 'B' && *cursor < end && (**cursor == '0' || **cursor == '1')) {
        lua_pushboolean(L, *(*cursor)++ == '1'); return 1;
    }
    if (type == 'I' || type == 'D') {
        const char *finish = memchr(*cursor, ';', (size_t)(end - *cursor));
        if (!finish || finish == *cursor || (size_t)(finish - *cursor) >= 64) return 0;
        char number[64]; memcpy(number, *cursor, (size_t)(finish - *cursor));
        number[finish - *cursor] = 0;
        char *parsedEnd = NULL;
        if (type == 'I') {
            long long value = strtoll(number, &parsedEnd, 10);
            if (!parsedEnd || *parsedEnd) return 0;
            lua_pushinteger(L, (lua_Integer)value);
        } else {
            double value = strtod(number, &parsedEnd);
            if (!parsedEnd || *parsedEnd || !isfinite(value)) return 0;
            lua_pushnumber(L, (lua_Number)value);
        }
        *cursor = finish + 1; return 1;
    }
    if (type == 'S') {
        size_t length;
        if (!decode_size(cursor, end, ':', &length) || length > (size_t)(end - *cursor) / 2) return 0;
        luaL_Buffer buffer;
        char *target = luaL_buffinitsize(L, &buffer, length);
        for (size_t i = 0; i < length; i++) {
            int high = hex_digit_value((WCHAR)(*cursor)[i * 2]);
            int low = hex_digit_value((WCHAR)(*cursor)[i * 2 + 1]);
            if (high < 0 || low < 0) return 0;
            target[i] = (char)((high << 4) | low);
        }
        *cursor += length * 2;
        luaL_pushresultsize(&buffer, length);
        return 1;
    }
    if (type == 'A' || type == 'O') {
        size_t count;
        if (!decode_size(cursor, end, ';', &count) || count > 10000) return 0;
        lua_createtable(L, type == 'A' ? (int)count : 0, type == 'O' ? (int)count : 0);
        for (size_t i = 0; i < count; i++) {
            if (type == 'A') {
                if (!decode_config_value(L, cursor, end, depth + 1)) return 0;
                lua_rawseti(L, -2, (lua_Integer)i + 1);
            } else {
                if (!decode_config_value(L, cursor, end, depth + 1) ||
                    lua_type(L, -1) != LUA_TSTRING ||
                    !decode_config_value(L, cursor, end, depth + 1)) return 0;
                lua_rawset(L, -3);
            }
        }
        return 1;
    }
    return 0;
}

static int config_get(lua_State *L) {
    const char *key = checked_config_key(L, 1);
    char *value = s_callbacks.config_get ? s_callbacks.config_get(current_owner(L), key) : NULL;
    if (!value) {
        if (lua_gettop(L) >= 2) lua_pushvalue(L, 2);
        else lua_pushnil(L);
        return 1;
    }
    if (lua_istable(L, 2)) {
        size_t prefix = sizeof(CONFIG_TABLE_PREFIX) - 1, length = strlen(value);
        const char *cursor = value + prefix, *end = value + length;
        if (length < prefix || memcmp(value, CONFIG_TABLE_PREFIX, prefix) != 0 ||
            !decode_config_value(L, &cursor, end, 0) || cursor != end) {
            lua_settop(L, 2);
            lua_pushvalue(L, 2);
        }
    } else if (lua_type(L, 2) == LUA_TBOOLEAN) {
        lua_pushboolean(L, config_value_is_true(value));
    } else if (lua_type(L, 2) == LUA_TNUMBER) {
        char *end = NULL;
        double number = strtod(value, &end);
        if (end && *end == 0) lua_pushnumber(L, number);
        else lua_pushvalue(L, 2);
    } else {
        lua_pushstring(L, value);
    }
    free(value);
    return 1;
}

static int config_set(lua_State *L) {
    const char *key = checked_config_key(L, 1);
    const char *value = NULL;
    char number[64];
    char *structured = NULL;
    switch (lua_type(L, 2)) {
        case LUA_TNIL: break;
        case LUA_TBOOLEAN: value = lua_toboolean(L, 2) ? "1" : "0"; break;
        case LUA_TNUMBER:
            snprintf(number, sizeof(number), "%.17g", (double)lua_tonumber(L, 2));
            value = number;
            break;
        case LUA_TSTRING: value = checked_cstring(L, 2, "config value"); break;
        case LUA_TTABLE: {
            lua_newtable(L);
            ConfigEncoder encoder = { L, lua_gettop(L), 0 };
            add_config_piece(&encoder, CONFIG_TABLE_PREFIX, sizeof(CONFIG_TABLE_PREFIX) - 1);
            encode_config_value(&encoder, 2, 0);
            /* luaL_addvalue takes the value just above the buffer, which is
               the one stack use a luaL_Buffer allows. */
            luaL_Buffer buffer; luaL_buffinit(L, &buffer);
            for (lua_Integer i = 1; i <= encoder.count; i++) {
                lua_rawgeti(L, encoder.parts, i);
                luaL_addvalue(&buffer);
            }
            luaL_pushresult(&buffer);
            structured = dup_string(lua_tostring(L, -1));
            lua_pop(L, 2); /* the joined string and the pieces table */
            if (!structured) return luaL_error(L, "out of memory");
            value = structured;
            break;
        }
        default: return luaL_argerror(L, 2, "expected string, number, boolean or nil");
    }
    if (!s_callbacks.config_set || !s_callbacks.config_set(current_owner(L), key, value)) {
        free(structured);
        return luaL_error(L, "could not write the plugin setting");
    }
    free(structured);
    return 0;
}

/* Safe clock access for plugins. The full Lua os library remains closed
   because it also exposes files, processes, environment variables and locale
   mutation. These fixed formats are suitable for inserting into documents. */
static int app_date(lua_State *L) {
    SYSTEMTIME now;
    char text[11];
    GetLocalTime(&now);
    snprintf(text, sizeof(text), "%04u-%02u-%02u",
             (unsigned)now.wYear, (unsigned)now.wMonth, (unsigned)now.wDay);
    lua_pushstring(L, text);
    return 1;
}

static int app_time(lua_State *L) {
    SYSTEMTIME now;
    char text[9];
    GetLocalTime(&now);
    snprintf(text, sizeof(text), "%02u:%02u:%02u",
             (unsigned)now.wHour, (unsigned)now.wMinute, (unsigned)now.wSecond);
    lua_pushstring(L, text);
    return 1;
}

static int app_date_time(lua_State *L) {
    SYSTEMTIME now;
    char text[20];
    GetLocalTime(&now);
    snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)now.wYear, (unsigned)now.wMonth, (unsigned)now.wDay,
             (unsigned)now.wHour, (unsigned)now.wMinute, (unsigned)now.wSecond);
    lua_pushstring(L, text);
    return 1;
}

static int app_version(lua_State *L) { lua_pushliteral(L, "1.0.0"); return 1; }
static int app_api_version(lua_State *L) {
    lua_pushinteger(L, TINKEREDIT_PLUGIN_API_VERSION);
    return 1;
}
static int app_platform(lua_State *L) { lua_pushliteral(L, "windows"); return 1; }
static int app_memory_used(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)s_memory.used);
    return 1;
}
static int app_traceback(lua_State *L) {
    const char *message = optional_cstring(L, 1, NULL, "traceback message");
    luaL_traceback(L, L, message, 1);
    return 1;
}
static int app_workspace_path(lua_State *L) {
    char *path = s_callbacks.workspace_path ? s_callbacks.workspace_path() : NULL;
    if (path) lua_pushstring(L, path); else lua_pushnil(L);
    free(path);
    return 1;
}
static int app_capabilities(lua_State *L) {
    static const char *const names[] = {
        "plugin_environments", "memory_limit", "lazy_require", "event_unsubscribe",
        "documents", "command_execution", "path_picker", "status", "logging", "defer",
        "eol_conversion", "save_encoding", "tab_width", "spelling", "output_panel",
        "context_menu", "problems", "save_and_open", "configured_runner", "menu_enabled",
        "terminal_panel", "relative_file_read", "output_table"
    };
    lua_createtable(L, 0, (int)(sizeof(names) / sizeof(names[0])));
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, names[i]);
    }
    return 1;
}

static int app_defer(lua_State *L) {
    lua_Integer delay = luaL_checkinteger(L, 1);
    luaL_argcheck(L, delay >= 0 && delay <= 60000, 1, "delay must be from 0 to 60000 ms");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!s_callbacks.schedule_deferred)
        return luaL_error(L, "app.defer is not available here");
    int slot = -1;
    for (int i = 0; i < PLUGIN_MAX_DEFERRED; i++) if (!s_deferred[i].token) { slot = i; break; }
    if (slot < 0) return luaL_error(L, "too many deferred plugin callbacks");
    int token = s_nextDeferredToken;
    s_nextDeferredToken = s_nextDeferredToken == INT_MAX ? 1 : s_nextDeferredToken + 1;
    const char *owner = current_owner(L);
    lua_pushvalue(L, 2);
    s_deferred[slot].ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s_deferred[slot].token = token;
    s_deferred[slot].owner = dup_string(owner);
    s_deferred[slot].label = dup_string(current_title());
    if (!s_deferred[slot].owner || !s_deferred[slot].label ||
        !s_callbacks.schedule_deferred(token, (unsigned)delay, 0)) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_deferred[slot].ref);
        free(s_deferred[slot].owner); free(s_deferred[slot].label);
        memset(&s_deferred[slot], 0, sizeof(s_deferred[slot]));
        return luaL_error(L, "the deferred callback could not be scheduled");
    }
    lua_pushinteger(L, token);
    return 1;
}

static int app_cancel(lua_State *L) {
    int token = (int)luaL_checkinteger(L, 1);
    for (int i = 0; i < PLUGIN_MAX_DEFERRED; i++) {
        if (s_deferred[i].token != token) continue;
        if (strcmp(s_deferred[i].owner, current_owner(L)) != 0) break;
        if (s_callbacks.schedule_deferred) s_callbacks.schedule_deferred(token, 0, 1);
        luaL_unref(L, LUA_REGISTRYINDEX, s_deferred[i].ref);
        free(s_deferred[i].owner); free(s_deferred[i].label);
        memset(&s_deferred[i], 0, sizeof(s_deferred[i]));
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int commands_execute(lua_State *L) {
    const char *name = checked_cstring(L, 1, "command name");
    /* The run.* commands execute the active document; with editor.set_text
       and documents.save that would let a plugin run code of its choosing. */
    if (strncmp(name, "run.", 4) == 0)
        return luaL_error(L, "commands.execute can't run, debug or step through files");
    if (!s_callbacks.invoke_command)
        return luaL_error(L, "commands.execute is not available here");
    lua_pushboolean(L, s_callbacks.invoke_command(name));
    return 1;
}

static int documents_count(lua_State *L) {
    lua_pushinteger(L, s_callbacks.document_count ? s_callbacks.document_count() : 1);
    return 1;
}
static int documents_active(lua_State *L) {
    lua_pushinteger(L, (s_callbacks.document_active ? s_callbacks.document_active() : 0) + 1);
    return 1;
}
static int checked_document_index(lua_State *L, int argument) {
    int count = s_callbacks.document_count ? s_callbacks.document_count() : 1;
    lua_Integer index = luaL_checkinteger(L, argument);
    luaL_argcheck(L, index >= 1 && index <= count, argument, "document index out of range");
    return (int)index - 1;
}
/* The document at the optional index argument, or the active one. */
static int optional_document_index(lua_State *L, int argument) {
    if (!lua_isnoneornil(L, argument)) return checked_document_index(L, argument);
    return s_callbacks.document_active ? s_callbacks.document_active() : 0;
}
static int documents_activate(lua_State *L) {
    int index = checked_document_index(L, 1);
    if (!s_callbacks.document_activate)
        return luaL_error(L, "documents.activate is not available here");
    lua_pushboolean(L, s_callbacks.document_activate(index));
    return 1;
}
static int documents_new(lua_State *L) {
    if (!s_callbacks.document_new) return luaL_error(L, "documents.new is not available here");
    lua_pushboolean(L, s_callbacks.document_new());
    return 1;
}
static int documents_open(lua_State *L) {
    const char *path = checked_cstring(L, 1, "file path");
    if (!s_callbacks.document_open) return luaL_error(L, "documents.open is not available here");
    lua_pushboolean(L, s_callbacks.document_open(path));
    return 1;
}
static int documents_save(lua_State *L) {
    if (!s_callbacks.document_save) return luaL_error(L, "documents.save is not available here");
    lua_pushboolean(L, s_callbacks.document_save(lua_toboolean(L, 1)));
    return 1;
}
static int documents_close(lua_State *L) {
    int index = optional_document_index(L, 1);
    if (!s_callbacks.document_close) return luaL_error(L, "documents.close is not available here");
    lua_pushboolean(L, s_callbacks.document_close(index));
    return 1;
}
static int documents_path(lua_State *L) {
    int index = optional_document_index(L, 1);
    char *path = s_callbacks.document_path ? s_callbacks.document_path(index) : NULL;
    if (path) lua_pushstring(L, path); else lua_pushnil(L);
    free(path);
    return 1;
}
static int documents_modified(lua_State *L) {
    int index = optional_document_index(L, 1);
    lua_pushboolean(L, s_callbacks.document_modified && s_callbacks.document_modified(index));
    return 1;
}
/* documents.auto_reload([index] [, on]): whether the document reloads
   changes on disk without asking, set when `on` is given. */
static int documents_auto_reload(lua_State *L) {
    int valueArgument = lua_type(L, 1) == LUA_TBOOLEAN ? 1 : 2;
    int index = valueArgument == 2 ? optional_document_index(L, 1)
              : s_callbacks.document_active ? s_callbacks.document_active() : 0;
    int value = lua_isnoneornil(L, valueArgument) ? -1 : lua_toboolean(L, valueArgument);
    if (value >= 0) luaL_checktype(L, valueArgument, LUA_TBOOLEAN);
    if (!s_callbacks.document_auto_reload)
        return luaL_error(L, "documents.auto_reload is not available here");
    lua_pushboolean(L, s_callbacks.document_auto_reload(index, value));
    return 1;
}
static int documents_encoding(lua_State *L) {
    if (!lua_isnoneornil(L, 1)) {
        lua_editor_api_require_editor(L);
        const char *encoding = checked_cstring(L, 1, "encoding");
        int valid = strcmp(encoding, "utf-8") == 0 || strcmp(encoding, "utf-8-bom") == 0 ||
                    strcmp(encoding, "utf-16le") == 0 || strcmp(encoding, "utf-16be") == 0 ||
                    strcmp(encoding, "ansi") == 0;
        luaL_argcheck(L, valid, 1,
                      "expected utf-8, utf-8-bom, utf-16le, utf-16be or ansi");
        if (!s_callbacks.document_set_encoding)
            return luaL_error(L, "documents.encoding cannot be changed here");
        lua_pushboolean(L, s_callbacks.document_set_encoding(encoding));
        return 1;
    }
    const char *encoding = s_callbacks.document_encoding ? s_callbacks.document_encoding() : "utf-8";
    lua_pushstring(L, encoding ? encoding : "utf-8");
    return 1;
}

static int spell_language(lua_State *L) {
    char *language = s_callbacks.spell_language ? s_callbacks.spell_language() : NULL;
    if (language) lua_pushstring(L, language); else lua_pushnil(L);
    free(language);
    return 1;
}

/* spell.check(word) returns a boolean; spell.check(words) returns a table
   with one boolean per word. */
static int spell_check(lua_State *L) {
    int single = lua_type(L, 1) == LUA_TSTRING;
    if (!single) luaL_checktype(L, 1, LUA_TTABLE);
    lua_Integer count = single ? 1 : (lua_Integer)lua_rawlen(L, 1);
    if (count > PLUGIN_MAX_SPELL_WORDS)
        return luaL_error(L, "spell.check takes at most %d words", PLUGIN_MAX_SPELL_WORDS);
    if (!s_callbacks.spell_check) return luaL_error(L, "spell.check is not available here");
    /* Userdata, so the arrays are freed even if a bad word raises an error.
       The pointers stay valid because the table keeps the strings alive, which
       is why the words are read raw: an __index result could be collected. */
    const char **words = (const char **)lua_newuserdatauv(
        L, (size_t)count * (sizeof(*words) + 1), 0);
    unsigned char *misspelled = (unsigned char *)(words + count);
    if (single) words[0] = checked_cstring(L, 1, "word");
    for (lua_Integer i = 0; !single && i < count; i++) {
        if (lua_rawgeti(L, 1, i + 1) != LUA_TSTRING)
            return luaL_error(L, "spell.check word %d is not a string", (int)(i + 1));
        words[i] = checked_cstring(L, -1, "spell.check word");
        lua_pop(L, 1);
    }
    if (count && !s_callbacks.spell_check(words, (int)count, misspelled))
        return luaL_error(L, "Windows spell checking is unavailable");
    if (single) {
        lua_pushboolean(L, !misspelled[0]);
        return 1;
    }
    lua_createtable(L, (int)count, 0);
    for (lua_Integer i = 0; i < count; i++) {
        lua_pushboolean(L, !misspelled[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static const luaL_Reg ui_functions[] = {
    {"message", ui_message},
    {"prompt", ui_prompt},
    {"choose", ui_choose},
    {"save_binary", ui_save_binary},
    {"confirm", ui_confirm}, {"status", ui_status}, {"log", ui_log},
    {"pick_file", ui_pick_file}, {"pick_folder", ui_pick_folder},
    {NULL, NULL}
};
static const luaL_Reg menu_functions[] = {
    {"add", menu_add}, {"checkbox", menu_checkbox}, {"separator", menu_separator}, {NULL, NULL}
};
static const luaL_Reg contextmenu_functions[] = {
    {"add", contextmenu_add}, {"checkbox", contextmenu_checkbox},
    {"remove", contextmenu_remove}, {NULL, NULL}
};
static const luaL_Reg events_functions[] = {
    {"on", events_on}, {"off", events_off}, {NULL, NULL}
};
static const luaL_Reg commands_functions[] = {
    {"register", commands_register}, {"execute", commands_execute}, {NULL, NULL}
};
static const luaL_Reg config_functions[] = { {"get", config_get}, {"set", config_set}, {NULL, NULL} };
static const luaL_Reg app_functions[] = {
    {"date", app_date}, {"time", app_time}, {"date_time", app_date_time},
    {"version", app_version}, {"api_version", app_api_version},
    {"platform", app_platform}, {"memory_used", app_memory_used},
    {"workspace_path", app_workspace_path}, {"traceback", app_traceback},
    {"capabilities", app_capabilities},
    {"defer", app_defer}, {"cancel", app_cancel},
    {NULL, NULL}
};
static const luaL_Reg documents_functions[] = {
    {"count", documents_count}, {"active", documents_active},
    {"activate", documents_activate}, {"new", documents_new}, {"open", documents_open},
    {"save", documents_save}, {"close", documents_close}, {"path", documents_path},
    {"modified", documents_modified}, {"encoding", documents_encoding},
    {"auto_reload", documents_auto_reload}, {NULL, NULL}
};
static int output_write(lua_State *L) {
    int count = lua_gettop(L);
    if (!s_callbacks.output_write) return luaL_error(L, "output.write is not available here");
    for (int i = 1; i <= count; i++) s_callbacks.output_write(checked_cstring(L, i, "output text"));
    return 0;
}

static int output_clear(lua_State *L) {
    if (!s_callbacks.output_clear) return luaL_error(L, "output.clear is not available here");
    s_callbacks.output_clear();
    return 0;
}

static int output_show(lua_State *L) {
    if (!s_callbacks.output_show) return luaL_error(L, "output.show is not available here");
    s_callbacks.output_show();
    return 0;
}

/* output.report(title, text): text under a heading naming the report and
   the document, followed by a blank line, with the panel opened on it. */
static int output_report(lua_State *L) {
    const char *title = checked_cstring(L, 1, "report title");
    const char *text = checked_cstring(L, 2, "report text");
    if (!s_callbacks.output_write || !s_callbacks.output_show)
        return luaL_error(L, "output.report is not available here");
    char *path = s_callbacks.file_path ? s_callbacks.file_path() : NULL;
    const char *name = path ? path : "Untitled";
    for (const char *c = name; *c; c++)
        if (*c == '\\' || *c == '/') name = c + 1;
    size_t length = strlen(text);
    luaL_Buffer report;
    luaL_buffinit(L, &report);
    luaL_addstring(&report, "\x1b[1;36m");
    luaL_addstring(&report, title);
    luaL_addstring(&report, " - ");
    luaL_addstring(&report, name);
    luaL_addstring(&report, "\x1b[0m\n");
    luaL_addstring(&report, text);
    luaL_addstring(&report, length && text[length - 1] == '\n' ? "\n" : "\n\n");
    free(path);
    luaL_pushresult(&report);
    s_callbacks.output_write(lua_tostring(L, -1));
    s_callbacks.output_show();
    return 0;
}

static int output_hide(lua_State *L) {
    if (!s_callbacks.output_hide) return luaL_error(L, "output.hide is not available here");
    s_callbacks.output_hide();
    return 0;
}

static int output_visible(lua_State *L) {
    lua_pushboolean(L, s_callbacks.output_visible && s_callbacks.output_visible());
    return 1;
}

/* output.table(columns, rows): a sortable grid on the Table tab. columns is
   an array of names; rows is an array of tables keyed by those names - the
   same shape db:query() already returns, so its two results can be passed
   straight through. */
static int output_table(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Integer columnCount = (lua_Integer)lua_rawlen(L, 1);
    if (columnCount < 1 || columnCount > PLUGIN_MAX_OUTPUT_TABLE_COLUMNS)
        return luaL_error(L, "output.table needs between 1 and %d columns",
                          PLUGIN_MAX_OUTPUT_TABLE_COLUMNS);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_Integer rowCount = (lua_Integer)lua_rawlen(L, 2);
    if (rowCount > PLUGIN_MAX_OUTPUT_TABLE_ROWS)
        return luaL_error(L, "output.table takes at most %d rows", PLUGIN_MAX_OUTPUT_TABLE_ROWS);
    if (!s_callbacks.output_table) return luaL_error(L, "output.table is not available here");

    /* Userdata, so the arrays are freed even if a bad row raises an error.
       Column-name and string-cell pointers stay valid because their tables
       keep the strings alive (as in ui_choose above); a number cell's text
       exists only on the stack once converted, so keepAlive anchors it. */
    const char **columns = (const char **)lua_newuserdatauv(L, (size_t)columnCount * sizeof(*columns), 0);
    for (lua_Integer i = 0; i < columnCount; i++) {
        if (lua_rawgeti(L, 1, i + 1) != LUA_TSTRING)
            return luaL_error(L, "output.table column %d is not a string", (int)(i + 1));
        columns[i] = checked_cstring(L, -1, "output.table column name");
        lua_pop(L, 1);
    }
    PluginTableCell *cells = (PluginTableCell *)lua_newuserdatauv(
        L, (size_t)rowCount * (size_t)columnCount * sizeof(*cells), 0);
    lua_newtable(L);
    int keepAlive = lua_gettop(L);
    lua_Integer kept = 0;
    for (lua_Integer row = 0; row < rowCount; row++) {
        if (lua_rawgeti(L, 2, row + 1) != LUA_TTABLE)
            return luaL_error(L, "output.table row %d is not a table", (int)(row + 1));
        int rowIndex = lua_gettop(L);
        for (lua_Integer column = 0; column < columnCount; column++) {
            PluginTableCell *cell = &cells[row * columnCount + column];
            int type = lua_getfield(L, rowIndex, columns[column]);
            if (type == LUA_TNUMBER) {
                cell->number = lua_tonumber(L, -1);
                cell->isNumber = 1;
                cell->text = lua_tolstring(L, -1, NULL);
                lua_seti(L, keepAlive, ++kept); /* anchors the converted string; pops it */
                continue;
            }
            if (type == LUA_TNIL) { cell->text = ""; cell->isNumber = 0; }
            else if (type == LUA_TBOOLEAN) { cell->text = lua_toboolean(L, -1) ? "true" : "false"; cell->isNumber = 0; }
            else if (type == LUA_TSTRING) { cell->text = checked_cstring(L, -1, "output.table cell"); cell->isNumber = 0; }
            else return luaL_error(L, "output.table cell values must be nil, a boolean, a number, or a string");
            lua_pop(L, 1);
        }
        lua_pop(L, 1); /* the row table */
    }
    s_callbacks.output_table(columns, (int)columnCount, cells, (int)rowCount);
    return 0;
}

static const luaL_Reg output_functions[] = {
    {"write", output_write}, {"clear", output_clear}, {"show", output_show},
    {"hide", output_hide}, {"visible", output_visible}, {"report", output_report},
    {"table", output_table}, {NULL, NULL}
};

static int terminal_begin(lua_State *L) {
    if (!s_callbacks.terminal_begin || !s_callbacks.terminal_begin())
        return luaL_error(L, "terminal.begin is not available here");
    return 0;
}

static int terminal_write(lua_State *L) {
    int count = lua_gettop(L);
    if (!s_callbacks.terminal_write)
        return luaL_error(L, "terminal.write is not available here");
    for (int i = 1; i <= count; i++)
        s_callbacks.terminal_write(checked_cstring(L, i, "terminal text"));
    return 0;
}

static int terminal_read(lua_State *L) {
    const char *prompt = optional_cstring(L, 1, NULL, "terminal prompt");
    if (!s_callbacks.terminal_read)
        return luaL_error(L, "terminal.read is not available here");
    int interrupted = 0;
    char *answer = s_callbacks.terminal_read(prompt, &interrupted);
    restart_time_limit();
    if (interrupted) {
        free(answer);
        plugin_lua_mark_stopped(L);
        return luaL_error(L, "interrupted");
    }
    if (answer) lua_pushstring(L, answer); else lua_pushnil(L);
    free(answer);
    return 1;
}

static int terminal_end(lua_State *L) {
    (void)L;
    if (!s_callbacks.terminal_end)
        return luaL_error(L, "terminal.finish is not available here");
    s_callbacks.terminal_end();
    return 0;
}

static int terminal_clear(lua_State *L) {
    (void)L;
    if (!s_callbacks.terminal_clear)
        return luaL_error(L, "terminal.clear is not available here");
    s_callbacks.terminal_clear();
    return 0;
}

static const luaL_Reg terminal_functions[] = {
    {"begin", terminal_begin}, {"write", terminal_write},
    {"read", terminal_read}, {"clear", terminal_clear},
    {"finish", terminal_end}, {NULL, NULL}
};
static int problems_report(lua_State *L) {
    const char *source = checked_cstring(L, 1, "problem source");
    const char *text = checked_cstring(L, 2, "problem report");
    if (!s_callbacks.problems_report)
        return luaL_error(L, "problems.report is not available here");
    s_callbacks.problems_report(source, text);
    return 0;
}

static int problems_clear(lua_State *L) {
    const char *source = checked_cstring(L, 1, "problem source");
    if (!s_callbacks.problems_report)
        return luaL_error(L, "problems.clear is not available here");
    s_callbacks.problems_report(source, NULL);
    return 0;
}

static int problems_clear_all(lua_State *L) {
    (void)L;
    if (!s_callbacks.problems_clear_all)
        return luaL_error(L, "problems.clear_all is not available here");
    s_callbacks.problems_clear_all();
    return 0;
}

static const luaL_Reg problems_functions[] = {
    {"report", problems_report}, {"clear", problems_clear},
    {"clear_all", problems_clear_all}, {NULL, NULL}
};
static const luaL_Reg spell_functions[] = {
    {"language", spell_language}, {"check", spell_check}, {NULL, NULL}
};

void plugin_registry_open(lua_State *L) {
    luaL_newlib(L, ui_functions);
    lua_setglobal(L, "ui");
    luaL_newlib(L, menu_functions);
    lua_setglobal(L, "menu");
    luaL_newlib(L, contextmenu_functions);
    lua_setglobal(L, "contextmenu");
    luaL_newlib(L, events_functions);
    lua_setglobal(L, "events");
    luaL_newlib(L, commands_functions);
    lua_setglobal(L, "commands");
    luaL_newlib(L, config_functions);
    lua_setglobal(L, "config");
    luaL_newlib(L, app_functions);
    lua_setglobal(L, "app");
    luaL_newlib(L, documents_functions);
    lua_setglobal(L, "documents");
    luaL_newlib(L, spell_functions);
    lua_setglobal(L, "spell");
    luaL_newlib(L, output_functions);
    lua_setglobal(L, "output");
    luaL_newlib(L, terminal_functions);
    lua_setglobal(L, "terminal");
    luaL_newlib(L, problems_functions);
    lua_setglobal(L, "problems");
}

int plugin_host_command_count(void) {
    return s_commandCount;
}

const char *plugin_host_command_label(int index) {
    return index >= 0 && index < s_commandCount ? s_commands[index].label : NULL;
}

const char *plugin_host_command_shortcut(int index) {
    return index >= 0 && index < s_commandCount ? s_commands[index].shortcut : NULL;
}

int plugin_host_command_accelerator(int index, BYTE *flags, WORD *key) {
    if (index < 0 || index >= s_commandCount || !s_commands[index].shortcut) return 0;
    if (flags) *flags = s_commands[index].shortcutFlags;
    if (key) *key = s_commands[index].shortcutKey;
    return 1;
}

int plugin_host_command_is_placed(int index) {
    return index >= 0 && index < s_commandCount && s_commands[index].placed;
}

int plugin_host_command_is_separator(int index) {
    return index >= 0 && index < s_commandCount && s_commands[index].separator;
}

int plugin_host_command_is_checkable(int index) {
    return index >= 0 && index < s_commandCount && s_commands[index].configKey != NULL;
}

static int stored_checkbox_state(const char *owner, const char *key, int defaultState) {
    char *value = s_callbacks.config_get
        ? s_callbacks.config_get(owner, key) : NULL;
    int checked = value ? config_value_is_true(value) : defaultState;
    free(value);
    return checked;
}

int plugin_host_command_is_checked(int index) {
    if (!plugin_host_command_is_checkable(index)) return 0;
    const PluginCommand *command = &s_commands[index];
    return stored_checkbox_state(command->owner, command->configKey, command->checkDefault);
}

const char *plugin_host_command_checkbox_owner(int index) {
    return plugin_host_command_is_checkable(index) ? s_commands[index].owner : NULL;
}

const char *plugin_host_command_checkbox_key(int index) {
    return plugin_host_command_is_checkable(index) ? s_commands[index].configKey : NULL;
}

int plugin_host_find_checkbox(const char *owner, const char *key) {
    if (!owner || !key) return -1;
    for (int i = 0; i < s_commandCount; i++)
        if (s_commands[i].configKey && strcmp(s_commands[i].owner, owner) == 0 &&
            strcmp(s_commands[i].configKey, key) == 0) return i;
    return -1;
}

const char *plugin_host_command_overrides(int index) {
    return index >= 0 && index < s_commandCount ? s_commands[index].overrides : NULL;
}

int plugin_host_context_menu_item_count(void) {
    return s_contextMenuItemCount;
}

const char *plugin_host_context_menu_item_label(int index) {
    return index >= 0 && index < s_contextMenuItemCount
        ? s_contextMenuItems[index].label : NULL;
}

int plugin_host_context_menu_item_id(int index) {
    return index >= 0 && index < s_contextMenuItemCount
        ? s_contextMenuItems[index].id : 0;
}

int plugin_host_context_menu_item_is_checkable(int index) {
    return index >= 0 && index < s_contextMenuItemCount &&
        s_contextMenuItems[index].configKey != NULL;
}

int plugin_host_context_menu_item_is_checked(int index) {
    if (!plugin_host_context_menu_item_is_checkable(index)) return 0;
    const PluginContextMenuItem *item = &s_contextMenuItems[index];
    return stored_checkbox_state(item->owner, item->configKey, item->checkDefault);
}

int plugin_host_command_has_enabled(int index) {
    return index >= 0 && index < s_commandCount &&
        s_commands[index].enabledRef != LUA_NOREF;
}

/* Calls a context-menu enabled predicate and writes its truth value through
   the light userdata argument. Keeping the result outside Lua lets the
   existing protected-call path restore the stack and enforce time limits. */
static int call_enabled_predicate(lua_State *L) {
    int *enabled = (int *)lua_touserdata(L, 2);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    lua_call(L, 0, 1);
    *enabled = lua_toboolean(L, -1);
    return 0;
}

char *plugin_host_context_menu_item_enabled(int index, HWND hEdit, int *enabled) {
    if (enabled) *enabled = 0;
    if (!enabled || s_busy || !s_lua || index < 0 || index >= s_contextMenuItemCount)
        return NULL;
    PluginContextMenuItem *item = &s_contextMenuItems[index];
    if (item->enabledRef == LUA_NOREF) {
        *enabled = 1;
        return NULL;
    }

    char *stableLabel = dup_string(item->label);
    char *stableOwner = dup_string(item->owner);
    if (!stableLabel || !stableOwner) {
        free(stableLabel); free(stableOwner);
        return dup_string("out of memory evaluating context menu state");
    }
    s_busy = 1;
    lua_editor_api_set_editor(hEdit);
    s_runningLabel = stableLabel;
    s_runningOwner = stableOwner;
    s_timeLimited = 1;
    lua_pushcfunction(s_lua, call_enabled_predicate);
    lua_rawgeti(s_lua, LUA_REGISTRYINDEX, item->enabledRef);
    lua_pushlightuserdata(s_lua, enabled);
    char *error = protected_call(2);
    lua_editor_api_set_editor(NULL);
    s_runningLabel = NULL;
    s_runningOwner = NULL;
    s_busy = 0;
    free(stableLabel);
    free(stableOwner);
    return error;
}

char *plugin_host_command_enabled(int index, HWND hEdit, int *enabled) {
    if (enabled) *enabled = 0;
    if (!enabled || s_busy || !s_lua || index < 0 || index >= s_commandCount)
        return NULL;
    PluginCommand *command = &s_commands[index];
    if (command->enabledRef == LUA_NOREF) {
        *enabled = 1;
        return NULL;
    }

    char *stableLabel = dup_string(command->label);
    char *stableOwner = dup_string(command->owner);
    if (!stableLabel || !stableOwner) {
        free(stableLabel); free(stableOwner);
        return dup_string("out of memory evaluating menu command state");
    }
    s_busy = 1;
    lua_editor_api_set_editor(hEdit);
    s_runningLabel = stableLabel;
    s_runningOwner = stableOwner;
    s_timeLimited = 1;
    lua_pushcfunction(s_lua, call_enabled_predicate);
    lua_rawgeti(s_lua, LUA_REGISTRYINDEX, command->enabledRef);
    lua_pushlightuserdata(s_lua, enabled);
    char *error = protected_call(2);
    lua_editor_api_set_editor(NULL);
    s_runningLabel = NULL;
    s_runningOwner = NULL;
    s_busy = 0;
    free(stableLabel);
    free(stableOwner);
    return error;
}

int plugin_host_find_override(const char *name) {
    for (int i = s_commandCount - 1; i >= 0; i--)
        if (s_commands[i].overrides && strcmp(s_commands[i].overrides, name) == 0) return i;
    return -1;
}

/* Calls the function with its one argument, which a string argument arrives
   as a light userdata for: copying it into Lua is done here, under protection,
   because running out of memory outside protected mode aborts the process. */
static int call_with_argument(lua_State *L) {
    if (lua_islightuserdata(L, 2)) {
        const char *argument = (const char *)lua_touserdata(L, 2);
        lua_pop(L, 1);
        lua_pushstring(L, argument);
    }
    lua_call(L, 1, 0);
    return 0;
}

#define PLUGIN_PERFORMANCE_MAX 512
typedef struct PluginPerformance {
    char *owner, *label;
    unsigned calls, failures;
    ULONGLONG cpuTotal, cpuMax;
    ptrdiff_t retainedMax;
} PluginPerformance;
static PluginPerformance s_performance[PLUGIN_PERFORMANCE_MAX];
static int s_performanceCount;

static ULONGLONG thread_cpu_time(void) {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER k = { .LowPart = kernel.dwLowDateTime, .HighPart = kernel.dwHighDateTime };
    ULARGE_INTEGER u = { .LowPart = user.dwLowDateTime, .HighPart = user.dwHighDateTime };
    return k.QuadPart + u.QuadPart;
}

static void record_performance(const char *owner, const char *label, ULONGLONG elapsed,
                               ptrdiff_t retained, int failed) {
    int index = -1;
    for (int i = 0; i < s_performanceCount; i++)
        if (strcmp(s_performance[i].owner, owner) == 0 &&
            strcmp(s_performance[i].label, label) == 0) { index = i; break; }
    if (index < 0) {
        if (s_performanceCount >= PLUGIN_PERFORMANCE_MAX) return;
        index = s_performanceCount++;
        s_performance[index].owner = dup_string(owner);
        s_performance[index].label = dup_string(label);
        if (!s_performance[index].owner || !s_performance[index].label) {
            free(s_performance[index].owner); free(s_performance[index].label);
            memset(&s_performance[index], 0, sizeof(s_performance[index]));
            s_performanceCount--;
            return;
        }
    }
    PluginPerformance *entry = &s_performance[index];
    entry->calls++;
    if (failed) entry->failures++;
    entry->cpuTotal += elapsed;
    if (elapsed > entry->cpuMax) entry->cpuMax = elapsed;
    if (retained > entry->retainedMax) entry->retainedMax = retained;
}

void plugin_host_clear_performance(void) {
    for (int i = 0; i < s_performanceCount; i++) {
        free(s_performance[i].owner);
        free(s_performance[i].label);
    }
    memset(s_performance, 0, sizeof(s_performance));
    s_performanceCount = 0;
}

char *plugin_host_performance_report(int warningsOnly) {
    char *report = NULL;
    size_t used = 0;
    for (int i = 0; i < s_performanceCount; i++) {
        const PluginPerformance *entry = &s_performance[i];
        if (warningsOnly && !entry->failures && entry->cpuMax < 500000 &&
            entry->retainedMax < 1024 * 1024) continue;
        double totalMs = (double)entry->cpuTotal / 10000.0;
        double maxMs = (double)entry->cpuMax / 10000.0;
        double memoryKiB = (double)entry->retainedMax / 1024.0;
        char failures[48] = "";
        if (entry->failures)
            snprintf(failures, sizeof(failures), ", %u failure%s", entry->failures,
                     entry->failures == 1 ? "" : "s");
        size_t needed = used + strlen(entry->owner) + strlen(entry->label) + 160;
        char *grown = (char *)realloc(report, needed);
        if (!grown) break;
        report = grown;
        int written = snprintf(report + used, needed - used,
            "%s — %s: %u call%s, %.2f ms total, %.2f ms max, +%.1f KiB retained%s\n",
            entry->owner, entry->label, entry->calls, entry->calls == 1 ? "" : "s",
            totalMs, maxMs, memoryKiB, failures);
        if (written < 0) break;
        used += (size_t)written;
    }
    return report;
}

/* Calls a registered function against hEdit as a single undo step, undoing
   its edits if it fails. argument is its one parameter (nil when NULL). */
static char *call_registered(int ref, const char *label, const char *owner,
                             const char *argument, int booleanArgument,
                             HWND hEdit, int timeLimited) {
    char *stableLabel = dup_string(label);
    char *stableOwner = dup_string(owner);
    if (!stableLabel || !stableOwner) {
        free(stableLabel); free(stableOwner);
        return dup_string("out of memory starting plugin callback");
    }
    ULONGLONG cpuBefore = thread_cpu_time();
    size_t memoryBefore = s_memory.used;
    s_busy = 1;
    lua_editor_api_set_editor(hEdit);
    s_runningLabel = stableLabel;
    s_runningOwner = stableOwner;
    s_timeLimited = timeLimited;
    /* An empty undo group isn't recorded, so the undo position only moves
       if the function really changed the document. */
    intptr_t undoBefore = (intptr_t)SendMessageA(hEdit, SCI_GETUNDOCURRENT, 0, 0);
    SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
    lua_pushcfunction(s_lua, call_with_argument);
    lua_rawgeti(s_lua, LUA_REGISTRYINDEX, ref);
    if (booleanArgument >= 0) lua_pushboolean(s_lua, booleanArgument);
    else if (argument) lua_pushlightuserdata(s_lua, (void *)argument);
    else lua_pushnil(s_lua);
    char *error = protected_call(2);
    lua_editor_api_finish_undo();
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
    if (error && (intptr_t)SendMessageA(hEdit, SCI_GETUNDOCURRENT, 0, 0) != undoBefore)
        SendMessageA(hEdit, SCI_UNDO, 0, 0);
    ULONGLONG cpuAfter = thread_cpu_time();
    ptrdiff_t retained = (ptrdiff_t)s_memory.used - (ptrdiff_t)memoryBefore;
    record_performance(stableOwner, stableLabel,
                       cpuAfter >= cpuBefore ? cpuAfter - cpuBefore : 0,
                       retained, error != NULL);
    /* A command on a large document can build temporaries that use a big
       share of the shared plugin heap; left to Lua's own incremental pacing,
       that garbage can sit uncollected for many further commands (event
       handlers included), starving every other plugin with spurious "not
       enough memory" failures until something eventually forces a full
       collection. Reclaiming it here, right as the heavy command that made
       it finishes, keeps that starvation to a single command instead of an
       unpredictable run of them. Ordinary commands stay far below this
       threshold, so they pay nothing extra. */
    if (s_memory.used > s_memory.limit / 2) lua_gc(s_lua, LUA_GCCOLLECT, 0);
    lua_editor_api_set_editor(NULL);
    s_runningLabel = NULL;
    s_runningOwner = NULL;
    s_timeLimited = 1;
    s_busy = 0;
    free(stableLabel);
    free(stableOwner);
    return error;
}

/* Toggles one persisted checkbox and calls its function with the new state.
   owner/key are copied because a context-menu callback may remove itself. */
static char *run_checkbox(int ref, const char *label, const char *owner,
                          const char *key, int defaultState, HWND hEdit,
                          int timeLimited) {
    int oldState = stored_checkbox_state(owner, key, defaultState);
    int newState = !oldState;
    char *stableOwner = dup_string(owner);
    char *stableKey = dup_string(key);
    if (!stableOwner || !stableKey) {
        free(stableOwner); free(stableKey);
        return dup_string("out of memory starting plugin callback");
    }
    if (!s_callbacks.config_set ||
        !s_callbacks.config_set(stableOwner, stableKey, newState ? "1" : "0")) {
        free(stableOwner); free(stableKey);
        return dup_string("Could not save the plugin checkbox state.");
    }
    char *error = call_registered(ref, label, owner, NULL, newState, hEdit, timeLimited);
    if (error) s_callbacks.config_set(stableOwner, stableKey, oldState ? "1" : "0");
    free(stableOwner); free(stableKey);
    return error;
}

char *plugin_host_run(int index, HWND hEdit) {
    if (s_busy) return dup_string("Another plugin is still running.");
    if (!s_lua || index < 0 || index >= s_commandCount)
        return dup_string("That plugin command is no longer loaded.");
    const PluginCommand *command = &s_commands[index];
    if (command->separator) return NULL; /* nothing to run */
    if (command->configKey)
        return run_checkbox(command->ref, command->label, command->owner,
                            command->configKey, command->checkDefault, hEdit,
                            !command->builtIn);
    return call_registered(command->ref, command->label, command->owner,
                           NULL, -1, hEdit, !command->builtIn);
}

char *plugin_host_run_context_menu_item(int id, HWND hEdit) {
    if (s_busy) return dup_string("Another plugin is still running.");
    if (!s_lua) return NULL;
    for (int i = 0; i < s_contextMenuItemCount; i++) {
        PluginContextMenuItem *item = &s_contextMenuItems[i];
        if (item->id != id) continue;
        if (item->configKey)
            return run_checkbox(item->ref, item->label, item->owner,
                                item->configKey, item->checkDefault, hEdit, 1);
        return call_registered(item->ref, item->label, item->owner,
                               NULL, -1, hEdit, 1);
    }
    return NULL;
}

char *plugin_host_fire(const char *event, const char *argument, HWND hEdit) {
    if (s_busy || !s_lua) return NULL;
    const char *registeredEvent = known_event(event);
    if (!registeredEvent) return NULL;
    char *report = NULL;
    /* Snapshot stable IDs. Handlers may remove themselves or each other,
       which compacts s_handlers; newly registered handlers wait until the
       next event. */
    int ids[PLUGIN_MAX_HANDLERS], count = 0;
    for (int i = 0; i < s_handlerCount; i++)
        if (s_handlers[i].event == registeredEvent) ids[count++] = s_handlers[i].id;
    for (int n = 0; n < count; n++) {
        int index = -1;
        for (int i = 0; i < s_handlerCount; i++)
            if (s_handlers[i].id == ids[n]) { index = i; break; }
        if (index < 0) continue;
        report = append_line(report, call_registered(s_handlers[index].ref,
            s_handlers[index].label, s_handlers[index].owner, argument, -1, hEdit, 1));
    }
    return report;
}

char *plugin_host_run_deferred(int token, HWND hEdit) {
    if (s_busy || !s_lua) return NULL;
    for (int i = 0; i < PLUGIN_MAX_DEFERRED; i++) {
        PluginDeferred *deferred = &s_deferred[i];
        if (deferred->token != token) continue;
        int ref = deferred->ref;
        char *owner = deferred->owner;
        char *label = deferred->label;
        memset(deferred, 0, sizeof(*deferred));
        char *error = call_registered(ref, label, owner, NULL, -1, hEdit, 1);
        luaL_unref(s_lua, LUA_REGISTRYINDEX, ref);
        free(owner); free(label);
        return error;
    }
    return NULL;
}
