#include "docx_zip.h"

#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* liolib.c and loslib.c (Lua's io/os library *implementations*) aren't part
   of this project's build at all (see lua-sources.rsp) - plugins.md's "io,
   os... not available" is enforced at link time, not just by never calling
   luaopen_io()/luaopen_os() in the sandboxed state. That's still true here:
   this driver creates its own throwaway `io`/`os` tables with just the two
   functions ziptool.lua needs, `io.open` (the fake in-memory handle below)
   and `os.date` (ziptool's dos_datetime() already defaults every field a
   `{}` is missing, so an always-empty table is a fine stand-in for "now" -
   the DOS timestamp in a ZIP entry is cosmetic metadata, not load-bearing).
   Takes (ziptoolSource, parts) via `...`. These globals have to exist
   before ziptool.lua's top-level chunk runs, since it captures them once,
   as the upvalues `io_open`/`os_date`, at that point. */
static const char DOCX_ZIP_DRIVER[] =
    "local ziptoolSource, parts = ...\n"
    "local chunks = {}\n"
    "local fakeFile = {}\n"
    "function fakeFile:write(s) chunks[#chunks + 1] = s; return true end\n"
    "function fakeFile:close() return true end\n"
    "local SENTINEL = '\\1tinkeredit-docx-export\\1'\n"
    "io = { open = function(path) if path == SENTINEL then return fakeFile end "
    "return nil, 'file access is not available' end }\n"
    "os = { date = function() return {} end }\n"
    "local chunkFn, loadErr = load(ziptoolSource, 'ziptool.lua')\n"
    "if not chunkFn then return nil, loadErr end\n"
    "local ok, ziptoolOrErr = pcall(chunkFn)\n"
    "if not ok then return nil, tostring(ziptoolOrErr) end\n"
    "local zw, zerr = ziptoolOrErr.ZipWriter.new(SENTINEL, { compress = false })\n"
    "if not zw then return nil, zerr end\n"
    "for i = 1, #parts do\n"
    "  local addOk, addErr = zw:add_data(parts[i].name, parts[i].data)\n"
    "  if not addOk then return nil, addErr end\n"
    "end\n"
    "local closeOk, closeErr = zw:close()\n"
    "if not closeOk then return nil, closeErr end\n"
    "return table.concat(chunks)\n";

/* Only the libraries lua-sources.rsp actually compiles (see the comment on
   DOCX_ZIP_DRIVER above) - the same list plugin_host.c's open_environment()
   opens for the sandboxed plugin state, minus that function's extra step of
   nilling out dofile/loadfile, which isn't needed here: nothing in
   DOCX_ZIP_DRIVER or ziptool.lua calls them. luaL_openlibs() would instead
   try to open every standard library, including io/os/package/debug, whose
   luaopen_*() functions don't exist in this build - an unresolved external
   at link time, not a runtime failure. */
static void open_docx_zip_libraries(lua_State *L) {
    static const luaL_Reg libraries[] = {
        {LUA_GNAME, luaopen_base},
        {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    for (size_t i = 0; i < sizeof(libraries) / sizeof(libraries[0]); i++) {
        luaL_requiref(L, libraries[i].name, libraries[i].func, 1);
        lua_pop(L, 1);
    }
}

char *docx_zip_build(const char *ziptoolSource, size_t ziptoolLength,
                     const DocxZipPart *parts, size_t partCount,
                     char **outBytes, size_t *outLength) {
    *outBytes = NULL;
    *outLength = 0;
    lua_State *L = luaL_newstate();
    if (!L) return _strdup("Not enough memory to build the .docx file.");
    open_docx_zip_libraries(L);

    if (luaL_loadbuffer(L, DOCX_ZIP_DRIVER, sizeof(DOCX_ZIP_DRIVER) - 1, "docx_zip") != LUA_OK) {
        char *error = _strdup(lua_tostring(L, -1));
        lua_close(L);
        return error;
    }
    lua_pushlstring(L, ziptoolSource, ziptoolLength);
    lua_createtable(L, (int)partCount, 0);
    for (size_t i = 0; i < partCount; i++) {
        lua_createtable(L, 0, 2);
        lua_pushstring(L, parts[i].name);
        lua_setfield(L, -2, "name");
        lua_pushlstring(L, parts[i].data, parts[i].length);
        lua_setfield(L, -2, "data");
        lua_rawseti(L, -2, (int)(i + 1));
    }
    if (lua_pcall(L, 2, 2, 0) != LUA_OK) {
        char *error = _strdup(lua_tostring(L, -1));
        lua_close(L);
        return error;
    }

    char *result;
    if (lua_isstring(L, -2)) {
        size_t length = 0;
        const char *bytes = lua_tolstring(L, -2, &length);
        char *copy = (char *)malloc(length ? length : 1);
        if (copy) memcpy(copy, bytes, length);
        *outBytes = copy;
        *outLength = length;
        result = copy ? NULL : _strdup("Not enough memory to build the .docx file.");
    } else {
        const char *message = lua_isstring(L, -1) ? lua_tostring(L, -1) : "Could not build the .docx file.";
        result = _strdup(message);
    }
    lua_close(L);
    return result;
}
