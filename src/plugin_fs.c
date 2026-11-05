/* Exposes `fs.list_files`/`fs.read_text` to plugins: read-only, recursive
   folder scanning and whole-file reads, gated on a folder path the plugin
   already has explicitly (normally from ui.pick_folder). This mirrors
   sqlite.open (plugin_sqlite.c) as a second named exception to "plugins
   can't touch arbitrary files" (plugins/README.md) - a vault-wide feature
   like a wikilink/backlinks index needs to read every Markdown file under a
   folder, not just the one open in the editor, and there is no way to do
   that from the existing editor/ui surface. Keep both exceptions narrow
   and explicit rather than growing a general file API. */
#include "plugin_fs.h"

#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <windows.h>

#include "lauxlib.h"
#include "file_io.h"
#include "plugin_host.h"
#include "text_codec.h"

/* Case-insensitive glob match ('*' and '?' only - enough for "*.md"). */
static int wildcard_match(const WCHAR *name, const WCHAR *pattern) {
    if (*pattern == L'\0') return *name == L'\0';
    if (*pattern == L'*') {
        if (wildcard_match(name, pattern + 1)) return 1;
        return *name != L'\0' && wildcard_match(name + 1, pattern);
    }
    if (*name == L'\0') return 0;
    if (*pattern == L'?' || towlower((wint_t)*name) == towlower((wint_t)*pattern))
        return wildcard_match(name + 1, pattern + 1);
    return 0;
}

typedef struct {
    lua_State *L;
    const WCHAR *pattern;
    int count;
    ULONGLONG started;
    int timedOut;
} ListWalk;

/* Recurses into every subfolder, skipping reparse points (symlinks and
   junctions, so a cyclic link under the scanned folder can't recurse
   forever) and dot-folders (.git, .obsidian, ...), which would otherwise
   add noise and wasted work to a vault scan. Bails out, like a runaway
   SQL query (plugin_sqlite.c), after PLUGIN_TIME_LIMIT_MS - this walk runs
   in one C call, invisible to the Lua instruction-count time limit. */
static void walk_dir(ListWalk *walk, const WCHAR *dir) {
    if (walk->timedOut) return;
    WCHAR search[MAX_PATH];
    if (swprintf(search, MAX_PATH, L"%ls\\*", dir) < 0) return;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(search, &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        if (data.cFileName[0] == L'.') continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (GetTickCount64() - walk->started > PLUGIN_TIME_LIMIT_MS) { walk->timedOut = 1; break; }
        WCHAR full[MAX_PATH];
        if (swprintf(full, MAX_PATH, L"%ls\\%ls", dir, data.cFileName) < 0) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walk_dir(walk, full);
        } else if (wildcard_match(data.cFileName, walk->pattern)) {
            char *utf8 = to_utf8(full);
            if (utf8) {
                lua_pushstring(walk->L, utf8);
                free(utf8);
                lua_seti(walk->L, -2, ++walk->count);
            }
        }
    } while (!walk->timedOut && FindNextFileW(find, &data));
    FindClose(find);
}

/* fs.list_files(folder [, pattern]): every file under folder (recursively)
   whose name matches pattern (default "*", e.g. "*.md"), as absolute
   paths. Returns files, or nil, err. */
static int fs_list_files(lua_State *L) {
    size_t folderLength = 0;
    const char *folder = luaL_checklstring(L, 1, &folderLength);
    const char *patternUtf8 = luaL_optstring(L, 2, "*");
    WCHAR *wideFolder = to_wide_n(folder, folderLength, NULL);
    if (!wideFolder) {
        lua_pushnil(L);
        lua_pushstring(L, "folder must be valid UTF-8");
        return 2;
    }
    WCHAR *widePattern = to_wide_n(patternUtf8, strlen(patternUtf8), NULL);
    if (!widePattern) {
        free(wideFolder);
        lua_pushnil(L);
        lua_pushstring(L, "pattern must be valid UTF-8");
        return 2;
    }
    DWORD attrs = GetFileAttributesW(wideFolder);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        free(wideFolder);
        free(widePattern);
        lua_pushnil(L);
        lua_pushstring(L, "not a folder");
        return 2;
    }
    ListWalk walk = { L, widePattern, 0, GetTickCount64(), 0 };
    lua_newtable(L);
    walk_dir(&walk, wideFolder);
    free(wideFolder);
    free(widePattern);
    if (walk.timedOut) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushfstring(L, "stopped after %d seconds without finishing", PLUGIN_TIME_LIMIT_MS / 1000);
        return 2;
    }
    return 1;
}

/* fs.read_text(path): the whole file as UTF-8 text, decoded from whatever
   encoding it was actually saved as (same detection the editor uses to open
   a file). Returns text, or nil, err. */
static int fs_read_text(lua_State *L) {
    size_t pathLength = 0;
    const char *path = luaL_checklstring(L, 1, &pathLength);
    WCHAR *widePath = to_wide_n(path, pathLength, NULL);
    if (!widePath) {
        lua_pushnil(L);
        lua_pushstring(L, "path must be valid UTF-8");
        return 2;
    }
    int length = 0, encoding = 0, hasBom = 0;
    char *text = read_text_file(widePath, &length, &encoding, &hasBom);
    free(widePath);
    if (!text) {
        lua_pushnil(L);
        lua_pushstring(L, "could not read the file");
        return 2;
    }
    lua_pushlstring(L, text, (size_t)length);
    free(text);
    return 1;
}

void plugin_fs_install(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, fs_list_files);
    lua_setfield(L, -2, "list_files");
    lua_pushcfunction(L, fs_read_text);
    lua_setfield(L, -2, "read_text");
    lua_setglobal(L, "fs");
}
