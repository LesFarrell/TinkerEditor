/*
 * file_explorer.c - the docked file-explorer side panel: a live TreeView over
 * the current workspace folder, with New/Rename/Delete and a background
 * ReadDirectoryChangesW watcher that keeps it in sync with external changes.
 *
 * tinkeredit.h's own forward declarations are static to tinkeredit.c's
 * translation unit (that header is only ever included there), so this file
 * cannot call tinkeredit.c functions like open_file_in_tab() or
 * show_text_input() by name. The operations it needs are instead handed in
 * once as plain function pointers by explorer_init().
 */
#define COBJMACROS

#include "file_explorer.h"

#include <commctrl.h>
#include <process.h>
#include <shellapi.h>
#include <shlobj.h> /* SHCreateItemFromParsingName() */
#include <shlwapi.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uxtheme.h>
#include <wchar.h>
#include <windowsx.h>
#include <winnetwk.h>

#include "command_ids.h" /* IDM_VIEW_EXPLORER, reused as the close button's id */
#include "path_utils.h"
#include "settings_values.h"
#include "theme.h"
#include "themed_scrollbar.h"

HWND g_hExplorerTree = NULL;
/* The close button above the tree. Its id is IDM_VIEW_EXPLORER itself -
   the same command the View menu's "File Explorer" toggle already sends -
   so clicking it reaches toggle_explorer_panel() exactly as that menu item
   does, with no separate dispatch case to keep in sync. Owner-drawn with
   tinkeredit.c's own draw_debug_panel_button() (see handle_owner_draw() in
   tinkeredit.c), the same painter the output and Git panels' own close
   buttons use, so all three match. */
HWND g_hExplorerClose = NULL;
/* g_explorerVisible, g_explorerWidth and g_explorerWidthRatio are defined in
   layout_metrics.c (declared in layout_metrics.h). */

typedef struct ExplorerItemData
{
    WCHAR *fullPath;
    int isDirectory;
    int isParentLink;   /* synthetic ".." entry directly beneath the workspace root */
    int isRootChoice;   /* drive/share entry in the virtual roots view */
    int childrenLoaded; /* directories only: has this node's real children been scanned yet? */
} ExplorerItemData;

typedef struct ExplorerWatcher
{
    HANDLE thread;
    HANDLE dirHandle;
    HANDLE stopEvent;
    OVERLAPPED overlapped;
    WCHAR *root;
    BYTE buffer[64 * 1024];
} ExplorerWatcher;

enum
{
    CTX_OPEN = 1,
    CTX_NEW_FILE,
    CTX_NEW_FOLDER,
    CTX_RENAME,
    CTX_DELETE,
    CTX_COPY_PATH,
    CTX_COPY_WIKILINK,
    CTX_REVEAL,
    CTX_REFRESH,
    CTX_OPEN_PARENT
};

static HWND s_hParent = NULL;
static ExplorerOpenFileFn s_openFile = NULL;
static ExplorerPromptTextFn s_promptText = NULL;
static ExplorerRevealFn s_revealPath = NULL;
static ExplorerRenameFn s_renamedPath = NULL;
static ExplorerChangeRootFn s_changeRoot = NULL;
static ExplorerDropTextFn s_dropText = NULL;
static WCHAR *s_root = NULL;
static HTREEITEM s_rootItem = NULL;
static int s_showingRoots = 0;
static ExplorerWatcher *s_watcher = NULL;
static WCHAR *s_pendingRootChoice = NULL;  /* queued drive/share to adopt, see queue_navigation() */
static int s_pendingParentNavigation = 0; /* queued move up one folder */

/* Posted to the tree itself (handled by its subclass), so it never collides
   with the WM_APP messages tinkeredit.c's main window uses. */
#define EXPLORER_WM_NAVIGATE (WM_APP + 1)

static CRITICAL_SECTION s_pendingLock;
static WCHAR **s_pendingDirs = NULL;
static int s_pendingCount = 0;
static int s_pendingCapacity = 0;

/* ---- dragging a file out of the tree to drop its wikilink elsewhere ----
   A TreeView has no built-in "drag this item out" gesture; TVN_BEGINDRAGW
   just reports that one started, and it's on the control (really this
   subclass, via SetCapture()) to track the mouse until the button comes up -
   the same manual pattern Win32 TreeView drag samples have always used.
   Only a plain file can be dragged: directories and the parent-folder/
   root-choice placeholder items have nothing to drop. */
static int s_dragging = 0;
static WCHAR *s_dragPath = NULL;
static HIMAGELIST s_dragImageList = NULL;

static void handle_keyboard_context_menu(void);

#define ID_EXPLORER_TREE 900

/* ---- icon cache: a private image list, indexed by extension ---- */

typedef struct IconCacheEntry
{
    WCHAR extension[16];
    int iconIndex;
} IconCacheEntry;

#define ICON_CACHE_CAPACITY 64
static IconCacheEntry s_iconCache[ICON_CACHE_CAPACITY];
static int s_iconCacheCount = 0;
static int s_folderIconIndex = -1;
static int s_fileIconIndex = -1;

/* Returns a newly allocated parent path, or NULL when the workspace is
   already a drive/UNC root (or parent navigation is unavailable). */
static WCHAR *root_parent_path(void)
{
    if (!s_root || !s_changeRoot || PathIsRootW(s_root))
        return NULL;
    WCHAR *parent = path_directory(s_root);
    if (parent && _wcsicmp(parent, s_root) == 0)
    {
        free(parent);
        return NULL;
    }
    return parent;
}

static int can_navigate_up(void)
{
    if (!s_root || !s_changeRoot || s_showingRoots)
        return 0;
    if (PathIsRootW(s_root))
        return 1; /* opens the virtual drive/share list */
    WCHAR *parent = root_parent_path();
    int available = parent != NULL;
    free(parent);
    return available;
}

/* A private ILC_COLOR32 list with ImageList_SetBkColor(CLR_NONE) - rather
   than attaching the shared shell system image list via SHGFI_SYSICONINDEX -
   so icons composite with real alpha against the tree's own (dark-mode-aware)
   background instead of carrying an opaque square from whatever background
   the shared list itself assumes. */
static HIMAGELIST s_iconList = NULL;

static int add_icon_to_list(HICON icon)
{
    if (!icon || !s_iconList)
        return -1;
    int index = ImageList_AddIcon(s_iconList, icon);
    DestroyIcon(icon);
    return index;
}

/* Shell icon for a name with the given attributes, added to s_iconList once
   and remembered in *slot. */
