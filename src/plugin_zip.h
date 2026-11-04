#ifndef TINKEREDIT_PLUGIN_ZIP_H
#define TINKEREDIT_PLUGIN_ZIP_H

#include <stddef.h>

#include "lua.h"

/* Called once by plugin_bridge.inc, before the first plugin or built-in
   script loads (plugin_host.c's open_environment() needs it by then - see
   plugin_zip_install()). `source` is the builtin_script(IDR_ZIPTOOL_LUA, ...)
   resource bytes; this just remembers the pointer and length, not a copy -
   the resource stays mapped for the process lifetime. */
void plugin_host_set_ziptool_source(const char *source, size_t length);

/* Installs the plugin-facing `zip` table (pack/unpack) as a global on L,
   using src/lua/ziptool.lua - see plugin_zip.c's header comment for how
   that stays true to "plugins can't touch arbitrary files" even though
   ziptool.lua itself is written as if it always does. Call exactly once,
   from plugin_host.c's open_environment(), after the safe standard
   libraries are open. A missing/empty source installs nothing (zip stays
   undefined), rather than failing plugin startup entirely. */
void plugin_zip_install(lua_State *L);

#endif
