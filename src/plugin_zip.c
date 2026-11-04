/* Exposes `zip.pack`/`zip.unpack` to plugins, built on src/lua/ziptool.lua
   the same way docx_zip.c builds DOCX export on it, but installed once into
   the shared sandboxed plugin state (plugin_host.c's s_lua) instead of a
   throwaway one, since every plugin needs to see the same `zip` table.

   ziptool.lua is written as though it always has real file access (its
   top-level chunk captures `io.open` and `os.date` as upvalues the moment it
   loads). PLUGIN_ZIP_DRIVER below supplies throwaway `io`/`os`/`package`
   tables with just enough behaviour for the one function each operation
   calls (ZipWriter for pack, unzip_file for unpack) - a fake file handle
   that reads from or writes to a Lua string instead of touching disk. That
   keeps the actual promise "plugins can't touch arbitrary files or run
   programs" (plugins/README.md) intact: nothing here, or in ziptool.lua
   itself, ever reaches a real path, and the `io`/`os`/`package` globals this
   sets are local to the one protected call that installs `zip` - plugins
   never see them, only the `pack`/`unpack` functions this returns, stored in
   the registry rather than as upvalues of anything plugin-reachable.

   zip.unpack() runs a decompressor (ziptool's inflate_deflate) over
   plugin-supplied bytes - the usual zip-bomb memory/CPU concern a decoder
   like that raises is already covered by the sandbox's existing per-state
   64 MiB heap cap and 5-second time limit (plugin_lua_runtime.c), the same
   two limits every other plugin call already runs under. */
#include "plugin_zip.h"

#include <string.h>

#include "lauxlib.h"

static const char *s_ziptoolSource;
static size_t s_ziptoolLength;

void plugin_host_set_ziptool_source(const char *source, size_t length) {
    s_ziptoolSource = source;
    s_ziptoolLength = length;
}

/* Takes ziptoolSource via `...` and returns (pack, unpack). See this file's
   header comment for why the fake handles and the throwaway io/os/package
   tables are shaped the way they are. */
