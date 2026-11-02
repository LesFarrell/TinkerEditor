#include "path_utils.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

WCHAR *wcsdup_heap(const WCHAR *value) {
    if (!value) return NULL;
    size_t length = wcslen(value);
    if (length > SIZE_MAX / sizeof(WCHAR) - 1) return NULL;
    size_t bytes = (length + 1) * sizeof(WCHAR);
    WCHAR *copy = (WCHAR *)malloc(bytes);
    if (copy) memcpy(copy, value, bytes);
    return copy;
}

void replace_wstring(WCHAR **target, const WCHAR *value) {
    WCHAR *copy = value ? wcsdup_heap(value) : NULL;
    if (value && !copy) return;
    free(*target);
    *target = copy;
}

WCHAR *get_module_path(void) {
    DWORD capacity = 512;
    for (;;) {
        WCHAR *path = (WCHAR *)malloc((size_t)capacity * sizeof(WCHAR));
        if (!path) return NULL;
        DWORD length = GetModuleFileNameW(NULL, path, capacity);
        if (length > 0 && length < capacity) return path;
        free(path);
        if (capacity >= LONG_PATH_CAPACITY) return NULL;
        capacity *= 2;
    }
}

WCHAR *module_directory(void) {
    WCHAR *module = get_module_path();
    WCHAR *directory = module ? path_directory(module) : NULL;
    free(module);
    return directory;
}

WCHAR *path_directory(const WCHAR *path) {
    WCHAR *directory = wcsdup_heap(path);
    if (!directory) return NULL;
    WCHAR *slash = wcsrchr(directory, L'\\');
    WCHAR *forward = wcsrchr(directory, L'/');
    if (forward && (!slash || forward > slash)) slash = forward;
    if (slash) {
        if (slash == directory + 2 && directory[1] == L':') slash[1] = 0;
        else *slash = 0;
    } else {
        free(directory);
        return wcsdup_heap(L".");
    }
    return directory;
}

WCHAR *path_join(const WCHAR *directory, const WCHAR *name) {
    if (!directory || !name) return NULL;
    size_t directoryLength = wcslen(directory);
    size_t nameLength = wcslen(name);
    if (nameLength > SIZE_MAX - 2 || directoryLength > SIZE_MAX - nameLength - 2)
        return NULL;
    int addSlash = directoryLength > 0 && directory[directoryLength - 1] != L'\\' &&
                   directory[directoryLength - 1] != L'/';
    WCHAR *joined = (WCHAR *)malloc(
        (directoryLength + (size_t)addSlash + nameLength + 1) * sizeof(WCHAR));
    if (!joined) return NULL;
    memcpy(joined, directory, directoryLength * sizeof(WCHAR));
    if (addSlash) joined[directoryLength++] = L'\\';
    memcpy(joined + directoryLength, name, (nameLength + 1) * sizeof(WCHAR));
    return joined;
}

WCHAR *absolute_path(const WCHAR *path) {
    if (!path) return NULL;
    DWORD capacity = GetFullPathNameW(path, 0, NULL, NULL);
    if (!capacity) return wcsdup_heap(path);
    for (;;) {
        WCHAR *full = (WCHAR *)malloc((size_t)capacity * sizeof(WCHAR));
        if (!full) return NULL;
        DWORD length = GetFullPathNameW(path, capacity, full, NULL);
        if (length > 0 && length < capacity) return full;
        free(full);
        if (!length) return NULL;
        capacity = (length == capacity) ? capacity + 1 : length;
    }
}

WCHAR *extended_path(const WCHAR *path) {
    WCHAR *full = absolute_path(path);
    if (!full || wcsncmp(full, L"\\\\?\\", 4) == 0) return full;
    const WCHAR *prefix = wcsncmp(full, L"\\\\", 2) == 0 ? L"\\\\?\\UNC\\" : L"\\\\?\\";
    const WCHAR *tail = wcsncmp(full, L"\\\\", 2) == 0 ? full + 2 : full;
    size_t prefixLength = wcslen(prefix), tailLength = wcslen(tail);
    if (tailLength > SIZE_MAX / sizeof(WCHAR) - prefixLength - 1) {
        free(full);
        return NULL;
    }
    size_t length = prefixLength + tailLength + 1;
    WCHAR *extended = (WCHAR *)malloc(length * sizeof(WCHAR));
    if (extended) swprintf(extended, length, L"%ls%ls", prefix, tail);
    free(full);
    return extended;
}

