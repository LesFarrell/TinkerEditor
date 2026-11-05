/* Exposes `sqlite.open`/db:exec/db:query/db:close to plugins, backed by
   winsqlite3.dll (System32\winsqlite3.dll on Windows 10/11, the SQLite build
   Windows itself uses for UWP apps). The functions needed are looked up by
   name with GetProcAddress, so this needs no sqlite3.h, no import library,
   and no vendored/third-party source - just declarations matching the
   public C API's stable ABI.

   sqlite.open(path) is a deliberate, explicit exception to "plugins can't
   touch arbitrary files" (plugins/README.md): it opens or creates a real
   file at `path`, unlike every other plugin-facing API, including `zip`
   (see plugin_zip.c), which only ever works on in-memory bytes. Keep it the
   only such exception; this file's loader pattern should not be reused to
   add more file access elsewhere.

   A query's row-stepping loop runs entirely inside one C call, so the
   plugin time-limit hook (plugin_lua_runtime.c), which fires between Lua
   instructions, never sees it; db_run() below checks GetTickCount64() itself
   so a runaway query still stops instead of hanging the editor. */
#include "plugin_sqlite.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "lauxlib.h"
#include "plugin_host.h"
#include "text_codec.h"

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef long long sqlite3_int64;

enum {
    SQLITE_OK = 0,
    SQLITE_ROW = 100,
    SQLITE_DONE = 101,
    SQLITE_INTEGER = 1,
    SQLITE_FLOAT = 2,
    SQLITE_TEXT = 3,
    SQLITE_BLOB = 4,
    SQLITE_NULL = 5
};
#define SQLITE_TRANSIENT ((void (*)(void *))(INT_PTR)-1)

typedef int (*Fn_open16)(const void *filename, sqlite3 **db);
typedef int (*Fn_close_v2)(sqlite3 *);
typedef int (*Fn_prepare16_v2)(sqlite3 *, const void *sql, int nByte, sqlite3_stmt **stmt, const void **tail);
typedef int (*Fn_step)(sqlite3_stmt *);
typedef int (*Fn_finalize)(sqlite3_stmt *);
typedef int (*Fn_column_count)(sqlite3_stmt *);
typedef const void *(*Fn_column_name16)(sqlite3_stmt *, int);
typedef int (*Fn_column_type)(sqlite3_stmt *, int);
typedef sqlite3_int64 (*Fn_column_int64)(sqlite3_stmt *, int);
typedef double (*Fn_column_double)(sqlite3_stmt *, int);
typedef const void *(*Fn_column_text16)(sqlite3_stmt *, int);
typedef const void *(*Fn_column_blob)(sqlite3_stmt *, int);
typedef int (*Fn_column_bytes)(sqlite3_stmt *, int);
typedef int (*Fn_bind_int64)(sqlite3_stmt *, int, sqlite3_int64);
typedef int (*Fn_bind_double)(sqlite3_stmt *, int, double);
typedef int (*Fn_bind_null)(sqlite3_stmt *, int);
typedef int (*Fn_bind_text16)(sqlite3_stmt *, int, const void *, int, void (*)(void *));
typedef int (*Fn_changes)(sqlite3 *);
typedef const void *(*Fn_errmsg16)(sqlite3 *);

static Fn_open16 s_open16;
static Fn_close_v2 s_close_v2;
static Fn_prepare16_v2 s_prepare16_v2;
static Fn_step s_step;
static Fn_finalize s_finalize;
static Fn_column_count s_column_count;
static Fn_column_name16 s_column_name16;
static Fn_column_type s_column_type;
static Fn_column_int64 s_column_int64;
static Fn_column_double s_column_double;
static Fn_column_text16 s_column_text16;
static Fn_column_blob s_column_blob;
static Fn_column_bytes s_column_bytes;
static Fn_bind_int64 s_bind_int64;
static Fn_bind_double s_bind_double;
static Fn_bind_null s_bind_null;
static Fn_bind_text16 s_bind_text16;
static Fn_changes s_changes;
static Fn_errmsg16 s_errmsg16;

static int s_loadAttempted, s_available;