static int shared_icon_index(int *slot, const WCHAR *name, DWORD attributes)
{
    if (*slot < 0)
    {
        SHFILEINFOW info = {0};
        SHGetFileInfoW(name, attributes, &info, sizeof(info),
                       SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        *slot = add_icon_to_list(info.hIcon);
    }
    return *slot;
}

/* Every icon added to s_iconList stays there, so a file only gets its own
   icon when its extension can be cached. Extensionless names, extensions too
   long for an entry, and anything after the cache fills share one generic
   file icon, keeping the list bounded however many files are listed. */
static int icon_index_for_path(const WCHAR *path, int isDirectory)
{
    if (isDirectory)
        return shared_icon_index(&s_folderIconIndex, L"folder", FILE_ATTRIBUTE_DIRECTORY);
    const WCHAR *extension = PathFindExtensionW(path);
    for (int i = 0; i < s_iconCacheCount; i++)
        if (_wcsicmp(s_iconCache[i].extension, extension) == 0)
            return s_iconCache[i].iconIndex;
    size_t extensionCapacity = sizeof(s_iconCache[0].extension) / sizeof(WCHAR);
    if (!*extension || wcslen(extension) >= extensionCapacity ||
        s_iconCacheCount >= ICON_CACHE_CAPACITY)
        return shared_icon_index(&s_fileIconIndex, L"file", FILE_ATTRIBUTE_NORMAL);
    SHFILEINFOW info = {0};
    SHGetFileInfoW(extension, FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                   SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    int index = add_icon_to_list(info.hIcon);
    wcscpy(s_iconCache[s_iconCacheCount].extension, extension);
    s_iconCache[s_iconCacheCount].iconIndex = index;
    s_iconCacheCount++;
    return index;
}

/* ---- tree item helpers ----
   TVM_INSERTITEM/TVM_GETITEM have distinct ANSI/Unicode message codes and
   this project defines neither UNICODE nor _UNICODE, so (matching how
   tinkeredit.c itself always sends TCM_INSERTITEMW explicitly rather than
   relying on the ambiguous TabCtrl_InsertItem macro) these two go through
   explicit *_W messages instead of the generic TreeView_InsertItem/GetItem
   macros. Every other TreeView_* macro used below wraps a message with no
   such ambiguity (no text payload), so those are used as-is. */

static HTREEITEM tree_insert_item(TVINSERTSTRUCTW *insert)
{
    return (HTREEITEM)SendMessageW(g_hExplorerTree, TVM_INSERTITEMW, 0, (LPARAM)insert);
}

static ExplorerItemData *tree_item_data(HTREEITEM item)
{
    if (!item)
        return NULL;
    TVITEMW tvItem = {0};
    tvItem.mask = TVIF_PARAM;
    tvItem.hItem = item;
    SendMessageW(g_hExplorerTree, TVM_GETITEMW, 0, (LPARAM)&tvItem);
    return (ExplorerItemData *)tvItem.lParam;
}

static void delete_all_children(HTREEITEM item)
{
    HTREEITEM child = TreeView_GetChild(g_hExplorerTree, item);
    while (child)
    {
        HTREEITEM next = TreeView_GetNextSibling(g_hExplorerTree, child);
        TreeView_DeleteItem(g_hExplorerTree, child);
        child = next;
    }
}

static void insert_placeholder_child(HTREEITEM parent)
{
    TVINSERTSTRUCTW dummy = {0};
    dummy.hParent = parent;
    dummy.hInsertAfter = TVI_LAST;
    dummy.item.mask = TVIF_TEXT | TVIF_PARAM;
    dummy.item.pszText = L"";
    dummy.item.lParam = 0;
    tree_insert_item(&dummy);
}

static HTREEITEM insert_tree_item(HTREEITEM parent, HTREEITEM insertAfter,
                                  const WCHAR *fullPath, const WCHAR *label, int isDirectory)
{
    ExplorerItemData *data = (ExplorerItemData *)calloc(1, sizeof(ExplorerItemData));
    if (!data)
        return NULL;
    data->fullPath = wcsdup_heap(fullPath);
    data->isDirectory = isDirectory;
    if (!data->fullPath)
    {
        free(data);
        return NULL;
    }

    TVINSERTSTRUCTW insert = {0};
    insert.hParent = parent;
    insert.hInsertAfter = insertAfter;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    insert.item.pszText = (WCHAR *)label;
    insert.item.lParam = (LPARAM)data;
    insert.item.iImage = icon_index_for_path(fullPath, isDirectory);
    insert.item.iSelectedImage = insert.item.iImage;
    HTREEITEM created = tree_insert_item(&insert);
    if (!created)
    {
        free(data->fullPath);
        free(data);
        return NULL;
    }

    if (isDirectory)
    {
        /* An unconditional placeholder child gives the expand glyph without
           scanning the directory twice; TVN_ITEMEXPANDINGW deletes it and
           inserts the real children (or nothing, if it's actually empty)
           the first time the user expands this node. */
        insert_placeholder_child(created);
    }
    return created;
}

static void insert_root_choice(const WCHAR *path, const WCHAR *label)
{
    HTREEITEM item = insert_tree_item(TVI_ROOT, TVI_LAST, path, label, 1);
    if (!item)
        return;
    ExplorerItemData *data = tree_item_data(item);
    if (data)
        data->isRootChoice = 1;
    /* Root choices switch workspace; they are not expanded in place. */
    delete_all_children(item);
}

static void show_roots_view(void)
{
    if (!g_hExplorerTree || !s_root)
        return;
    explorer_stop_watch();
    themed_scrollbar_begin_update(g_hExplorerTree);
    TreeView_DeleteAllItems(g_hExplorerTree);
    s_rootItem = NULL;
    s_showingRoots = 1;

    DWORD capacity = GetLogicalDriveStringsW(0, NULL);
    WCHAR *drives = capacity ? (WCHAR *)calloc((size_t)capacity + 1, sizeof(WCHAR)) : NULL;
    if (drives && GetLogicalDriveStringsW(capacity, drives))
    {
        for (const WCHAR *drive = drives; *drive; drive += wcslen(drive) + 1)
        {
            WCHAR label[1200];
            wcscpy(label, drive);
            if (GetDriveTypeW(drive) == DRIVE_REMOTE)
            {
                WCHAR remote[1024];
                DWORD remoteLength = (DWORD)(sizeof(remote) / sizeof(remote[0]));
                WCHAR device[3] = {drive[0], L':', 0};
                if (WNetGetConnectionW(device, remote, &remoteLength) == NO_ERROR)
                    swprintf(label, sizeof(label) / sizeof(label[0]), L"%ls  (%ls)", drive, remote);
            }
            insert_root_choice(drive, label);
        }
    }
    free(drives);

    /* An un-mapped UNC share is not returned by GetLogicalDriveStringsW, so
       retain it explicitly when it was the workspace we navigated up from. */
    if (PathIsUNCW(s_root))
        insert_root_choice(s_root, s_root);
    themed_scrollbar_end_update(g_hExplorerTree);
}

typedef struct DirEntry
{
    WCHAR *name;
    int isDirectory;
} DirEntry;

/* Sort order for the tree: directories before files, then case-insensitive
   alphabetical within each group - matches Explorer. */
static int compare_dir_entries(const void *a, const void *b)
{
    const DirEntry *entryA = (const DirEntry *)a;
    const DirEntry *entryB = (const DirEntry *)b;
    if (entryA->isDirectory != entryB->isDirectory)
        return entryA->isDirectory ? -1 : 1;
    int order = _wcsicmp(entryA->name, entryB->name);
    return order ? order : wcscmp(entryA->name, entryB->name);
}

static void free_dir_entries(DirEntry *entries, int count)
{
    for (int i = 0; i < count; i++)
        free(entries[i].name);
    free(entries);
}

/* Take a complete, sorted snapshot before changing the control. If the scan
   runs out of memory, leave the existing tree intact rather than treating a
   partial snapshot as a set of deleted files. */
static int read_directory_entries(const WCHAR *path, DirEntry **result)
{
    WCHAR *pattern = path_join(path, L"*");
    DirEntry *entries = NULL;
    int count = 0;
    int complete = pattern != NULL;
    if (pattern)
    {
        WIN32_FIND_DATAW findData;
        HANDLE search = FindFirstFileW(pattern, &findData);
        free(pattern);
        if (search != INVALID_HANDLE_VALUE)
        {
            int capacity = 0;
            do
            {
                if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0)
                    continue;
                if (count >= capacity)
                {
                    int nextCapacity = capacity ? capacity * 2 : 32;
                    DirEntry *grown = (DirEntry *)realloc(entries, (size_t)nextCapacity * sizeof(DirEntry));
                    if (!grown)
                    {
                        complete = 0;
                        break;
                    }
                    entries = grown;
                    capacity = nextCapacity;
                }
                entries[count].name = wcsdup_heap(findData.cFileName);
                if (!entries[count].name)
                {
                    complete = 0;
                    break;
                }
                entries[count].isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
                count++;
            } while (FindNextFileW(search, &findData));
            if (complete && GetLastError() != ERROR_NO_MORE_FILES)
                complete = 0;
            FindClose(search);
        }
        else if (GetLastError() != ERROR_FILE_NOT_FOUND)
        {
            complete = 0;
        }
    }
    if (!complete)
    {
        free_dir_entries(entries, count);
        *result = NULL;
        return -1;
    }
    if (count > 0)
        qsort(entries, (size_t)count, sizeof(DirEntry), compare_dir_entries);
    *result = entries;
    return count;
}

static void populate_directory_children(HTREEITEM item, ExplorerItemData *data)
{
    if (item == s_rootItem)
    {
        WCHAR *parent = PathIsRootW(s_root) ? wcsdup_heap(s_root) : root_parent_path();
        if (parent && can_navigate_up())
        {
            HTREEITEM parentItem = insert_tree_item(item, TVI_LAST, parent, L"..", 1);
            if (parentItem)
            {
                ExplorerItemData *parentData = tree_item_data(parentItem);
                if (parentData)
                    parentData->isParentLink = 1;
                /* ".." is an action, not an expandable view of the parent. */
                delete_all_children(parentItem);
            }
            free(parent);
        }
    }

    DirEntry *entries = NULL;
    int count = read_directory_entries(data->fullPath, &entries);
    if (count < 0)
    {
        /* TVN_ITEMEXPANDING removed the original placeholder before this
           scan. Restore it so an unavailable/network directory can be
           collapsed and retried instead of becoming permanently leaf-like. */
        insert_placeholder_child(item);
        return;
    }
    for (int i = 0; i < count; i++)
    {
        WCHAR *full = path_join(data->fullPath, entries[i].name);
        if (full)
            insert_tree_item(item, TVI_LAST, full, entries[i].name, entries[i].isDirectory);
        free(full);
    }
    free_dir_entries(entries, count);
    data->childrenLoaded = 1;
}

static void reload_directory_children(HTREEITEM item, ExplorerItemData *data)
{
    DirEntry *entries = NULL;
    int count = read_directory_entries(data->fullPath, &entries);
    if (count < 0)
        return;

    themed_scrollbar_begin_update(g_hExplorerTree);
    /* Both lists are already in directory-first, case-insensitive order, so
       merge them in one pass. Unchanged TreeView items stay alive, preserving
       selection and expanded descendants. Crucially, do not use WM_SETREDRAW:
       it passes through the themed-scrollbar subclass, where temporarily
       hiding the TreeView also moves its sibling overlay windows and can
       corrupt the editor's repaint/focus state. TVS_EX_DOUBLEBUFFER presents
       these small row-level changes atomically. */
    HTREEITEM current = TreeView_GetChild(g_hExplorerTree, item);
    ExplorerItemData *currentData = tree_item_data(current);
    HTREEITEM previous = NULL;
    if (currentData && currentData->isParentLink)
    {
        previous = current;
        current = TreeView_GetNextSibling(g_hExplorerTree, current);
    }

    int entryIndex = 0;
    while (current || entryIndex < count)
    {
        currentData = tree_item_data(current);
        int order;
        if (!current)
        {
            order = 1;
        }
        else if (!currentData || currentData->isParentLink)
        {
            order = -1;
        }
        else if (entryIndex >= count)
        {
            order = -1;
        }
        else if (currentData->isDirectory != entries[entryIndex].isDirectory)
        {
            order = currentData->isDirectory ? -1 : 1;
        }
        else
        {
            const WCHAR *currentName = PathFindFileNameW(currentData->fullPath);
            order = _wcsicmp(currentName, entries[entryIndex].name);
            if (!order)
                order = wcscmp(currentName, entries[entryIndex].name);
        }

        if (order < 0)
        {
            HTREEITEM next = TreeView_GetNextSibling(g_hExplorerTree, current);
            TreeView_DeleteItem(g_hExplorerTree, current);
            current = next;
        }
        else if (order > 0)
        {
            WCHAR *full = path_join(data->fullPath, entries[entryIndex].name);
            if (full)
            {
                HTREEITEM inserted = insert_tree_item(item, previous ? previous : TVI_FIRST,
                                                      full, entries[entryIndex].name,
                                                      entries[entryIndex].isDirectory);
                if (inserted)
                    previous = inserted;
                free(full);
            }
            entryIndex++;
        }
        else
        {
            const WCHAR *oldName = PathFindFileNameW(currentData->fullPath);
            if (wcscmp(oldName, entries[entryIndex].name) != 0)
            {
                WCHAR *full = path_join(data->fullPath, entries[entryIndex].name);
                if (full)
                {
                    TVITEMW changed = {0};
                    changed.mask = TVIF_TEXT;
                    changed.hItem = current;
                    changed.pszText = entries[entryIndex].name;
                    SendMessageW(g_hExplorerTree, TVM_SETITEMW, 0, (LPARAM)&changed);
                    free(currentData->fullPath);
                    currentData->fullPath = full;
                }
            }
            previous = current;
            current = TreeView_GetNextSibling(g_hExplorerTree, current);
            entryIndex++;
        }
    }

    free_dir_entries(entries, count);
    data->childrenLoaded = 1;
    themed_scrollbar_end_update(g_hExplorerTree);
}

static HTREEITEM find_loaded_item_for_path(HTREEITEM item, const WCHAR *path)
{
    if (!item)
        return NULL;
    ExplorerItemData *data = tree_item_data(item);
    if (data && _wcsicmp(data->fullPath, path) == 0)
        return item;
    if (data && data->isDirectory && data->childrenLoaded)
    {
        HTREEITEM child = TreeView_GetChild(g_hExplorerTree, item);
        while (child)
        {
            HTREEITEM found = find_loaded_item_for_path(child, path);
            if (found)
                return found;
            child = TreeView_GetNextSibling(g_hExplorerTree, child);
        }
    }
    return NULL;
}

/* Like find_loaded_item_for_path(), but also walks top-level siblings, so it
   works in the roots view too (one top-level item per drive/share, with
   s_rootItem NULL) as well as the normal single-root workspace view. */
static HTREEITEM find_item_for_path(const WCHAR *path)
{
    if (!path)
        return NULL;
    for (HTREEITEM top = TreeView_GetRoot(g_hExplorerTree); top;
        top = TreeView_GetNextSibling(g_hExplorerTree, top))
    {
        HTREEITEM found = find_loaded_item_for_path(top, path);
        if (found)
            return found;
    }
    return NULL;
}

/* Synthetic parent links can have the same path as their root item (at a
   drive root), so a context-menu item needs its kind as well as its path to
   be found again after a live refresh. */
static HTREEITEM find_loaded_item_for_identity(HTREEITEM item, const WCHAR *path,
                                               int isParentLink, int isRootChoice)
{
    if (!item)
        return NULL;
    ExplorerItemData *data = tree_item_data(item);
    if (data && data->isParentLink == isParentLink &&
        data->isRootChoice == isRootChoice && _wcsicmp(data->fullPath, path) == 0)
        return item;
    if (data && data->isDirectory && data->childrenLoaded)
    {
        for (HTREEITEM child = TreeView_GetChild(g_hExplorerTree, item); child;
             child = TreeView_GetNextSibling(g_hExplorerTree, child))
        {
            HTREEITEM found = find_loaded_item_for_identity(child, path, isParentLink,
                                                            isRootChoice);
            if (found)
                return found;
        }
    }
    return NULL;
}

static HTREEITEM find_item_for_identity(const WCHAR *path, int isParentLink, int isRootChoice)
{
    if (!path)
        return NULL;
    for (HTREEITEM top = TreeView_GetRoot(g_hExplorerTree); top;
         top = TreeView_GetNextSibling(g_hExplorerTree, top))
    {
        HTREEITEM found = find_loaded_item_for_identity(top, path, isParentLink,
                                                       isRootChoice);
        if (found)
            return found;
    }
    return NULL;
}

/* Rescans the folder at `path` if the tree is showing it with its children
   loaded; a folder that isn't loaded yet is scanned when it's expanded. */
static void reload_loaded_directory(const WCHAR *path)
{
    HTREEITEM item = find_loaded_item_for_path(s_rootItem, path);
    ExplorerItemData *data = item ? tree_item_data(item) : NULL;
    if (data && data->isDirectory && data->childrenLoaded)
        reload_directory_children(item, data);
}

/* ---- filesystem operations invoked from the context menu ---- */

static void copy_text_to_clipboard(const WCHAR *text)
{
    size_t bytes = (wcslen(text) + 1) * sizeof(WCHAR);
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!global)
        return;
    void *locked = GlobalLock(global);
    if (locked)
    {
        memcpy(locked, text, bytes);
        GlobalUnlock(global);
        if (OpenClipboard(s_hParent))
        {
            EmptyClipboard();
            HANDLE transferred = SetClipboardData(CF_UNICODETEXT, global);
            CloseClipboard();
            if (transferred)
                return; /* the clipboard now owns `global` */
        }
    }
    GlobalFree(global);
}