int portable_mode_enabled(void) {
    WCHAR *directory = module_directory();
    WCHAR *marker = directory ? path_join(directory, L"portable.mode") : NULL;
    int enabled = file_exists(marker);
    free(marker);
    free(directory);
    return enabled;
}

/* Settings, menu/theme customisations and personal plugins share this root.
   Keeping portable state in its own folder avoids confusing personal plugins
   with the bundled plugins directory beside the executable. */
WCHAR *get_user_config_directory(void) {
    if (portable_mode_enabled()) {
        WCHAR *module = module_directory();
        WCHAR *portable = module ? path_join(module, L"portable-data") : NULL;
        free(module);
        if (portable) CreateDirectoryW(portable, NULL);
        return portable;
    }

    DWORD capacity = GetEnvironmentVariableW(L"APPDATA", NULL, 0);
    WCHAR *appData = NULL;
    while (capacity) {
        appData = (WCHAR *)malloc((size_t)capacity * sizeof(WCHAR));
        if (!appData) return NULL;
        DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, capacity);
        if (length > 0 && length < capacity) break;
        free(appData);
        appData = NULL;
        if (!length) return NULL;
        capacity = (length == capacity) ? capacity + 1 : length;
    }
    if (!appData) return NULL;
    WCHAR *directory = path_join(appData, L"tinkeredit");
    free(appData);
    if (directory) CreateDirectoryW(directory, NULL);
    return directory;
}

/* Shared by the workspace-scoped file enumeration (Quick Open/Find-in-Folder,
   in tinkeredit.c) and the file explorer's live watcher (file_explorer.c), so
   both agree on which directories are noisy/uninteresting to recurse into. */
static const WCHAR *const SKIPPED_WORKSPACE_DIRECTORIES[] = {
    L".git", L".svn", L".hg", L"node_modules", L"build", L"dist", L"Output",
    L"webview2_data", L".vs", L".vscode"
};

static int skipped_workspace_directory_n(const WCHAR *name, size_t length) {
    for (size_t i = 0; i < sizeof(SKIPPED_WORKSPACE_DIRECTORIES) /
                            sizeof(SKIPPED_WORKSPACE_DIRECTORIES[0]); i++) {
        const WCHAR *skipped = SKIPPED_WORKSPACE_DIRECTORIES[i];
        if (wcslen(skipped) == length && _wcsnicmp(name, skipped, length) == 0) return 1;
    }
    return 0;
}

int skipped_workspace_directory(const WCHAR *name) {
    return name && skipped_workspace_directory_n(name, wcslen(name));
}

/* True if any path component (not just the last one) is a skipped
   directory name - e.g. ".git\objects\ab\cdef" needs this to be caught by
   the ".git" check even though the file's own immediate parent is "ab",
   not ".git". Used by the file explorer's watcher to filter change
   notifications from deep inside a skipped directory, where checking only
   the immediate parent would miss almost everything git itself writes. */
int path_has_skipped_component(const WCHAR *path) {
    if (!path) return 0;
    const WCHAR *component = path;
    while (*component) {
        while (*component == L'\\' || *component == L'/') component++;
        const WCHAR *end = component;
        while (*end && *end != L'\\' && *end != L'/') end++;
        size_t length = (size_t)(end - component);
        if (length && skipped_workspace_directory_n(component, length)) return 1;
        component = end;
    }
    return 0;
}

/* %LOCALAPPDATA%\TinkerEditor\<name>, created if needed: normally per-user,
   machine-local state that stays writable wherever tinkeredit.exe lives -
   including a Program Files install a standard user can't write to. Portable
   mode redirects it under portable-data beside the executable instead. */
WCHAR *get_local_app_directory(const WCHAR *name) {
    if (portable_mode_enabled()) {
        WCHAR *root = get_user_config_directory();
        WCHAR *directory = root ? path_join(root, name) : NULL;
        free(root);
        if (directory) CreateDirectoryW(directory, NULL);
        return directory;
    }
    DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", NULL, 0);
    if (!needed) return NULL;
    WCHAR *local = (WCHAR *)malloc((size_t)needed * sizeof(WCHAR));
    if (!local || !GetEnvironmentVariableW(L"LOCALAPPDATA", local, needed)) {
        free(local);
        return NULL;
    }
    WCHAR *app = path_join(local, L"TinkerEditor");
    free(local);
    if (!app) return NULL;
    CreateDirectoryW(app, NULL);
    WCHAR *directory = path_join(app, name);
    free(app);
    if (directory) CreateDirectoryW(directory, NULL);
    return directory;
}

int file_exists(const WCHAR *path) {
    DWORD attributes = path ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES;
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