static void load_sqlite(void) {
    if (s_loadAttempted) return;
    s_loadAttempted = 1;
    HMODULE module = LoadLibraryW(L"winsqlite3.dll");
    if (!module) return;
#define LOAD(var, type, exportName) do { \
        var = (type)(void *)GetProcAddress(module, exportName); \
        if (!var) return; \
    } while (0)
    LOAD(s_open16, Fn_open16, "sqlite3_open16");
    LOAD(s_close_v2, Fn_close_v2, "sqlite3_close_v2");
    LOAD(s_prepare16_v2, Fn_prepare16_v2, "sqlite3_prepare16_v2");
    LOAD(s_step, Fn_step, "sqlite3_step");
    LOAD(s_finalize, Fn_finalize, "sqlite3_finalize");
    LOAD(s_column_count, Fn_column_count, "sqlite3_column_count");
    LOAD(s_column_name16, Fn_column_name16, "sqlite3_column_name16");
    LOAD(s_column_type, Fn_column_type, "sqlite3_column_type");
    LOAD(s_column_int64, Fn_column_int64, "sqlite3_column_int64");
    LOAD(s_column_double, Fn_column_double, "sqlite3_column_double");
    LOAD(s_column_text16, Fn_column_text16, "sqlite3_column_text16");
    LOAD(s_column_blob, Fn_column_blob, "sqlite3_column_blob");
    LOAD(s_column_bytes, Fn_column_bytes, "sqlite3_column_bytes");
    LOAD(s_bind_int64, Fn_bind_int64, "sqlite3_bind_int64");
    LOAD(s_bind_double, Fn_bind_double, "sqlite3_bind_double");
    LOAD(s_bind_null, Fn_bind_null, "sqlite3_bind_null");
    LOAD(s_bind_text16, Fn_bind_text16, "sqlite3_bind_text16");
    LOAD(s_changes, Fn_changes, "sqlite3_changes");
    LOAD(s_errmsg16, Fn_errmsg16, "sqlite3_errmsg16");
#undef LOAD
    s_available = 1;
}

#define SQLITE_DB_METATABLE "tinkeredit.sqlite.db"

/* Pushes a UTF-16 string returned by sqlite (already NUL-terminated by
   sqlite itself, except for blobs, which never go through here) as UTF-8,
   or nil when there is none. */
static void push_wide_or_nil(lua_State *L, const void *wide) {
    char *utf8 = wide ? to_utf8((const WCHAR *)wide) : NULL;
    if (utf8) {
        lua_pushstring(L, utf8);
        free(utf8);
    } else {
        lua_pushnil(L);
    }
}

static sqlite3 **check_db(lua_State *L, int index) {
    return (sqlite3 **)luaL_checkudata(L, index, SQLITE_DB_METATABLE);
}