/* "C:\vault\sub\Note Name.md" -> "[[Note Name]]", matching how
   plugins/wikilinks.lua's note_name_from_path() names a note: the file name
   alone (no folder, no extension), since that's what it indexes and looks
   up by. Caller frees. */
static WCHAR *wikilink_text_for_path(const WCHAR *path)
{
    const WCHAR *lastBackslash = wcsrchr(path, L'\\');
    const WCHAR *lastSlash = wcsrchr(path, L'/');
    const WCHAR *lastSeparator = lastBackslash && lastSlash ? (lastBackslash > lastSlash ? lastBackslash : lastSlash)
                                                             : (lastBackslash ? lastBackslash : lastSlash);
    const WCHAR *nameStart = lastSeparator ? lastSeparator + 1 : path;
    size_t nameLen = wcslen(nameStart);
    const WCHAR *dot = wcsrchr(nameStart, L'.');
    size_t baseLen = (dot && dot != nameStart) ? (size_t)(dot - nameStart) : nameLen;

    WCHAR *result = (WCHAR *)malloc((baseLen + 5) * sizeof(WCHAR));
    if (!result)
        return NULL;
    result[0] = L'[';
    result[1] = L'[';
    memcpy(result + 2, nameStart, baseLen * sizeof(WCHAR));
    result[2 + baseLen] = L']';
    result[3 + baseLen] = L']';
    result[4 + baseLen] = L'\0';
    return result;
}