static const char PLUGIN_ZIP_DRIVER[] =
    "local ziptoolSource = ...\n"
    "local sep = '/'\n"
    "local function makeWriteHandle()\n"
    "  local chunks = {}\n"
    "  local h = { chunks = chunks }\n"
    "  function h:write(s) chunks[#chunks + 1] = s; return true end\n"
    "  function h:close() return true end\n"
    "  return h\n"
    "end\n"
    "local function makeReadHandle(data)\n"
    "  local pos = 0\n"
    "  local h = {}\n"
    "  function h:read(n)\n"
    "    if n == 0 then return '' end\n" /* a 0-length read succeeds even at EOF - an empty archive's 0-byte central directory needs this */
    "    if pos >= #data then return nil end\n"
    "    local chunk = data:sub(pos + 1, pos + n)\n"
    "    pos = pos + #chunk\n"
    "    return chunk\n"
    "  end\n"
    "  function h:seek(whence, offset)\n"
    "    offset = offset or 0\n"
    "    if whence == 'set' then pos = offset\n"
    "    elseif whence == 'end' then pos = #data + offset\n"
    "    else pos = pos + offset end\n"
    "    return pos\n"
    "  end\n"
    "  function h:close() return true end\n"
    "  return h\n"
    "end\n"
    "local currentInput, currentWrites\n"
    "local INPUT = '\\1zip-input\\1'\n"
    "io = { open = function(path)\n"
    "  if path == INPUT then return makeReadHandle(currentInput) end\n"
    "  local h = makeWriteHandle()\n"
    "  currentWrites[path] = h\n"
    "  return h\n"
    "end }\n"
    "os = { date = function() return {} end, execute = function() return true end }\n"
    "package = { config = sep .. '\\n;\\n?\\n!\\n-\\n' }\n"
    "local chunkFn, loadErr = load(ziptoolSource, 'ziptool.lua')\n"
    "if not chunkFn then error(loadErr) end\n"
    "local ziptool = chunkFn()\n"
    "io, os, package = nil, nil, nil\n" /* ziptool.lua already captured what it needs as upvalues */
    "local function pack(entries)\n"
    "  if type(entries) ~= 'table' then return nil, 'entries must be a table' end\n"
    "  currentWrites = {}\n"
    "  local zw, zerr = ziptool.ZipWriter.new('output', { compress = false })\n"
    "  if not zw then return nil, zerr end\n"
    "  for i = 1, #entries do\n"
    "    local e = entries[i]\n"
    "    if type(e) ~= 'table' or type(e.name) ~= 'string' or type(e.data) ~= 'string' then\n"
    "      return nil, 'each entry needs a string name and a string data field'\n"
    "    end\n"
    "    local ok, addErr = zw:add_data(e.name, e.data)\n"
    "    if not ok then return nil, addErr end\n"
    "  end\n"
    "  local ok2, closeErr = zw:close()\n"
    "  if not ok2 then return nil, closeErr end\n"
    "  local output = currentWrites.output\n"
    "  return output and table.concat(output.chunks) or ''\n"
    "end\n"
    "local function unpack(bytes)\n"
    "  if type(bytes) ~= 'string' then return nil, 'zip bytes must be a string' end\n"
    "  currentInput = bytes\n"
    "  currentWrites = {}\n"
    /* unzip_file() reads `package.config` and calls `os.execute()` directly
       as globals at call time (unlike io_open/os_date, these were never
       captured as upvalues at module-load time), so - unlike io, which
       stays nil the whole time - os/package have to be live again for just
       this one call, then cleared immediately after it returns, success or
       not, so they're never visible to anything else running in this
       state. */
    "  os = { execute = function() return true end }\n"
    "  package = { config = sep .. '\\n;\\n?\\n!\\n-\\n' }\n"
    "  local callOk, resultOk, resultErr = pcall(ziptool.unzip_file, INPUT, '')\n"
    "  os, package = nil, nil\n"
    "  if not callOk then return nil, tostring(resultOk) end\n"
    "  if not resultOk then return nil, resultErr end\n"
    "  local result = {}\n"
    "  for path, handle in pairs(currentWrites) do\n"
    "    result[#result + 1] = { name = (path:gsub('^' .. sep, '')), data = table.concat(handle.chunks) }\n"
    "  end\n"
    "  return result\n"
    "end\n"
    "return pack, unpack\n";

static int call_stored(lua_State *L, const char *registryKey, int nargsFromStack) {
    lua_getfield(L, LUA_REGISTRYINDEX, registryKey);
    if (lua_isnil(L, -1)) return luaL_error(L, "%s is not available", registryKey);
    for (int i = 1; i <= nargsFromStack; i++) lua_pushvalue(L, i);
    if (lua_pcall(L, nargsFromStack, 2, 0) != LUA_OK) return lua_error(L);
    return 2;
}

static int lua_zip_pack(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    return call_stored(L, "tinkeredit.zip.pack", 1);
}

static int lua_zip_unpack(lua_State *L) {
    luaL_checkstring(L, 1);
    return call_stored(L, "tinkeredit.zip.unpack", 1);
}

void plugin_zip_install(lua_State *L) {
    if (!s_ziptoolSource || !s_ziptoolLength) return;
    if (luaL_loadbuffer(L, PLUGIN_ZIP_DRIVER, sizeof(PLUGIN_ZIP_DRIVER) - 1, "plugin_zip") != LUA_OK) {
        lua_pop(L, 1); /* the install failing just leaves `zip` undefined */
        return;
    }
    lua_pushlstring(L, s_ziptoolSource, s_ziptoolLength);
    if (lua_pcall(L, 1, 2, 0) != LUA_OK) {
        lua_pop(L, 1);
        return;
    }
    lua_setfield(L, LUA_REGISTRYINDEX, "tinkeredit.zip.unpack"); /* pops the 2nd return value */
    lua_setfield(L, LUA_REGISTRYINDEX, "tinkeredit.zip.pack");   /* pops the 1st */

    lua_newtable(L);
    lua_pushcfunction(L, lua_zip_pack);
    lua_setfield(L, -2, "pack");
    lua_pushcfunction(L, lua_zip_unpack);
    lua_setfield(L, -2, "unpack");
    lua_setglobal(L, "zip");
}
