#ifndef TINKEREDIT_PLUGIN_SQLITE_H
#define TINKEREDIT_PLUGIN_SQLITE_H

#include "lua.h"

/* Installs the plugin-facing `sqlite` table (open) as a global on L, backed
   by winsqlite3.dll - the SQLite build Windows 10/11 ships in System32 for
   its own UWP apps. Loaded dynamically by ordinary export name, so there is
   no bundled library and no new dependency.

   Unlike everything else plugins see, sqlite.open(path) really does reach
   the file system: it opens or creates an ordinary file at `path`. That is
   a deliberate, explicit exception to "plugins can't touch arbitrary files"
   (plugins/README.md) - see plugin_sqlite.c's header comment for the design
   this keeps to. Call once, from plugin_host.c's open_environment(), after
   the safe standard libraries are open. If winsqlite3.dll or a required
   export is missing, `sqlite` stays undefined rather than failing plugin
   startup entirely. */
void plugin_sqlite_install(lua_State *L);

#endif