static void copy_wikilink_to_clipboard(const WCHAR *path)
{
    WCHAR *text = wikilink_text_for_path(path);
    if (!text)
        return;
    copy_text_to_clipboard(text);
    free(text);
}

/* Explorer itself uses IFileOperation, and this app already runs STA COM on
   the UI thread for WebView2 and already uses another modern shell COM
   interface the same way (IFileOpenDialog, in choose_workspace_folder()) -
   so this is no new idiom here. SHFileOperationW is a deprecated fallback
   for the rare case COM isn't available, so Delete still works (just
   without IFileOperation's richer per-item error reporting). */
static int delete_to_recycle_bin(const WCHAR *path)
{
    IFileOperation *operation = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOperation, NULL, CLSCTX_ALL,
                                  &IID_IFileOperation, (void **)&operation);
    if (SUCCEEDED(hr) && operation)
    {
        IFileOperation_SetOperationFlags(operation, FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT);
        IShellItem *item = NULL;
        hr = SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)&item);
        if (SUCCEEDED(hr) && item)
        {
            hr = IFileOperation_DeleteItem(operation, item, NULL);
            if (SUCCEEDED(hr))
                hr = IFileOperation_PerformOperations(operation);
            BOOL aborted = TRUE;
            if (SUCCEEDED(hr))
                hr = IFileOperation_GetAnyOperationsAborted(operation, &aborted);
            IShellItem_Release(item);
            IFileOperation_Release(operation);
            /* A cancelled operation must not be retried through the fallback. */
            return SUCCEEDED(hr) && !aborted;
        }
        IFileOperation_Release(operation);
        if (SUCCEEDED(hr))
            return 1;
    }
    WCHAR doubleNulPath[LONG_PATH_CAPACITY] = {0};
    size_t pathLength = wcslen(path);
    if (pathLength > sizeof(doubleNulPath) / sizeof(doubleNulPath[0]) - 2)
        return 0;
    memcpy(doubleNulPath, path, (pathLength + 1) * sizeof(WCHAR));
    SHFILEOPSTRUCTW op = {0};
    op.wFunc = FO_DELETE;
    op.pFrom = doubleNulPath;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

/* Rejects anything that isn't a single plain path component - in particular
   a path separator, which would otherwise let New File/New Folder/Rename
   silently create or move something outside the folder the user actually
   right-clicked (e.g. typing "..\..\evil" into the prompt). */
