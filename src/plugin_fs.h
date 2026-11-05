#ifndef TINKEREDIT_PLUGIN_FS_H
#define TINKEREDIT_PLUGIN_FS_H

#include "lua.h"

/* Installs the plugin-facing `fs` table (list_files, read_text) as a global
   on L. Like sqlite.open (plugin_sqlite.c), this is a second deliberate,
   explicit exception to "plugins can't touch arbitrary files"
   (plugins/README.md): once a plugin has an explicit folder path (e.g. from
   ui.pick_folder), fs.list_files/fs.read_text really do reach the file
   system, read-only, for things like a vault-wide wikilink index that
   needs every Markdown file in a folder, not just the open document.

   Call once, from plugin_host.c's open_environment(), after the safe
   standard libraries are open. */
void plugin_fs_install(lua_State *L);

#endif