static int db_close(lua_State *L) {
    sqlite3 **handle = check_db(L, 1);
    if (*handle) {
        s_close_v2(*handle);
        *handle = NULL;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* Shared by __gc and __close: a plugin that never calls close() explicitly,
   or that errors first, still releases the OS file handle. */
static int db_release(lua_State *L) {
    sqlite3 **handle = (sqlite3 **)lua_touserdata(L, 1);
    if (handle && *handle) {
        s_close_v2(*handle);
        *handle = NULL;
    }
    return 0;
}

/* Binds Lua values at stack indices [first, last] as positional sqlite
   parameters (1-based, in order). Returns an sqlite result code, with
   *errMessage set on anything other than SQLITE_OK. */
static int bind_params(lua_State *L, sqlite3_stmt *stmt, int first, int last, const char **errMessage) {
    int parameterIndex = 0;
    for (int i = first; i <= last; i++) {
        parameterIndex++;
        int rc = SQLITE_OK;
        switch (lua_type(L, i)) {
            case LUA_TNIL:
                rc = s_bind_null(stmt, parameterIndex);
                break;
            case LUA_TBOOLEAN:
                rc = s_bind_int64(stmt, parameterIndex, lua_toboolean(L, i) ? 1 : 0);
                break;
            case LUA_TNUMBER:
                rc = lua_isinteger(L, i)
                    ? s_bind_int64(stmt, parameterIndex, (sqlite3_int64)lua_tointeger(L, i))
                    : s_bind_double(stmt, parameterIndex, (double)lua_tonumber(L, i));
                break;
            case LUA_TSTRING: {
                size_t length = 0;
                const char *text = lua_tolstring(L, i, &length);
                WCHAR *wide = to_wide_n(text, length, NULL);
                if (!wide) {
                    *errMessage = "parameters must be valid UTF-8 text";
                    return -1;
                }
                rc = s_bind_text16(stmt, parameterIndex, wide, -1, SQLITE_TRANSIENT);
                free(wide);
                break;
            }
            default:
                *errMessage = "parameters must be nil, a boolean, a number, or a string";
                return -1;
        }
        if (rc != SQLITE_OK) {
            *errMessage = "binding a parameter failed";
            return rc;
        }
    }
    return SQLITE_OK;
}

static void push_column_value(lua_State *L, sqlite3_stmt *stmt, int column) {
    switch (s_column_type(stmt, column)) {
        case SQLITE_INTEGER:
            lua_pushinteger(L, (lua_Integer)s_column_int64(stmt, column));
            break;
        case SQLITE_FLOAT:
            lua_pushnumber(L, s_column_double(stmt, column));
            break;
        case SQLITE_BLOB: {
            int bytes = s_column_bytes(stmt, column);
            const void *blob = s_column_blob(stmt, column);
            lua_pushlstring(L, (const char *)blob, bytes > 0 ? (size_t)bytes : 0);
            break;
        }
        case SQLITE_NULL:
            lua_pushnil(L);
            break;
        default:
            push_wide_or_nil(L, s_column_text16(stmt, column));
            break;
    }
}

/* db:exec(sql [, ...]) and db:query(sql [, ...]) share everything but what
   happens to the rows sqlite produces. db:exec("select ...") works too - it
   just throws the rows away and reports how many rows the statement
   touched instead, same as a plain insert/update/delete. */
static int db_run(lua_State *L, int collectRows) {
    sqlite3 **handle = check_db(L, 1);
    if (!*handle) return luaL_error(L, "the database is closed");
    size_t sqlLength = 0;
    const char *sql = luaL_checklstring(L, 2, &sqlLength);
    WCHAR *wideSql = to_wide_n(sql, sqlLength, NULL);
    if (!wideSql) {
        lua_pushnil(L);
        lua_pushstring(L, "SQL text must be valid UTF-8");
        return 2;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = s_prepare16_v2(*handle, wideSql, -1, &stmt, NULL);
    free(wideSql);
    if (rc != SQLITE_OK) {
        lua_pushnil(L);
        push_wide_or_nil(L, s_errmsg16(*handle));
        return 2;
    }
    const char *bindError = NULL;
    if (bind_params(L, stmt, 3, lua_gettop(L), &bindError) != SQLITE_OK) {
        s_finalize(stmt);
        lua_pushnil(L);
        lua_pushstring(L, bindError);
        return 2;
    }
    if (collectRows) lua_newtable(L);
    ULONGLONG started = GetTickCount64();
    int rowCount = 0;
    for (;;) {
        rc = s_step(stmt);
        if (rc != SQLITE_ROW) break;
        if (collectRows) {
            int columnCount = s_column_count(stmt);
            lua_createtable(L, 0, columnCount);
            for (int column = 0; column < columnCount; column++) {
                char *name = to_utf8((const WCHAR *)s_column_name16(stmt, column));
                push_column_value(L, stmt, column);
                if (name) {
                    lua_setfield(L, -2, name);
                    free(name);
                } else {
                    lua_pop(L, 1);
                }
            }
            lua_seti(L, -2, ++rowCount);
        }
        if (GetTickCount64() - started > PLUGIN_TIME_LIMIT_MS) {
            s_finalize(stmt);
            return luaL_error(L, "stopped after %d seconds without finishing", PLUGIN_TIME_LIMIT_MS / 1000);
        }
    }
    if (rc != SQLITE_DONE) {
        if (collectRows) lua_pop(L, 1);
        push_wide_or_nil(L, s_errmsg16(*handle));
        const char *message = lua_tostring(L, -1);
        lua_pop(L, 1);
        s_finalize(stmt);
        lua_pushnil(L);
        lua_pushstring(L, message ? message : "the statement failed");
        return 2;
    }
    s_finalize(stmt);
    if (collectRows) return 1;
    lua_pushboolean(L, 1);
    lua_pushinteger(L, s_changes(*handle));
    return 2;
}

static int db_exec(lua_State *L) { return db_run(L, 0); }
static int db_query(lua_State *L) { return db_run(L, 1); }

/* sqlite.open(path): opens path, creating it (and an empty database in it)
   if it doesn't exist yet - the usual sqlite3_open16 default. */
static int sqlite_open(lua_State *L) {
    if (!s_available) {
        lua_pushnil(L);
        lua_pushstring(L, "sqlite is not available on this system");
        return 2;
    }
    size_t pathLength = 0;
    const char *path = luaL_checklstring(L, 1, &pathLength);
    WCHAR *widePath = to_wide_n(path, pathLength, NULL);
    if (!widePath) {
        lua_pushnil(L);
        lua_pushstring(L, "path must be valid UTF-8");
        return 2;
    }
    sqlite3 *db = NULL;
    int rc = s_open16(widePath, &db);
    free(widePath);
    if (rc != SQLITE_OK) {
        lua_pushnil(L);
        if (db) {
            push_wide_or_nil(L, s_errmsg16(db));
            s_close_v2(db);
        } else {
            lua_pushstring(L, "could not open the database");
        }
        return 2;
    }
    sqlite3 **handle = (sqlite3 **)lua_newuserdatauv(L, sizeof(sqlite3 *), 0);
    *handle = db;
    luaL_setmetatable(L, SQLITE_DB_METATABLE);
    return 1;
}

void plugin_sqlite_install(lua_State *L) {
    load_sqlite();
    if (!s_available) return;

    if (luaL_newmetatable(L, SQLITE_DB_METATABLE)) {
        static const luaL_Reg methods[] = {
            {"exec", db_exec}, {"query", db_query}, {"close", db_close}, {NULL, NULL}
        };
        luaL_setfuncs(L, methods, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, db_release);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, db_release);
        lua_setfield(L, -2, "__close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, sqlite_open);
    lua_setfield(L, -2, "open");
    lua_setglobal(L, "sqlite");
}