static int is_valid_entry_name(const WCHAR *name)
{
    if (!name[0] || wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
        return 0;
    size_t length = wcslen(name);
    if (name[length - 1] == L' ' || name[length - 1] == L'.' ||
        wcspbrk(name, L"\\/:*?\"<>|") != NULL)
        return 0;
    for (const WCHAR *p = name; *p; p++)
        if (*p < 32)
            return 0;

    /* Win32 reserves these device names even when they have an extension
       ("CON.txt", for example). Reject them up front instead of reporting a
       misleading create/rename failure after the prompt has closed. */
    size_t stemLength = wcscspn(name, L".");
    static const WCHAR *const reserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$", L"CLOCK$"
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
        if (stemLength == wcslen(reserved[i]) &&
            _wcsnicmp(name, reserved[i], stemLength) == 0)
            return 0;
    if (stemLength == 4 &&
        (_wcsnicmp(name, L"COM", 3) == 0 || _wcsnicmp(name, L"LPT", 3) == 0)) {
        WCHAR suffix = name[3];
        /* Windows also recognises the ISO-8859-1 superscript forms of 1,
           2 and 3 as digits in these device names. */
        if ((suffix >= L'1' && suffix <= L'9') || suffix == 0x00B9 ||
            suffix == 0x00B2 || suffix == 0x00B3)
            return 0;
    }
    return 1;
}

static void warn_invalid_entry_name(const WCHAR *title)
{
    MessageBoxW(s_hParent, L"That name isn't valid. Avoid reserved device names, control characters, "
                            L"\\ / : * ? \" < > |, a trailing space or period, and . or ..",
                title, MB_OK | MB_ICONWARNING);
}

static void create_new_entry(HTREEITEM containerItem, ExplorerItemData *containerData, int isDirectory)
{
    if (!s_promptText || !containerData)
        return;
    /* The prompt owns a modal message loop, so the watcher may refresh and
       free both arguments while it is open. Keep only a copied path across
       that boundary and resolve the item again afterwards. */
    WCHAR *containerPath = wcsdup_heap(containerData->fullPath);
    if (!containerPath)
        return;
    WCHAR name[260] = L"";
    const WCHAR *title = isDirectory ? L"New Folder" : L"New File";
    const WCHAR *prompt = isDirectory ? L"Folder name:" : L"File name:";
    if (!s_promptText(title, prompt, L"", name, sizeof(name) / sizeof(name[0])) || !name[0])
    {
        free(containerPath);
        return;
    }
    if (!is_valid_entry_name(name))
    {
        warn_invalid_entry_name(title);
        free(containerPath);
        return;
    }

    WCHAR *full = path_join(containerPath, name);
    if (!full)
    {
        free(containerPath);
        return;
    }
    int ok;
    if (isDirectory)
    {
        ok = CreateDirectoryW(full, NULL) != 0;
    }
    else
    {
        HANDLE file = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        ok = file != INVALID_HANDLE_VALUE;
        if (ok)
            CloseHandle(file);
    }
    if (!ok)
    {
        MessageBoxW(s_hParent, L"That name already exists or could not be created.", title,
                    MB_OK | MB_ICONWARNING);
        free(full);
        free(containerPath);
        return;
    }

    containerItem = find_item_for_path(containerPath);
    containerData = containerItem ? tree_item_data(containerItem) : NULL;
    if (containerData && containerData->childrenLoaded)
        reload_directory_children(containerItem, containerData);
    if (containerItem)
    {
        TreeView_Expand(g_hExplorerTree, containerItem, TVE_EXPAND);
        HTREEITEM created = find_item_for_path(full);
        if (created)
            TreeView_SelectItem(g_hExplorerTree, created);
    }
    free(full);
    free(containerPath);
}

static void rename_entry(HTREEITEM item, ExplorerItemData *data)
{
    if (!s_promptText || !data || item == s_rootItem)
        return;
    /* As with New, copy everything needed before showing the modal prompt;
       its message loop can run the live-refresh timer and invalidate data. */
    WCHAR *oldPath = wcsdup_heap(data->fullPath);
    WCHAR *directory = path_directory(data->fullPath);
    int isDirectory = data->isDirectory;
    if (!oldPath || !directory)
    {
        free(oldPath);
        free(directory);
        return;
    }
    WCHAR currentName[260];
    wcsncpy(currentName, PathFindFileNameW(oldPath), 259);
    currentName[259] = 0;
    WCHAR newName[260];
    wcsncpy(newName, currentName, 259);
    newName[259] = 0;
    if (!s_promptText(L"Rename", L"New name:", currentName, newName,
                      sizeof(newName) / sizeof(newName[0])) ||
        wcscmp(newName, currentName) == 0)
    {
        free(oldPath);
        free(directory);
        return;
    }
    if (!is_valid_entry_name(newName))
    {
        warn_invalid_entry_name(L"Rename");
        free(oldPath);
        free(directory);
        return;
    }

    WCHAR *newPath = directory ? path_join(directory, newName) : NULL;
    if (!newPath)
    {
        free(directory);
        free(oldPath);
        return;
    }

    if (!MoveFileW(oldPath, newPath))
    {
        MessageBoxW(s_hParent, L"Could not rename - that name may already be in use.",
                    L"Rename", MB_OK | MB_ICONWARNING);
    }
    else
    {
        if (s_renamedPath)
            s_renamedPath(oldPath, newPath, isDirectory);
        reload_loaded_directory(directory);
        HTREEITEM renamed = find_item_for_path(newPath);
        if (renamed)
            TreeView_SelectItem(g_hExplorerTree, renamed);
    }
    free(directory);
    free(oldPath);
    free(newPath);
}

static void delete_entry(HTREEITEM item, ExplorerItemData *data)
{
    if (item == s_rootItem)
        return;
    /* As in rename_entry(): the confirmation box's modal loop can let the
       watcher rebuild the tree, so only copied paths are used after it. */
    WCHAR *path = wcsdup_heap(data->fullPath);
    WCHAR *directory = path_directory(data->fullPath);
    if (!path || !directory)
    {
        free(path);
        free(directory);
        return;
    }
    WCHAR message[600];
    swprintf(message, 600, L"Move \"%ls\" to the Recycle Bin?", PathFindFileNameW(path));
    if (MessageBoxW(s_hParent, message, L"Delete", MB_YESNO | MB_ICONWARNING) == IDYES)
    {
        if (!delete_to_recycle_bin(path))
        {
            MessageBoxW(s_hParent, L"Could not delete that item.", L"Delete", MB_OK | MB_ICONERROR);
        }
        else
        {
            reload_loaded_directory(directory);
        }
    }
    free(path);
    free(directory);
}

/* ---- deferred root changes ----
   Changing the root deletes every tree item. Doing that from inside the
   tree's own NM_DBLCLK/TVN_KEYDOWN/NM_RCLICK notification would let comctl32
   carry on with its default handling against the item it already hit-tested
   and that item would then be freed. So the change is queued and run once
   the tree has finished handling the input. */

static void navigate_to_parent(void)
{
    if (!can_navigate_up())
        return;
    if (PathIsRootW(s_root))
    {
        show_roots_view();
        return;
    }
    WCHAR *parent = root_parent_path();
    if (!parent)
        return;
    s_changeRoot(parent);
    free(parent);
}

static void run_pending_navigation(void)
{
    WCHAR *target = s_pendingRootChoice;
    int toParent = s_pendingParentNavigation;
    s_pendingRootChoice = NULL;
    s_pendingParentNavigation = 0;
    if (target && s_changeRoot)
        s_changeRoot(target);
    else if (toParent)
        navigate_to_parent();
    free(target);
}

static void end_tree_drag(void)
{
    if (!s_dragging)
        return;
    s_dragging = 0;
    ImageList_DragLeave(NULL);
    ImageList_EndDrag();
    if (s_dragImageList)
    {
        ImageList_Destroy(s_dragImageList);
        s_dragImageList = NULL;
    }
    ShowCursor(TRUE);
    ReleaseCapture();
    free(s_dragPath);
    s_dragPath = NULL;
}

static void begin_tree_drag(NMTREEVIEWW *info)
{
    ExplorerItemData *data = (ExplorerItemData *)info->itemNew.lParam;
    if (!data || data->isDirectory || data->isParentLink || data->isRootChoice)
        return;
    TreeView_SelectItem(g_hExplorerTree, info->itemNew.hItem);
    s_dragImageList = TreeView_CreateDragImage(g_hExplorerTree, info->itemNew.hItem);
    if (!s_dragImageList)
        return;
    free(s_dragPath);
    s_dragPath = wcsdup_heap(data->fullPath);
    if (!s_dragPath)
    {
        ImageList_Destroy(s_dragImageList);
        s_dragImageList = NULL;
        return;
    }
    POINT screenPt = info->ptDrag;
    ClientToScreen(g_hExplorerTree, &screenPt);
    ImageList_BeginDrag(s_dragImageList, 0, 0, 0);
    ImageList_DragEnter(NULL, screenPt.x, screenPt.y);
    ShowCursor(FALSE);
    SetCapture(g_hExplorerTree);
    s_dragging = 1;
}

static LRESULT CALLBACK ExplorerTreeSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                                 LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    (void)id;
    (void)data;
    if (msg == EXPLORER_WM_NAVIGATE)
    {
        run_pending_navigation();
        return 0;
    }
    if (msg == WM_CONTEXTMENU && lParam == (LPARAM)-1)
    {
        handle_keyboard_context_menu();
        return 0;
    }
    if (s_dragging && msg == WM_MOUSEMOVE)
    {
        POINT screenPt;
        GetCursorPos(&screenPt);
        ImageList_DragMove(screenPt.x, screenPt.y);
        return 0;
    }
    if (s_dragging && msg == WM_LBUTTONUP)
    {
        POINT screenPt;
        GetCursorPos(&screenPt);
        WCHAR *path = s_dragPath;
        s_dragPath = NULL; /* claimed here so end_tree_drag() doesn't free it first */
        end_tree_drag();
        WCHAR *text = path ? wikilink_text_for_path(path) : NULL;
        if (text && s_dropText) s_dropText(text, screenPt);
        free(text);
        free(path);
        return 0;
    }
    if (s_dragging && (msg == WM_CAPTURECHANGED || (msg == WM_KEYDOWN && wParam == VK_ESCAPE)))
    {
        end_tree_drag();
        return 0;
    }
    if (msg == WM_NCDESTROY)
    {
        end_tree_drag();
        RemoveWindowSubclass(hwnd, ExplorerTreeSubclassProc, 1);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void queue_navigation(const WCHAR *rootChoice)
{
    free(s_pendingRootChoice);
    s_pendingRootChoice = rootChoice ? wcsdup_heap(rootChoice) : NULL;
    s_pendingParentNavigation = rootChoice == NULL;
    if (rootChoice && !s_pendingRootChoice)
        return;
    PostMessageW(g_hExplorerTree, EXPLORER_WM_NAVIGATE, 0, 0);
}

/* ---- WM_NOTIFY handling ---- */

static void open_parent_folder(void)
{
    if (can_navigate_up())
        queue_navigation(NULL);
}

static void handle_open_item(HTREEITEM item)
{
    ExplorerItemData *data = item ? tree_item_data(item) : NULL;
    if (!data)
        return;
    if (data->isParentLink)
        open_parent_folder();
    else if (data->isRootChoice && s_changeRoot)
        queue_navigation(data->fullPath);
    else if (data->isDirectory)
        TreeView_Expand(g_hExplorerTree, item, TVE_TOGGLE);
    else if (s_openFile)
    {
        /* Opening can show a modal error/large-file prompt. Pass a stable
           copy in case a watcher refresh frees the selected tree item. */
        WCHAR *path = wcsdup_heap(data->fullPath);
        if (path)
        {
            s_openFile(path);
            free(path);
        }
    }
}

static void handle_open_selected(void)
{
    handle_open_item(TreeView_GetSelection(g_hExplorerTree));
}

/* The tree item under the cursor position of the message being handled;
   also stores that position in screen coordinates, and/or the hit-test
   flags (TVHT_*), when asked. */
static HTREEITEM item_at_message_pos(POINT *screenOut, UINT *flagsOut)
{
    DWORD pos = GetMessagePos();
    POINT screen = {GET_X_LPARAM(pos), GET_Y_LPARAM(pos)};
    if (screenOut)
        *screenOut = screen;
    TVHITTESTINFO hit = {0};
    hit.pt = screen;
    ScreenToClient(g_hExplorerTree, &hit.pt);
    HTREEITEM item = TreeView_HitTest(g_hExplorerTree, &hit);
    if (flagsOut)
        *flagsOut = hit.flags;
    return item;
}

static void handle_click(void)
{
    UINT flags = 0;
    HTREEITEM item = item_at_message_pos(NULL, &flags);
    /* A click on the expand/collapse glyph itself already toggles the node
       through the TreeView's own default handling (TVS_HASBUTTONS), before
       this NM_CLICK notification is even sent; toggling it again here would
       immediately undo that, making the glyph appear to do nothing. Only a
       click elsewhere on a directory's row (its icon or label) needs this
       handler to toggle it. */
    if (flags & TVHT_ONITEMBUTTON)
        return;
    ExplorerItemData *data = item ? tree_item_data(item) : NULL;
    if (data && data->isDirectory && !data->isParentLink && !data->isRootChoice)
        TreeView_Expand(g_hExplorerTree, item, TVE_TOGGLE);
}

static void show_context_menu(HTREEITEM item, POINT screenPt)
{
    if (item)
        TreeView_SelectItem(g_hExplorerTree, item);
    ExplorerItemData *data = item ? tree_item_data(item) : NULL;
    int hadClickedItem = data != NULL;
    int isRoot = item && item == s_rootItem;
    int isParentLink = data && data->isParentLink;
    int isRootChoice = data && data->isRootChoice;
    /* TrackPopupMenu() runs its own modal loop below, which - like the
       MessageBoxW calls in rename_entry()/delete_entry() - still pumps this
       thread's WM_TIMER, so the watcher's debounced refresh can rebuild the
       tree (deleting and recreating items) while the menu is open. `item` is
       re-resolved by path afterwards rather than trusted to still name the
       same item; comctl32 can and does reuse a deleted item's HTREEITEM
       value for an unrelated one. */
    WCHAR *clickedPath = data ? wcsdup_heap(data->fullPath) : NULL;

    HMENU menu = CreatePopupMenu();
    if (can_navigate_up() && !isParentLink && !isRootChoice)
    {
        AppendMenuW(menu, MF_STRING, CTX_OPEN_PARENT, L"Open Parent Folder\tBackspace");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    }
    if (data)
    {
        const WCHAR *openLabel = isParentLink ? L"Open Parent Folder" : (isRootChoice ? L"Open as Workspace" : (data->isDirectory ? L"Expand/Collapse" : L"Open"));
        AppendMenuW(menu, MF_STRING, CTX_OPEN, openLabel);
        if (!isParentLink && !isRootChoice)
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    }
    if (!s_showingRoots && !isParentLink && !isRootChoice)
    {
        AppendMenuW(menu, MF_STRING, CTX_NEW_FILE, L"New File...");
        AppendMenuW(menu, MF_STRING, CTX_NEW_FOLDER, L"New Folder...");
    }
    if (data && !isRoot && !isParentLink && !isRootChoice)
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, CTX_RENAME, L"Rename...");
        AppendMenuW(menu, MF_STRING, CTX_DELETE, L"Delete");
    }
    if (data)
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, CTX_COPY_PATH, L"Copy Path");
        if (!data->isDirectory)
            AppendMenuW(menu, MF_STRING, CTX_COPY_WIKILINK, L"Copy as Wikilink");
        AppendMenuW(menu, MF_STRING, CTX_REVEAL, L"Reveal in Explorer");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CTX_REFRESH, L"Refresh");

    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  screenPt.x, screenPt.y, 0, s_hParent, NULL);
    DestroyMenu(menu);
    if (!command)
    {
        free(clickedPath);
        return;
    }
    item = clickedPath ? find_item_for_identity(clickedPath, isParentLink, isRootChoice) : NULL;
    data = item ? tree_item_data(item) : NULL;
    free(clickedPath);

    /* New File/New Folder/Refresh operate on the clicked directory, or the
       clicked file's parent, or the tree root if nothing was clicked. */
    HTREEITEM containerItem = item ? item : (hadClickedItem ? NULL : s_rootItem);
    ExplorerItemData *containerData = containerItem ? tree_item_data(containerItem) : NULL;
    if (containerData && (containerData->isParentLink || containerData->isRootChoice))
    {
        containerItem = s_rootItem;
        containerData = tree_item_data(containerItem);
    }
    if (containerData && !containerData->isDirectory)
    {
        containerItem = TreeView_GetParent(g_hExplorerTree, containerItem);
        containerData = containerItem ? tree_item_data(containerItem) : NULL;
    }

    switch (command)
    {
    case CTX_OPEN_PARENT:
        open_parent_folder();
        break;
    case CTX_OPEN:
        handle_open_item(item);
        break;
    case CTX_NEW_FILE:
        if (containerData)
            create_new_entry(containerItem, containerData, 0);
        break;
    case CTX_NEW_FOLDER:
        if (containerData)
            create_new_entry(containerItem, containerData, 1);
        break;
    case CTX_RENAME:
        if (data)
            rename_entry(item, data);
        break;
    case CTX_DELETE:
        if (data)
            delete_entry(item, data);
        break;
    case CTX_COPY_PATH:
        if (data)
            copy_text_to_clipboard(data->fullPath);
        break;
    case CTX_COPY_WIKILINK:
        if (data && !data->isDirectory)
            copy_wikilink_to_clipboard(data->fullPath);
        break;
    case CTX_REVEAL:
        if (data && s_revealPath)
            s_revealPath(data->fullPath);
        break;
    case CTX_REFRESH:
        if (s_showingRoots)
            show_roots_view();
        else if (containerData)
            reload_directory_children(containerItem, containerData);
        break;
    }
}

static void handle_mouse_context_menu(void)
{
    POINT screenPt;
    HTREEITEM item = item_at_message_pos(&screenPt, NULL);
    show_context_menu(item, screenPt);
}

static void handle_keyboard_context_menu(void)
{
    HTREEITEM item = TreeView_GetSelection(g_hExplorerTree);
    RECT itemRect = {0};
    POINT screenPt = {0};
    if (item && TreeView_GetItemRect(g_hExplorerTree, item, &itemRect, TRUE))
    {
        screenPt.x = itemRect.left;
        screenPt.y = itemRect.bottom;
    }
    else
    {
        RECT client;
        GetClientRect(g_hExplorerTree, &client);
        screenPt.x = client.left + 12;
        screenPt.y = client.top + 12;
    }
    ClientToScreen(g_hExplorerTree, &screenPt);
    show_context_menu(item, screenPt);
}

static void refresh_selected(void)
{
    if (s_showingRoots)
    {
        show_roots_view();
        return;
    }
    HTREEITEM item = TreeView_GetSelection(g_hExplorerTree);
    ExplorerItemData *data = item ? tree_item_data(item) : NULL;
    if (data && !data->isDirectory)
    {
        item = TreeView_GetParent(g_hExplorerTree, item);
        data = item ? tree_item_data(item) : NULL;
    }
    if (!data || data->isParentLink || data->isRootChoice)
    {
        item = s_rootItem;
        data = item ? tree_item_data(item) : NULL;
    }
    if (data && data->isDirectory)
        reload_directory_children(item, data);
}

int explorer_pretranslate_message(const MSG *message)
{
    if (!message || !g_hExplorerTree || GetFocus() != g_hExplorerTree ||
        (message->message != WM_KEYDOWN && message->message != WM_SYSKEYDOWN))
        return 0;

    int control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    int alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    HTREEITEM item = TreeView_GetSelection(g_hExplorerTree);
    ExplorerItemData *data = item ? tree_item_data(item) : NULL;

    if (!control && !shift && !alt && message->wParam == VK_F2)
    {
        if (data && item != s_rootItem && !data->isParentLink && !data->isRootChoice)
            rename_entry(item, data);
        return 1;
    }
    if (!control && !shift && !alt && message->wParam == VK_DELETE)
    {
        if (data && item != s_rootItem && !data->isParentLink && !data->isRootChoice)
            delete_entry(item, data);
        return 1;
    }
    if (!control && !shift && !alt && message->wParam == VK_F5)
    {
        refresh_selected();
        return 1;
    }
    if (control && !shift && !alt && message->wParam == 'C')
    {
        if (data)
            copy_text_to_clipboard(data->fullPath);
        return 1;
    }
    return 0;
}

static void handle_item_expanding(NMTREEVIEWW *info)
{
    if (info->action != TVE_EXPAND)
        return;
    ExplorerItemData *data = (ExplorerItemData *)info->itemNew.lParam;
    if (!data || !data->isDirectory || data->childrenLoaded)
        return;
    themed_scrollbar_begin_update(g_hExplorerTree);
    delete_all_children(info->itemNew.hItem);
    populate_directory_children(info->itemNew.hItem, data);
    themed_scrollbar_end_update(g_hExplorerTree);
}

static void handle_delete_item(NMTREEVIEWW *info)
{
    ExplorerItemData *data = (ExplorerItemData *)info->itemOld.lParam;
    if (data)
    {
        free(data->fullPath);
        free(data);
    }
}

void explorer_handle_notify(NMHDR *notification)
{
    switch (notification->code)
    {
    case TVN_ITEMEXPANDINGW:
        handle_item_expanding((NMTREEVIEWW *)notification);
        return;
    case TVN_DELETEITEMW:
        handle_delete_item((NMTREEVIEWW *)notification);
        return;
    case TVN_BEGINDRAGW:
        begin_tree_drag((NMTREEVIEWW *)notification);
        return;
    case NM_CLICK:
        handle_click();
        return;
    case NM_DBLCLK:
    {
        UINT flags = 0;
        HTREEITEM item = item_at_message_pos(NULL, &flags);
        ExplorerItemData *data = item ? tree_item_data(item) : NULL;
        /* Real folders already toggle on the first NM_CLICK in the double-
           click sequence; toggling again here would undo that action. */
        if (data && !(flags & TVHT_ONITEMBUTTON) &&
            (!data->isDirectory || data->isParentLink || data->isRootChoice))
            handle_open_item(item);
        return;
    }
    case TVN_KEYDOWN:
        if (((NMTVKEYDOWN *)notification)->wVKey == VK_RETURN)
            handle_open_selected();
        else if (((NMTVKEYDOWN *)notification)->wVKey == VK_BACK)
            open_parent_folder();
        return;
    case NM_RCLICK:
        handle_mouse_context_menu();
        return;
    }
}

/* ---- live file-system watcher ----
   One background thread per open root. It never touches the TreeView (or
   any other UI) directly - it only appends to the critical-section-guarded
   pending-directories queue and posts TINKEREDIT_WM_EXPLORER_CHANGED, which
   tinkeredit.c's WndProc debounces before calling explorer_handle_change_message()
   back on the UI thread. This is the app's first background thread, so its
   shutdown path is deliberately conservative: CancelIoEx before joining, so
   a still-pending overlapped read can never leave the thread stuck in
   GetOverlappedResult and the join in explorer_stop_watch() hanging. */

static void queue_changed_directory(const WCHAR *directory)
{
    EnterCriticalSection(&s_pendingLock);
    int exists = 0;
    for (int i = 0; i < s_pendingCount; i++)
    {
        if (_wcsicmp(s_pendingDirs[i], directory) == 0)
        {
            exists = 1;
            break;
        }
    }
    if (!exists)
    {
        if (s_pendingCount >= s_pendingCapacity)
        {
            int nextCapacity = s_pendingCapacity ? s_pendingCapacity * 2 : 8;
            WCHAR **grown = (WCHAR **)realloc(s_pendingDirs, (size_t)nextCapacity * sizeof(WCHAR *));
            if (grown)
            {
                s_pendingDirs = grown;
                s_pendingCapacity = nextCapacity;
            }
        }
        if (s_pendingCount < s_pendingCapacity)
        {
            WCHAR *copy = wcsdup_heap(directory);
            if (copy)
                s_pendingDirs[s_pendingCount++] = copy;
        }
    }
    LeaveCriticalSection(&s_pendingLock);
    if (s_hParent)
        PostMessageW(s_hParent, TINKEREDIT_WM_EXPLORER_CHANGED, 0, 0);
}

static unsigned __stdcall explorer_watch_thread_proc(void *param)
{
    ExplorerWatcher *watcher = (ExplorerWatcher *)param;
    for (;;)
    {
        DWORD bytesReturned = 0;
        BOOL queued = ReadDirectoryChangesW(watcher->dirHandle, watcher->buffer, sizeof(watcher->buffer),
                                            TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
                                            NULL, &watcher->overlapped, NULL);
        if (!queued && GetLastError() != ERROR_IO_PENDING)
            break;

        HANDLE waitHandles[2] = {watcher->overlapped.hEvent, watcher->stopEvent};
        DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (wait != WAIT_OBJECT_0)
        {
            CancelIoEx(watcher->dirHandle, &watcher->overlapped);
            GetOverlappedResult(watcher->dirHandle, &watcher->overlapped, &bytesReturned, TRUE);
            break;
        }
        if (!GetOverlappedResult(watcher->dirHandle, &watcher->overlapped, &bytesReturned, FALSE))
            continue;
        if (bytesReturned == 0)
        {
            /* The notification buffer overflowed - some changes were
               dropped. Fall back to refreshing from the root. */
            queue_changed_directory(watcher->root);
            continue;
        }

        BYTE *cursor = watcher->buffer;
        for (;;)
        {
            FILE_NOTIFY_INFORMATION *info = (FILE_NOTIFY_INFORMATION *)cursor;
            int nameLength = (int)(info->FileNameLength / sizeof(WCHAR));
            WCHAR *name = (WCHAR *)malloc(((size_t)nameLength + 1) * sizeof(WCHAR));
            if (name)
            {
                memcpy(name, info->FileName, (size_t)nameLength * sizeof(WCHAR));
                name[nameLength] = 0;
                /* Filter relative to the watched root. Checking the absolute
                   directory would suppress every event when the workspace
                   itself lived below an otherwise-skipped name such as
                   C:\\build or C:\\work\\node_modules\\project. */
                WCHAR *relativeDirectory = path_directory(name);
                WCHAR *full = path_join(watcher->root, name);
                int skipped = relativeDirectory && path_has_skipped_component(relativeDirectory);
                free(relativeDirectory);
                free(name);
                if (full)
                {
                    WCHAR *directory = path_directory(full);
                    free(full);
                    if (directory)
                    {
                        /* Checks every path component, not just the immediate
                           parent - git itself writes deep inside .git\objects\
                           and .git\refs\, whose own immediate parent is never
                           literally ".git". */
                        if (!skipped)
                            queue_changed_directory(directory);
                        free(directory);
                    }
                }
            }
            if (info->NextEntryOffset == 0)
                break;
            cursor += info->NextEntryOffset;
        }
    }
    return 0;
}

/* Closes whichever of a watcher's handles were opened and frees it. */
static void free_watcher(ExplorerWatcher *watcher)
{
    if (watcher->thread)
        CloseHandle(watcher->thread);
    if (watcher->dirHandle)
        CloseHandle(watcher->dirHandle);
    if (watcher->overlapped.hEvent)
        CloseHandle(watcher->overlapped.hEvent);
    if (watcher->stopEvent)
        CloseHandle(watcher->stopEvent);
    free(watcher->root);
    free(watcher);
}

static void start_watcher(const WCHAR *root)
{
    ExplorerWatcher *watcher = (ExplorerWatcher *)calloc(1, sizeof(ExplorerWatcher));
    if (!watcher)
        return;
    watcher->root = wcsdup_heap(root);
    if (!watcher->root)
    {
        free_watcher(watcher);
        return;
    }
    watcher->dirHandle = CreateFileW(root, FILE_LIST_DIRECTORY,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    if (watcher->dirHandle == INVALID_HANDLE_VALUE)
    {
        watcher->dirHandle = NULL;
        free_watcher(watcher);
        return;
    }
    watcher->overlapped.hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    watcher->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!watcher->overlapped.hEvent || !watcher->stopEvent)
    {
        free_watcher(watcher);
        return;
    }
    watcher->thread = (HANDLE)_beginthreadex(NULL, 0, explorer_watch_thread_proc, watcher, 0, NULL);
    if (!watcher->thread)
    {
        free_watcher(watcher);
        return;
    }
    s_watcher = watcher;
}

void explorer_stop_watch(void)
{
    if (!s_watcher)
        return;
    ExplorerWatcher *watcher = s_watcher;
    s_watcher = NULL;
    SetEvent(watcher->stopEvent);
    WaitForSingleObject(watcher->thread, INFINITE);
    free_watcher(watcher);
}

void explorer_handle_change_message(void)
{
    EnterCriticalSection(&s_pendingLock);
    WCHAR **dirs = s_pendingDirs;
    int count = s_pendingCount;
    s_pendingDirs = NULL;
    s_pendingCount = 0;
    s_pendingCapacity = 0;
    LeaveCriticalSection(&s_pendingLock);

    themed_scrollbar_begin_update(g_hExplorerTree);
    for (int i = 0; i < count; i++)
    {
        reload_loaded_directory(dirs[i]);
        free(dirs[i]);
    }
    free(dirs);
    themed_scrollbar_end_update(g_hExplorerTree);
}

/* ---- public API ---- */

void explorer_init(HWND hParent, ExplorerOpenFileFn openFile, ExplorerPromptTextFn promptText,
                   ExplorerRevealFn revealPath, ExplorerRenameFn renamedPath,
                   ExplorerChangeRootFn changeRoot, ExplorerDropTextFn dropText)
{
    s_hParent = hParent;
    s_openFile = openFile;
    s_promptText = promptText;
    s_revealPath = revealPath;
    s_renamedPath = renamedPath;
    s_changeRoot = changeRoot;
    s_dropText = dropText;
    InitializeCriticalSection(&s_pendingLock);

    g_hExplorerTree = CreateWindowExW(0, WC_TREEVIEWW, L"",
                                      WS_CHILD | WS_VSCROLL | TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT |
                                          TVS_SHOWSELALWAYS,
                                      0, 0, 0, 0, hParent, (HMENU)(UINT_PTR)ID_EXPLORER_TREE, GetModuleHandleW(NULL), NULL);
    if (!g_hExplorerTree)
        return;

    /* Buffer the complete tree into an off-screen surface before presenting
       it. Without this, scrolling exposes and erases intermediate rows before
       their icons/text are repainted, which is especially visible beside the
       independently painted themed scrollbar overlays. */
    TreeView_SetExtendedStyle(g_hExplorerTree, TVS_EX_DOUBLEBUFFER,
                              TVS_EX_DOUBLEBUFFER);

    /* ILC_COLOR32 alone (no ILC_MASK) stores icons with no legacy 1-bit mask
       at all, so ImageList_Draw has no choice but to alpha-blend the real
       per-pixel alpha channel against whatever's already painted underneath -
       this is what actually gives correct transparency on any background.
       Adding ILC_MASK here (an earlier attempt) forced hard mask-based
       compositing instead, which happened to look fine against the near-
       black dark theme but showed as a wrong-coloured box in light mode. */
    int iconSize = GetSystemMetrics(SM_CXSMICON);
    s_iconList = ImageList_Create(iconSize, iconSize, ILC_COLOR32, 8, 32);
    if (s_iconList)
        TreeView_SetImageList(g_hExplorerTree, s_iconList, TVSIL_NORMAL);

    TreeView_SetItemHeight(g_hExplorerTree, MulDiv(18, GetDpiForWindow(hParent), 96));
    SetWindowSubclass(g_hExplorerTree, ExplorerTreeSubclassProc, 1, 0);
    themed_scrollbar_attach(g_hExplorerTree, THEMED_SCROLL_TREEVIEW);

    g_hExplorerClose = CreateWindowExW(0, L"BUTTON", L"X",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, hParent, (HMENU)(UINT_PTR)IDM_VIEW_EXPLORER, GetModuleHandleW(NULL), NULL);
    SendMessageW(g_hExplorerClose, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

    explorer_apply_theme();
}

void explorer_apply_theme(void)
{
    if (!g_hExplorerTree)
        return;
    apply_explorer_theme(g_hExplorerTree);
    TreeView_SetBkColor(g_hExplorerTree, theme_bg_color());
    TreeView_SetTextColor(g_hExplorerTree, theme_fg_color());
    themed_scrollbar_refresh(g_hExplorerTree);
    InvalidateRect(g_hExplorerTree, NULL, TRUE);
    if (g_hExplorerClose) InvalidateRect(g_hExplorerClose, NULL, TRUE);
}

void explorer_set_root(const WCHAR *rootPath)
{
    if (!g_hExplorerTree || !rootPath)
        return;
    /* Re-opening the folder already being shown (e.g. toggling the panel off
       then back on) would otherwise discard every node's expand state and
       redo the same disk scan + watcher restart for no reason - the watcher
       already keeps what's shown in step with the disk. */
    if (s_root && s_watcher && _wcsicmp(s_root, rootPath) == 0)
        return;
    WCHAR *newRoot = wcsdup_heap(rootPath);
    if (!newRoot)
        return;
    explorer_stop_watch();
    themed_scrollbar_begin_update(g_hExplorerTree);
    TreeView_DeleteAllItems(g_hExplorerTree); /* frees every ExplorerItemData via TVN_DELETEITEMW */
    s_rootItem = NULL;
    s_showingRoots = 0;
    free(s_root);
    s_root = newRoot;

    const WCHAR *leaf = PathFindFileNameW(s_root);
    s_rootItem = insert_tree_item(TVI_ROOT, TVI_LAST, s_root, *leaf ? leaf : s_root, 1);
    if (s_rootItem)
        TreeView_Expand(g_hExplorerTree, s_rootItem, TVE_EXPAND);
    themed_scrollbar_end_update(g_hExplorerTree);
    start_watcher(s_root);
}

/* explorer_content_x() now lives in layout_metrics.c. */

void explorer_shutdown(void)
{
    explorer_stop_watch();
    EnterCriticalSection(&s_pendingLock);
    for (int i = 0; i < s_pendingCount; i++)
        free(s_pendingDirs[i]);
    free(s_pendingDirs);
    s_pendingDirs = NULL;
    s_pendingCount = 0;
    s_pendingCapacity = 0;
    LeaveCriticalSection(&s_pendingLock);
    DeleteCriticalSection(&s_pendingLock);
    free(s_pendingRootChoice);
    s_pendingRootChoice = NULL;
    s_pendingParentNavigation = 0;
    free(s_root);
    s_root = NULL;
    s_rootItem = NULL;
    if (s_iconList)
    {
        ImageList_Destroy(s_iconList);
        s_iconList = NULL;
    }
    g_hExplorerTree = NULL;
}
