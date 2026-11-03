/* ---- folder and workspace tools ---------------------------------------- */

typedef struct WorkspaceFiles {
    WCHAR **paths;
    WCHAR **labels;
    int *lines;
    int count;
    int capacity;
    int truncated; /* scan incomplete: a limit, allocation, or I/O failure */
    WCHAR *root;
} WorkspaceFiles;

static void free_workspace_files(WorkspaceFiles *files) {
    for (int i = 0; i < files->count; i++) {
        free(files->paths[i]);
        free(files->labels[i]);
    }
    free(files->paths); free(files->labels); free(files->lines); free(files->root);
    memset(files, 0, sizeof(*files));
}

static int add_workspace_item(WorkspaceFiles *files, const WCHAR *path,
                              const WCHAR *label, int line) {
    if (files->count >= files->capacity) {
        int next = files->capacity ? files->capacity * 2 : 128;
        WCHAR **paths = (WCHAR **)realloc(files->paths, (size_t)next * sizeof(WCHAR *));
        if (!paths) return 0;
        files->paths = paths;
        WCHAR **labels = (WCHAR **)realloc(files->labels, (size_t)next * sizeof(WCHAR *));
        if (!labels) return 0;
        files->labels = labels;
        int *lines = (int *)realloc(files->lines, (size_t)next * sizeof(int));
        if (!lines) return 0;
        files->lines = lines;
        files->capacity = next;
    }
    files->paths[files->count] = wcsdup_heap(path);
    files->labels[files->count] = wcsdup_heap(label);
    files->lines[files->count] = line;
    if (!files->paths[files->count] || !files->labels[files->count]) {
        free(files->paths[files->count]); free(files->labels[files->count]);
        return 0;
    }
    files->count++;
    return 1;
}

static WCHAR *choose_workspace_folder(void) {
    IFileOpenDialog *dialog = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IFileOpenDialog, (void **)&dialog)) || !dialog) return NULL;
    FILEOPENDIALOGOPTIONS options = 0;
    IFileOpenDialog_GetOptions(dialog, &options);
    IFileOpenDialog_SetOptions(dialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                               FOS_PATHMUSTEXIST);
    WCHAR *resultPath = NULL;
    if (SUCCEEDED(IFileOpenDialog_Show(dialog, g_hMainWnd))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(dialog, &item)) && item) {
            PWSTR selected = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &selected)) && selected) {
                resultPath = wcsdup_heap(selected);
                CoTaskMemFree(selected);
            }
            IShellItem_Release(item);
        }
    }
    IFileOpenDialog_Release(dialog);
    return resultPath;
}

/* Reuses the same LANGUAGES table language_for_path() draws from, rather
   than maintaining a second, separate extension list here that would
   otherwise silently drift and hide files the app can actually open and
   highlight (.lua, .cs, .java, .go, .rs, .php, etc.) from the workspace
   tree/quick-open/find-in-folder. */
static int workspace_file_extension(const WCHAR *name) {
    return language_for_known_path(name) != NULL;
}

/* Scans stop (and report themselves as truncated) past these limits, so
   a workspace rooted at a huge tree stays responsive. */
enum {
    WORKSPACE_MAX_FILES = 5000,
    WORKSPACE_MAX_DEPTH = 24,
    WORKSPACE_SCAN_MAX_FILE_BYTES = 4 * 1024 * 1024
};

static void enumerate_workspace_files(WorkspaceFiles *files, const WCHAR *directory, int depth) {
    if (depth > WORKSPACE_MAX_DEPTH || files->count >= WORKSPACE_MAX_FILES) { files->truncated = 1; return; }
    WCHAR *pattern = path_join(directory, L"*");
    if (!pattern) { files->truncated = 1; return; }
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(pattern, &data);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_NO_MORE_FILES)
            files->truncated = 1;
        return;
    }
    int stoppedEarly = 0;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        WCHAR *full = path_join(directory, data.cFileName);
        if (!full) { files->truncated = 1; continue; }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                !skipped_workspace_directory(data.cFileName))
                enumerate_workspace_files(files, full, depth + 1);
        } else if (!(data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) &&
                   workspace_file_extension(data.cFileName)) {
            const WCHAR *relative = full;
            size_t rootLength = wcslen(files->root);
            if (_wcsnicmp(full, files->root, rootLength) == 0) {
                relative = full + rootLength;
                if (*relative == L'\\' || *relative == L'/') relative++;
            }
            if (!add_workspace_item(files, full, relative, 0)) {
                files->truncated = 1;
                stoppedEarly = 1;
            }
        }
        free(full);
        if (stoppedEarly) break;
    } while (files->count < WORKSPACE_MAX_FILES && FindNextFileW(search, &data));
    if (files->count >= WORKSPACE_MAX_FILES) files->truncated = 1;
    else if (!stoppedEarly && GetLastError() != ERROR_NO_MORE_FILES)
        files->truncated = 1;
    FindClose(search);
}

static void warn_incomplete_workspace_scan(const WCHAR *title, int enumerationTruncated,
                                           int skippedLarge, int readFailed,
                                           int resultsTruncated) {
    if (!enumerationTruncated && !skippedLarge && !readFailed && !resultsTruncated) return;
    WCHAR message[640];
    int used = swprintf(message, sizeof(message) / sizeof(message[0]),
                        L"Some workspace content was not searched:");
    if (enumerationTruncated && used > 0)
        used += swprintf(message + used, sizeof(message) / sizeof(message[0]) - (size_t)used,
                         L"\n\u2022 The workspace scan was incomplete (a folder could not be read, "
                         L"or the 5,000-file/24-folder-depth limit was reached).");
    if (skippedLarge && used > 0)
        used += swprintf(message + used, sizeof(message) / sizeof(message[0]) - (size_t)used,
                         L"\n\u2022 %d file%ls larger than 4 MB %ls skipped.", skippedLarge,
                         skippedLarge == 1 ? L"" : L"s", skippedLarge == 1 ? L"was" : L"were");
    if (readFailed && used > 0)
        used += swprintf(message + used, sizeof(message) / sizeof(message[0]) - (size_t)used,
                         L"\n\u2022 %d file%ls could not be read.", readFailed,
                         readFailed == 1 ? L"" : L"s");
    if (resultsTruncated && used > 0)
        swprintf(message + used, sizeof(message) / sizeof(message[0]) - (size_t)used,
                 L"\n\u2022 Some matches could not be collected, or only the first 2,000 are shown.");
    MessageBoxW(g_hMainWnd, message, title, MB_OK | MB_ICONWARNING);
}

static int collect_workspace_files_at(WorkspaceFiles *files, const WCHAR *root) {
    memset(files, 0, sizeof(*files));
    files->root = wcsdup_heap(root);
    if (!files->root) return 0;
    enumerate_workspace_files(files, files->root, 0);
    return 1;
}

/* ---- workspace Markdown index ----------------------------------------- */

/* File names are enough for completion and wiki-link resolution. The cache
   is short-lived as well as explicitly invalidated, so it remains useful
   when the explorer panel (and therefore its live watcher) is hidden. */
static WorkspaceFiles g_markdownWorkspaceFiles;
static ULONGLONG g_markdownWorkspaceScanAt;
static int g_markdownWorkspaceIndexPinned;

static int markdown_file_path(const WCHAR *path) {
    const LanguageInfo *language = language_for_known_path(path);
    return language && language->id == LANG_MARKDOWN;
}

static WCHAR *active_markdown_workspace_root(void) {
    if (g_currentWorkspaceFolder) return wcsdup_heap(g_currentWorkspaceFolder);
    return g_currentFile ? path_directory(g_currentFile) : NULL;
}

static void invalidate_markdown_workspace_cache(void) {
    free_workspace_files(&g_markdownWorkspaceFiles);
    g_markdownWorkspaceScanAt = 0;
}

static int collect_markdown_workspace_files(WorkspaceFiles *markdown, const WCHAR *root) {
    WorkspaceFiles all;
    if (!root || !collect_workspace_files_at(&all, root)) return 0;
    memset(markdown, 0, sizeof(*markdown));
    markdown->root = wcsdup_heap(all.root);
    markdown->truncated = all.truncated;
    if (!markdown->root) {
        free_workspace_files(&all);
        return 0;
    }
    for (int i = 0; i < all.count; i++) {
        if (markdown_file_path(all.paths[i]) &&
            !add_workspace_item(markdown, all.paths[i], all.labels[i], 0)) {
            markdown->truncated = 1;
            break;
        }
    }
    free_workspace_files(&all);
    return 1;
}

static WorkspaceFiles *markdown_workspace_files(int forceRefresh) {
    WCHAR *root = active_markdown_workspace_root();
    if (!root) return NULL;
    ULONGLONG now = GetTickCount64();
    int differentRoot = !g_markdownWorkspaceFiles.root ||
                        _wcsicmp(g_markdownWorkspaceFiles.root, root) != 0;
    int expired = !g_markdownWorkspaceScanAt || now - g_markdownWorkspaceScanAt > 5000;
    if (forceRefresh || differentRoot || (expired && !g_markdownWorkspaceIndexPinned)) {
        invalidate_markdown_workspace_cache();
        if (!collect_markdown_workspace_files(&g_markdownWorkspaceFiles, root)) {
            free(root);
            return NULL;
        }
        g_markdownWorkspaceScanAt = now;
    }
    free(root);
    return &g_markdownWorkspaceFiles;
}

static WCHAR *without_markdown_extension(const WCHAR *value) {
    WCHAR *copy = wcsdup_heap(value);
    if (!copy) return NULL;
    WCHAR *extension = PathFindExtensionW(copy);
    if (extension && markdown_file_path(copy)) *extension = 0;
    return copy;
}

static int path_separator_present(const WCHAR *value) {
    return wcschr(value, L'\\') != NULL || wcschr(value, L'/') != NULL;
}

/* Resolves local wiki links first, then workspace-relative paths, then a
   unique workspace-wide page name. With chooseAmbiguous, duplicate page
   names are offered in the existing searchable choice dialog. */
static WCHAR *resolve_workspace_wikilink(const WCHAR *name, int chooseAmbiguous,
                                         int *ambiguous) {
    if (ambiguous) *ambiguous = 0;
    if (!name || !*name) return NULL;
    WCHAR *target = wcsdup_heap(name);
    if (!target) return NULL;
    WCHAR *fragment = wcschr(target, L'#');
    if (fragment) *fragment = 0;
    WCHAR *query = wcschr(target, L'?');
    if (query) *query = 0;
    for (WCHAR *p = target; *p; p++) if (*p == L'/') *p = L'\\';
    if (!*target || path_escapes_base(target)) { free(target); return NULL; }

    WCHAR *withExtension = target;
    if (!*PathFindExtensionW(target)) {
        size_t length = wcslen(target);
        withExtension = (WCHAR *)malloc((length + 4) * sizeof(WCHAR));
        if (!withExtension) { free(target); return NULL; }
        memcpy(withExtension, target, length * sizeof(WCHAR));
        memcpy(withExtension + length, L".md", 4 * sizeof(WCHAR));
    }

    WCHAR *documentDirectory = g_currentFile ? path_directory(g_currentFile) : NULL;
    WCHAR *joined = documentDirectory ? path_join(documentDirectory, withExtension) : NULL;
    WCHAR *local = joined ? absolute_path(joined) : NULL;
    free(joined);
    free(documentDirectory);
    if (file_exists(local)) {
        if (withExtension != target) free(withExtension);
        free(target);
        return local;
    }
    free(local);

    WorkspaceFiles *files = markdown_workspace_files(0);
    if (!files) {
        if (withExtension != target) free(withExtension);
        free(target);
        return NULL;
    }
    joined = path_join(files->root, withExtension);
    WCHAR *fromRoot = joined ? absolute_path(joined) : NULL;
    free(joined);
    if (file_exists(fromRoot)) {
        if (withExtension != target) free(withExtension);
        free(target);
        return fromRoot;
    }
    free(fromRoot);

    WCHAR *needle = without_markdown_extension(target);
    int hasPath = path_separator_present(target);
    int matches[WORKSPACE_MAX_FILES];
    int matchCount = 0;
    for (int i = 0; needle && i < files->count; i++) {
        WCHAR *candidate = without_markdown_extension(files->labels[i]);
        const WCHAR *comparison = candidate
            ? (hasPath ? candidate : PathFindFileNameW(candidate)) : NULL;
        if (comparison && _wcsicmp(comparison, needle) == 0 && matchCount < WORKSPACE_MAX_FILES)
            matches[matchCount++] = i;
        free(candidate);
    }
    free(needle);
    if (withExtension != target) free(withExtension);
    free(target);
    if (matchCount == 1) return wcsdup_heap(files->paths[matches[0]]);
    if (matchCount > 1) {
        if (ambiguous) *ambiguous = 1;
        if (chooseAmbiguous) {
            WCHAR **labels = (WCHAR **)malloc((size_t)matchCount * sizeof(WCHAR *));
            if (!labels) return NULL;
            for (int i = 0; i < matchCount; i++) labels[i] = files->labels[matches[i]];
            int selected = show_choice_dialog(L"Open Wiki Link",
                                              L"More than one page has that name:",
                                              labels, matchCount, L"Open");
            free(labels);
            if (selected >= 0 && selected < matchCount)
                return wcsdup_heap(files->paths[matches[selected]]);
        }
    }
    return NULL;
}

/* Returns nonzero whenever the caret is in an unfinished [[target, even if
   there are no candidates, so ordinary prose-word completion stays out of
   the link target. */
static int update_wikilink_completions(HWND hEdit) {
    if (g_activeLanguage != LANG_MARKDOWN) return 0;
    Sci_Position pos = (Sci_Position)SendMessageA(hEdit, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position line = (Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)pos, 0);
    Sci_Position start = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
    char *before = get_text_range(hEdit, start, pos);
    if (!before) return 0;
    char *open = NULL;
    for (char *p = before; p[0] && p[1]; p++) {
        if (p[0] == '[' && p[1] == '[') { open = p + 2; p++; }
        else if (p[0] == ']' && p[1] == ']') { open = NULL; p++; }
    }
    if (!open || strchr(open, '|') || strchr(open, '\n') || strchr(open, '\r')) {
        free(before);
        return 0;
    }
    size_t prefixLength = strlen(open);
    WorkspaceFiles *files = markdown_workspace_files(0);
    char *candidates[WORD_COMPLETION_MAX_WORDS];
    int count = 0;
    for (int i = 0; files && i < files->count && count < WORD_COMPLETION_MAX_WORDS; i++) {
        const WCHAR *label = PathFindFileNameW(files->labels[i]);
        WCHAR *without = without_markdown_extension(label);
        char *candidate = without ? to_utf8(without) : NULL;
        free(without);
        if (!candidate) continue;
        for (char *p = candidate; *p; p++) if (*p == '\\') *p = '/';
        const char *match = utf8_case_find(candidate, open);
        if (match != candidate) { free(candidate); continue; }
        int duplicate = 0;
        for (int c = 0; c < count; c++)
            if (_stricmp(candidates[c], candidate) == 0) { duplicate = 1; break; }
        if (duplicate) free(candidate);
        else candidates[count++] = candidate;
    }
    free(before);
    if (!count) {
        SendMessageA(hEdit, SCI_AUTOCCANCEL, 0, 0);
        return 1;
    }
    qsort(candidates, (size_t)count, sizeof(char *), compare_words_ordinal);
    size_t total = 1;
    for (int i = 0; i < count; i++) total += strlen(candidates[i]) + 1;
    char *list = (char *)malloc(total);
    if (list) {
        size_t used = 0;
        for (int i = 0; i < count; i++) {
            size_t length = strlen(candidates[i]);
            memcpy(list + used, candidates[i], length);
            used += length;
            if (i + 1 < count) list[used++] = '\n';
        }
        list[used] = 0;
        SendMessageA(hEdit, SCI_AUTOCSHOW, (WPARAM)prefixLength, (LPARAM)list);
        theme_autocomplete_popup();
        free(list);
    }
    for (int i = 0; i < count; i++) free(candidates[i]);
    return 1;
}

/* ---- workspace link diagnostics and backlinks ------------------------ */

/* Defined with the workspace replacement helpers below. A declaration here
   lets link diagnostics share their open-buffer-aware reader. */
typedef struct WorkspaceText {
    char *text;
    size_t length;
    int openIndex; /* -1 when read from disk */
    int encoding;
    int hasBom;
} WorkspaceText;
static int load_workspace_text(const WCHAR *path, WorkspaceText *out);

typedef struct MarkdownLinkReport {
    char *text;
    size_t length;
    size_t capacity;
    int count;
} MarkdownLinkReport;

static int append_markdown_link_report(MarkdownLinkReport *report, const WCHAR *label,
                                       int line, const char *message, const char *target) {
    char *file = to_utf8(label);
    if (!file) return 0;
    int needed = snprintf(NULL, 0, "%s:%d: %s%s%s\n", file, line, message,
                          target ? " \"" : "", target ? target : "");
    if (target) needed++; /* closing quote */
    if (needed < 0) { free(file); return 0; }
    size_t required = report->length + (size_t)needed + 1;
    if (required > report->capacity) {
        size_t next = report->capacity ? report->capacity * 2 : 1024;
        while (next < required) next *= 2;
        char *grown = (char *)realloc(report->text, next);
        if (!grown) { free(file); return 0; }
        report->text = grown;
        report->capacity = next;
    }
    int written;
    if (target)
        written = snprintf(report->text + report->length, report->capacity - report->length,
                           "%s:%d: %s \"%s\"\n", file, line, message, target);
    else
        written = snprintf(report->text + report->length, report->capacity - report->length,
                           "%s:%d: %s\n", file, line, message);
    free(file);
    if (written < 0) return 0;
    report->length += (size_t)written;
    report->count++;
    return 1;
}

static char *copy_markdown_link_target(const char *target, size_t length) {
    char *copy = (char *)malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, target, length);
    copy[length] = 0;
    return copy;
}

static int markdown_link_is_external(const char *target) {
    if (!target || !*target || target[0] == '#') return 0;
    if (target[0] == '/' || target[0] == '\\') return 1;
    const char *colon = strchr(target, ':');
    const char *slash = strpbrk(target, "/\\");
    return colon && (!slash || colon < slash);
}

/* Separates and percent-decodes a link's path and fragment. The returned
   path is absolute for ordinary Markdown links and workspace-resolved for
   wiki links. */
static WCHAR *resolved_markdown_link_path(const WCHAR *sourcePath,
                                          MarkdownLinkKind kind, const char *raw,
                                          char **fragment, int *ambiguous) {
    if (fragment) *fragment = NULL;
    if (ambiguous) *ambiguous = 0;
    if (!raw || markdown_link_is_external(raw)) return NULL;
    char *copy = _strdup(raw);
    if (!copy) return NULL;
    char *hash = strchr(copy, '#');
    if (hash) {
        *hash++ = 0;
        char *questionInFragment = strchr(hash, '?');
        if (questionInFragment) *questionInFragment = 0;
        WCHAR *wideFragment = to_wide(hash);
        WCHAR *decodedFragment = wideFragment
            ? percent_decode_wide(wideFragment, wideFragment + wcslen(wideFragment)) : NULL;
        if (fragment && decodedFragment) *fragment = to_utf8(decodedFragment);
        free(decodedFragment);
        free(wideFragment);
    }
    char *query = strchr(copy, '?');
    if (query) *query = 0;
    WCHAR *encoded = to_wide(copy);
    free(copy);
    if (!encoded) return NULL;
    WCHAR *decoded = *encoded
        ? percent_decode_wide(encoded, encoded + wcslen(encoded)) : wcsdup_heap(L"");
    free(encoded);
    if (!decoded) return NULL;
    for (WCHAR *p = decoded; *p; p++) if (*p == L'/') *p = L'\\';
    if (kind == MARKDOWN_LINK_WIKI) {
        WCHAR *resolved = resolve_workspace_wikilink(decoded, 0, ambiguous);
        free(decoded);
        return resolved;
    }
    if (!*decoded) {
        free(decoded);
        return absolute_path(sourcePath);
    }
    WCHAR *directory = path_directory(sourcePath);
    WCHAR *joined = directory ? path_join(directory, decoded) : NULL;
    WCHAR *resolved = joined ? absolute_path(joined) : NULL;
    free(joined);
    free(directory);
    free(decoded);
    return resolved;
}

static int filesystem_path_exists(const WCHAR *path) {
    return path && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static int markdown_anchor_at_path(const WCHAR *path, const char *fragment) {
    if (!path || !fragment || !*fragment || !markdown_file_path(path)) return 1;
    WorkspaceText document;
    int loaded = load_workspace_text(path, &document);
    int exists = loaded > 0 && markdown_heading_anchor_exists(document.text, fragment);
    free(document.text);
    return exists;
}

typedef struct MarkdownLinkScan {
    const WCHAR *sourcePath;
    const WCHAR *sourceLabel;
    const WCHAR *backlinkTarget;
    MarkdownLinkReport *report;
} MarkdownLinkScan;

static void inspect_workspace_link(MarkdownLinkKind kind, int line,
                                   const char *target, size_t targetLength,
                                   void *context) {
    MarkdownLinkScan *scan = (MarkdownLinkScan *)context;
    char *raw = copy_markdown_link_target(target, targetLength);
    if (!raw || markdown_link_is_external(raw)) { free(raw); return; }
    char *fragment = NULL;
    int ambiguous = 0;
    WCHAR *resolved = resolved_markdown_link_path(scan->sourcePath, kind, raw,
                                                  &fragment, &ambiguous);
    if (scan->backlinkTarget) {
        if (resolved && _wcsicmp(resolved, scan->backlinkTarget) == 0 &&
            _wcsicmp(scan->sourcePath, scan->backlinkTarget) != 0)
            append_markdown_link_report(scan->report, scan->sourceLabel, line,
                                        "Links to the active document.", NULL);
        free(fragment); free(resolved); free(raw);
        return;
    }
    if (ambiguous) {
        append_markdown_link_report(scan->report, scan->sourceLabel, line,
                                    "Ambiguous wiki link:", raw);
    } else if (!filesystem_path_exists(resolved)) {
        const char *message = kind == MARKDOWN_LINK_IMAGE ? "Missing image:"
                              : kind == MARKDOWN_LINK_WIKI ? "Missing wiki page:"
                              : "Missing linked file:";
        append_markdown_link_report(scan->report, scan->sourceLabel, line, message, raw);
    } else if (fragment && *fragment && !markdown_anchor_at_path(resolved, fragment)) {
        append_markdown_link_report(scan->report, scan->sourceLabel, line,
                                    "Missing heading anchor:", fragment);
    }
    free(fragment);
    free(resolved);
    free(raw);
}

static int workspace_markdown_file_too_large(const WCHAR *path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    return GetFileAttributesExW(path, GetFileExInfoStandard, &attributes) &&
           (attributes.nFileSizeHigh || attributes.nFileSizeLow > WORKSPACE_SCAN_MAX_FILE_BYTES);
}

static void check_workspace_markdown_links(void) {
    WorkspaceFiles *files = markdown_workspace_files(1);
    if (!files) {
        MessageBoxW(g_hMainWnd, L"Open a workspace or save the document before checking links.",
                    L"Check Workspace Links", MB_OK | MB_ICONINFORMATION);
        return;
    }
    MarkdownLinkReport report = {0};
    int unreadable = 0, skippedLarge = 0;
    g_markdownWorkspaceIndexPinned++;
    for (int i = 0; i < files->count; i++) {
        if (workspace_markdown_file_too_large(files->paths[i])) { skippedLarge++; continue; }
        WorkspaceText document;
        if (load_workspace_text(files->paths[i], &document) <= 0) { unreadable++; continue; }
        MarkdownLinkScan scan = { files->paths[i], files->labels[i], NULL, &report };
        markdown_links_visit(document.text, inspect_workspace_link, &scan);
        free(document.text);
    }
    g_markdownWorkspaceIndexPinned--;
    set_problem_report("Workspace Links", report.text, files->root);
    if (report.count) {
        show_output_panel(OUTPUT_TAB_PROBLEMS, 1);
        WCHAR status[96];
        swprintf(status, 96, L"%d workspace link problem%ls found", report.count,
                 report.count == 1 ? L"" : L"s");
        show_plugin_reload_status(status);
    } else {
        show_plugin_reload_status(L"Every workspace Markdown link resolves");
    }
    warn_incomplete_workspace_scan(L"Check Workspace Links", files->truncated,
                                   skippedLarge, unreadable, 0);
    free(report.text);
}

static void show_markdown_backlinks(void) {
    if (!g_currentFile || !markdown_file_path(g_currentFile)) return;
    WorkspaceFiles *files = markdown_workspace_files(1);
    if (!files) return;
    WCHAR *target = absolute_path(g_currentFile);
    if (!target) return;
    MarkdownLinkReport report = {0};
    int unreadable = 0, skippedLarge = 0;
    g_markdownWorkspaceIndexPinned++;
    for (int i = 0; i < files->count; i++) {
        if (_wcsicmp(files->paths[i], target) == 0) continue;
        if (workspace_markdown_file_too_large(files->paths[i])) { skippedLarge++; continue; }
        WorkspaceText document;
        if (load_workspace_text(files->paths[i], &document) <= 0) { unreadable++; continue; }
        MarkdownLinkScan scan = { files->paths[i], files->labels[i], target, &report };
        markdown_links_visit(document.text, inspect_workspace_link, &scan);
        free(document.text);
    }
    g_markdownWorkspaceIndexPinned--;
    if (!report.text) {
        char *name = to_utf8(PathFindFileNameW(target));
        size_t capacity = (name ? strlen(name) : 0) + 48;
        report.text = (char *)malloc(capacity);
        if (report.text)
            snprintf(report.text, capacity, "No backlinks found for %s.\n", name ? name : "this document");
        free(name);
    }
    set_problem_report("Markdown Backlinks", report.text, files->root);
    show_output_panel(OUTPUT_TAB_PROBLEMS, 1);
    warn_incomplete_workspace_scan(L"Find Backlinks", files->truncated,
                                   skippedLarge, unreadable, 0);
    free(report.text);
    free(target);
}

static int collect_workspace_files(WorkspaceFiles *files) {
    WCHAR *root = choose_workspace_folder();
    if (!root) { memset(files, 0, sizeof(*files)); return 0; }
    int ok = collect_workspace_files_at(files, root);
    free(root);
    return ok;
}

static void do_quick_open_in(const WCHAR *root) {
    WorkspaceFiles files;
    if (!collect_workspace_files_at(&files, root)) return;
    if (files.truncated)
        warn_incomplete_workspace_scan(L"Quick Open", 1, 0, 0, 0);
    if (!files.count) {
        MessageBoxW(g_hMainWnd, L"No supported text files were found in that folder.",
                    L"Quick Open", MB_OK | MB_ICONINFORMATION);
        free_workspace_files(&files);
        return;
    }
    int selected = show_choice_dialog(L"Quick Open", L"Type part of a file name:",
                                      files.labels, files.count, L"Open");
    if (selected >= 0 && selected < files.count) open_file_in_tab(files.paths[selected]);
    free_workspace_files(&files);
}

static void do_quick_open(void) {
    WCHAR *root = choose_workspace_folder();
    if (!root) return;
    do_quick_open_in(root);
    free(root);
}

/* Records `root` as the current workspace folder: persists it, moves it to
   the front of Recent Workspaces, refreshes the menu, and - if the explorer
   panel is open - points it at the new folder. Shared by every place that
   adopts a workspace folder so they can't drift out of sync with each other
   (do_open_recent_workspace's own "folder no longer exists" early-out is
   deliberately not part of this, since only that one caller needs it). */
static void adopt_workspace_folder(HWND hwnd, const WCHAR *root) {
    invalidate_markdown_workspace_cache();
    replace_wstring(&g_currentWorkspaceFolder, root);
    SetCurrentDirectoryW(root);
    recent_list_add(&g_recentFolderList, root);
    save_workspace_settings();
    refresh_menu(hwnd);
    if (g_explorerVisible) explorer_set_root(g_currentWorkspaceFolder);
    refresh_git_status();
    fire_plugin_event("workspace_changed", g_currentWorkspaceFolder);
}

/* Opens a workspace folder - the folder is remembered so Recent Workspaces
   can return to it without browsing for it again. */
static void do_open_workspace(void) {
    WCHAR *root = choose_workspace_folder();
    if (!root) return;
    adopt_workspace_folder(g_hMainWnd, root);
    free(root);
}

static void do_open_recent_workspace(int index) {
    if (index < 0 || index >= g_recentFolderCount) return;
    WCHAR *root = wcsdup_heap(g_recentFolders[index]);
    if (!root) return;
    DWORD attrs = GetFileAttributesW(root);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(g_hMainWnd, L"That folder could not be found.", L"Tinker Editor",
                    MB_OK | MB_ICONINFORMATION);
        recent_list_remove(&g_recentFolderList, root);
        refresh_menu(g_hMainWnd);
        free(root);
        return;
    }
    adopt_workspace_folder(g_hMainWnd, root);
    free(root);
}

/* Mirrors toggle_outline()/toggle_preview(): flip the visibility flag, then
   menu/layout/repaint/persist. load_workspace_settings() supplies C:\ as the
   initial root, while Open Workspace remains available for choosing another
   folder. */
static void toggle_explorer_panel(HWND hwnd) {
    g_explorerVisible = !g_explorerVisible;
    if (g_explorerVisible) {
        if (g_currentWorkspaceFolder) {
            explorer_set_root(g_currentWorkspaceFolder);
        } else {
            adopt_workspace_folder(hwnd, L"C:\\"); /* also sets the explorer root */
        }
    } else {
        explorer_stop_watch();
    }
    relayout_and_save(hwnd);
}

#define WORKSPACE_SCAN_MAX_MATCHES 2000

/* Stores one match found by scan_workspace_matches(). `label` (the
   "file:line — snippet" text) is the callback's to keep or free. Returns 0
   when the match couldn't be stored. */
typedef int (*WorkspaceMatchFn)(void *context, const WCHAR *path, int lineNumber, WCHAR *label);

enum { WORKSPACE_SNIPPET_BYTES = 180 };

/* "relative/path:12 — text of the line", the snippet cut at
   WORKSPACE_SNIPPET_BYTES on a UTF-8 character boundary. */
static WCHAR *workspace_match_label(const WCHAR *fileLabel, int lineNumber, const char *line) {
    char snippet[WORKSPACE_SNIPPET_BYTES + 1];
    size_t snippetLength = 0;
    while (snippetLength < WORKSPACE_SNIPPET_BYTES && line[snippetLength]) snippetLength++;
    while (snippetLength && ((unsigned char)line[snippetLength] & 0xC0) == 0x80)
        snippetLength--;
    memcpy(snippet, line, snippetLength);
    snippet[snippetLength] = 0;
    WCHAR *wideSnippet = to_wide(snippet);
    size_t capacity = wcslen(fileLabel) + (wideSnippet ? wcslen(wideSnippet) : 0) + 48;
    WCHAR *label = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (label)
        swprintf(label, capacity, L"%ls:%d — %ls", fileLabel, lineNumber,
                 wideSnippet ? wideSnippet : L"");
    free(wideSnippet);
    return label;
}

/* The line-by-line, case-insensitive scan shared by Find in Folder and
   Replace in Folder. Files over 4 MB are skipped and counted; at most
   WORKSPACE_SCAN_MAX_MATCHES matches are passed to `store`. Returns nonzero
   when matches were left out (the cap was reached or one couldn't be stored). */
static int scan_workspace_matches(const WorkspaceFiles *files, const char *query,
                                  WorkspaceMatchFn store, void *context,
                                  int *skippedLarge, int *readFailed) {
    int matchCount = 0, truncated = 0;
    *skippedLarge = 0;
    *readFailed = 0;
    for (int i = 0; i < files->count && matchCount < WORKSPACE_SCAN_MAX_MATCHES; i++) {
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (!GetFileAttributesExW(files->paths[i], GetFileExInfoStandard, &attributes)) {
            (*readFailed)++;
            continue;
        }
        if (attributes.nFileSizeHigh || attributes.nFileSizeLow > WORKSPACE_SCAN_MAX_FILE_BYTES) {
            (*skippedLarge)++;
            continue;
        }
        char *content = read_text_file(files->paths[i], NULL, NULL, NULL);
        if (!content) { (*readFailed)++; continue; }
        char *cursor = content;
        int lineNumber = 1;
        while (*cursor && matchCount < WORKSPACE_SCAN_MAX_MATCHES) {
            size_t terminatorLength = 0;
            char *end = text_line_break(cursor, &terminatorLength);
            char saved = *end;
            *end = 0;
            if (utf8_case_find(cursor, query)) {
                WCHAR *label = workspace_match_label(files->labels[i], lineNumber, cursor);
                if (label && store(context, files->paths[i], lineNumber, label)) matchCount++;
                else truncated = 1;
            }
            *end = saved;
            if (!terminatorLength) break;
            cursor = end + terminatorLength;
            lineNumber++;
        }
        free(content);
    }
    return truncated || matchCount >= WORKSPACE_SCAN_MAX_MATCHES;
}

static int store_find_match(void *context, const WCHAR *path, int lineNumber, WCHAR *label) {
    int stored = add_workspace_item((WorkspaceFiles *)context, path, label, lineNumber - 1);
    free(label);
    return stored;
}

/* Writes every match to the Output tab, one "relative/path:line — snippet"
   line each - already the exact shape open_output_location() parses - so the
   full result set stays double-click-able after the quick picker below is
   closed, for working through many hits instead of just the one just opened.
   root is the workspace folder the labels' paths are relative to. */
static void report_find_in_folder_results(const WorkspaceFiles *results, const WCHAR *root,
                                          const WCHAR *queryWide) {
    if (!g_hOutputPanel) return;
    OutputPane *pane = &g_outputPanes[OUTPUT_TAB_RUN];
    end_output_program(pane);
    clear_output_pane(pane);
    free(pane->directory);
    pane->directory = root ? _wcsdup(root) : NULL;
    WCHAR note[320];
    swprintf(note, sizeof(note) / sizeof(note[0]),
            L"Find in Folder: \"%ls\" — %d match%ls (double-click a line to open it)",
            queryWide, results->count, results->count == 1 ? L"" : L"es");
    append_output_note(pane, OUTPUT_STYLE_NOTE, note);
    for (int i = 0; i < results->count; i++) {
        char *utf8 = to_utf8(results->labels[i]);
        if (!utf8) continue;
        append_output(pane, OUTPUT_STYLE_DEFAULT, utf8, strlen(utf8));
        append_output(pane, OUTPUT_STYLE_DEFAULT, "\n", 1);
        free(utf8);
    }
    show_output_panel(OUTPUT_TAB_RUN, 0);
}

static void do_find_in_folder(void) {
    WorkspaceFiles files;
    if (!collect_workspace_files(&files)) return;
    WCHAR queryWide[256];
    if (!show_text_input(L"Find in Folder", L"Text to find (case-insensitive):", L"",
                         queryWide, sizeof(queryWide) / sizeof(queryWide[0])) || !queryWide[0]) {
        free_workspace_files(&files); return;
    }
    char *query = to_utf8(queryWide);
    if (!query) { free_workspace_files(&files); return; }
    WorkspaceFiles results = {0};
    int skippedLarge = 0, readFailed = 0;
    int resultsTruncated = scan_workspace_matches(&files, query, store_find_match, &results,
                                                  &skippedLarge, &readFailed);
    int enumerationTruncated = files.truncated;
    WCHAR *root = wcsdup_heap(files.root);
    free(query);
    free_workspace_files(&files);
    warn_incomplete_workspace_scan(L"Find in Folder", enumerationTruncated, skippedLarge,
                                   readFailed, resultsTruncated);
    if (!results.count) {
        MessageBoxW(g_hMainWnd, L"No matches were found.", L"Find in Folder",
                    MB_OK | MB_ICONINFORMATION);
        free_workspace_files(&results); free(root); return;
    }
    report_find_in_folder_results(&results, root, queryWide);
    free(root);
    int selected = show_choice_dialog(L"Find in Folder", L"Filter results, then open a match:",
                                      results.labels, results.count, L"Open");
    if (selected >= 0 && selected < results.count && open_file_in_tab(results.paths[selected])) {
        SendMessageA(g_hEdit, SCI_GOTOLINE, (WPARAM)results.lines[selected], 0);
        SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
    }
    free_workspace_files(&results);
}

/* Finds the open tab (if any) showing `path`, the same case-insensitive
   comparison open_file_in_tab() uses to detect an already-open document. */
static int find_open_document_for_path(const WCHAR *path) {
    for (int i = 0; i < g_documentStore.count; i++) {
        if (g_documentStore.documents[i].path && _wcsicmp(g_documentStore.documents[i].path, path) == 0) return i;
    }
    return -1;
}

/* A workspace file's text for a folder-wide edit: the open tab's buffer
   (unsaved edits included) when it's open, otherwise the file on disk. */
/* Returns 1 with `out` filled, 0 when the text couldn't be read, or -1 when
   it contains a NUL (possible in UTF-16): line-based rewrites walk
   NUL-terminated text and would write such a file back cut short. */
static int load_workspace_text(const WCHAR *path, WorkspaceText *out) {
    memset(out, 0, sizeof(*out));
    out->openIndex = find_open_document_for_path(path);
    out->encoding = TEXT_ENCODING_UTF8;
    if (out->openIndex >= 0) {
        Sci_Position length = 0;
        out->text = get_document_text(out->openIndex, &length);
        out->length = (size_t)length;
        out->encoding = g_documentStore.documents[out->openIndex].encoding;
        out->hasBom = g_documentStore.documents[out->openIndex].hasBom;
    } else {
        int length = 0;
        out->text = read_text_file(path, &length, &out->encoding, &out->hasBom);
        out->length = (size_t)length;
    }
    if (!out->text) return 0;
    if (strlen(out->text) != out->length) {
        free(out->text);
        out->text = NULL;
        return -1;
    }
    return 1;
}

/* Writes rewritten text back in the file's own encoding. Returns 1 when
   written, 0 when the write failed, or -1 when the encoding can't hold the
   new text - the file is then left alone rather than gaining '?'/U+FFFD. */
static int save_workspace_text(const WCHAR *path, const char *text, size_t length,
                               int encoding, int hasBom) {
    if (text_encoding_is_lossless(text, length, (TextEncoding)encoding) != 1) return -1;
    WCHAR *ioPath = extended_path(path);
    int ok = write_file_atomic(ioPath ? ioPath : path, text, length, (TextEncoding)encoding, hasBom);
    free(ioPath);
    return ok ? 1 : 0;
}

/* Silently replaces a clean (non-dirty) document's buffer with freshly
   written content - used after a workspace replace changes a file that's
   open in a tab but has no unsaved edits to lose, so the tab reflects the
   new content instead of going stale. Swaps the shared editor's document
   pointer the same transient way get_document_text() does, so this works
   for a background tab without visibly switching to it. */
static void reload_document_buffer(int index, const char *content) {
    if (!g_hEdit || index < 0 || index >= g_documentStore.count) return;
    BackgroundDocumentAccess access;
    begin_background_document_access(&access);
    if (access.document != g_documentStore.documents[index].sciDocument)
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documentStore.documents[index].sciDocument);
    set_edit_text(g_hEdit, content, (Sci_Position)strlen(content));
    reset_undo_history(g_hEdit);
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    end_background_document_access(&access);
    g_documentStore.documents[index].dirty = 0;
    forget_loaded_file_end(&g_documentStore.documents[index]);
    record_document_file_time(index);
    update_tab_label(index);
    update_document_mapping();
    if (index == g_documentStore.active) {
        update_preview();
        update_title();
        update_outline();
    }
}

/* Returns a newly allocated copy of `line` with every case-insensitive
   occurrence of `find` replaced by `replace`, or NULL if `find` doesn't
   occur (or is empty - callers must not pass one, since StrStrIA would
   "find" it at every position and loop forever). */
static char *replace_all_in_line(const char *line, const char *find, const char *replace) {
    size_t matchLen = 0;
    const char *hit = find[0] ? utf8_case_find_length(line, find, &matchLen) : NULL;
    if (!hit || !matchLen) return NULL;
    size_t replaceLen = strlen(replace);
    size_t cap = strlen(line) + replaceLen + 64;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t outLen = 0;
    const char *cursor = line;
    /* Advance by the bytes actually matched: a linguistic match can be
       longer or shorter than `find` itself. */
    do {
        size_t prefixLen = (size_t)(hit - cursor);
        size_t needed = outLen + prefixLen + replaceLen + 1;
        if (needed > cap) {
            cap = needed * 2;
            char *grown = (char *)realloc(out, cap);
            if (!grown) { free(out); return NULL; }
            out = grown;
        }
        memcpy(out + outLen, cursor, prefixLen); outLen += prefixLen;
        memcpy(out + outLen, replace, replaceLen); outLen += replaceLen;
        cursor = hit + matchLen;
        hit = utf8_case_find_length(cursor, find, &matchLen);
    } while (hit && matchLen);
    size_t restLen = strlen(cursor);
    size_t needed = outLen + restLen + 1;
    if (needed > cap) {
        char *grown = (char *)realloc(out, needed);
        if (!grown) { free(out); return NULL; }
        out = grown;
    }
    memcpy(out + outLen, cursor, restLen); outLen += restLen;
    out[outLen] = 0;
    return out;
}

/* How a Replace in Folder run went, for the summary at the end. */
typedef struct ReplaceTally {
    int changed;  /* files written */
    int failed;   /* files that couldn't be read or written */
    int skipped;  /* files left alone: NUL bytes, or text the encoding can't store */
} ReplaceTally;

/* `text` with the replacement applied to each line that has an accepted
   match among matches[first..last), keeping every line ending as it was.
   `text` is modified while scanning but restored. Returns a heap string
   with *outLength set and *changed nonzero if any line changed, or NULL
   when out of memory. */
static char *replace_accepted_lines(char *text, size_t length, const ReplaceMatch *matches,
                                    int first, int last, const char *find, const char *replace,
                                    size_t *outLength, int *changed) {
    size_t outCapacity = length + 256;
    char *out = (char *)malloc(outCapacity);
    if (!out) return NULL;
    size_t used = 0;
    *changed = 0;
    char *cursor = text;
    int lineNumber = 1;
    int nextMatch = first;
    while (*cursor) {
        size_t terminatorLength = 0;
        char *end = text_line_break(cursor, &terminatorLength);
        char terminator[2] = {0, 0};
        if (terminatorLength) memcpy(terminator, end, terminatorLength);
        *end = 0;
        int accept = 0;
        while (nextMatch < last && matches[nextMatch].line < lineNumber) nextMatch++;
        while (nextMatch < last && matches[nextMatch].line == lineNumber) {
            if (matches[nextMatch].accepted) accept = 1;
            nextMatch++;
        }
        char *lineOut = accept ? replace_all_in_line(cursor, find, replace) : NULL;
        const char *piece = lineOut ? lineOut : cursor;
        size_t pieceLen = strlen(piece);
        if (lineOut) *changed = 1;
        size_t needed = used + pieceLen + terminatorLength + 1;
        if (needed > outCapacity) {
            outCapacity = needed * 2;
            char *grown = (char *)realloc(out, outCapacity);
            if (!grown) { free(lineOut); free(out); memcpy(end, terminator, terminatorLength); return NULL; }
            out = grown;
        }
        memcpy(out + used, piece, pieceLen);
        used += pieceLen;
        free(lineOut);
        if (!terminatorLength) break;
        memcpy(out + used, terminator, terminatorLength);
        used += terminatorLength;
        memcpy(end, terminator, terminatorLength);
        cursor = end + terminatorLength;
        lineNumber++;
    }
    out[used] = 0;
    *outLength = used;
    return out;
}

/* Applies the accepted matches[first..last), which all belong to one file,
   and writes the file back. */
static void replace_in_file(const ReplaceMatch *matches, int first, int last,
                            const char *find, const char *replace, ReplaceTally *tally) {
    const WCHAR *path = matches[first].path;
    WorkspaceText source;
    int loaded = load_workspace_text(path, &source);
    if (loaded <= 0) {
        if (loaded < 0) tally->skipped++;
        else tally->failed++;
        return;
    }
    size_t outLength = 0;
    int changed = 0;
    char *out = replace_accepted_lines(source.text, source.length, matches, first, last,
                                       find, replace, &outLength, &changed);
    if (out && changed) {
        int saved = save_workspace_text(path, out, outLength, source.encoding, source.hasBom);
        if (saved < 0) {
            tally->skipped++;
        } else if (saved) {
            tally->changed++;
            /* `out` was built from the live editor buffer (including any
               other unsaved edits), so it's exactly what was just written
               to disk regardless of the tab's prior dirty state - syncing
               the buffer to it (and clearing dirty) is correct either way.
               Leaving the buffer holding the pre-replacement text would let
               the next Ctrl+S silently overwrite this file again, undoing
               the replacement that had just been written. */
            if (source.openIndex >= 0) reload_document_buffer(source.openIndex, out);
        } else {
            tally->failed++;
        }
    }
    free(out);
    free(source.text);
}

/* One summary for the whole run rather than one message per failed file,
   which otherwise forced dismissing a modal for every read-only or locked
   file before a large batch replace could finish. */
static void report_workspace_replace(const ReplaceTally *tally) {
    WCHAR message[512];
    const size_t messageCap = sizeof(message) / sizeof(message[0]);
    if (tally->changed == 0 && tally->failed == 0 && tally->skipped == 0) {
        swprintf(message, messageCap, L"No files were changed.");
    } else {
        int used = swprintf(message, messageCap, L"Updated %d file%ls.",
                            tally->changed, tally->changed == 1 ? L"" : L"s");
        if (tally->failed && used > 0)
            used += swprintf(message + used, messageCap - (size_t)used,
                             L" %d file%ls could not be written and %ls skipped.",
                             tally->failed, tally->failed == 1 ? L"" : L"s",
                             tally->failed == 1 ? L"was" : L"were");
        if (tally->skipped && used > 0)
            swprintf(message + used, messageCap - (size_t)used,
                     L" %d file%ls left unchanged (a NUL character in the file, or a "
                     L"replacement its encoding can't store).",
                     tally->skipped, tally->skipped == 1 ? L" was" : L"s were");
    }
    MessageBoxW(g_hMainWnd, message, L"Replace in Folder",
               MB_OK | (tally->failed || tally->skipped ? MB_ICONWARNING : MB_ICONINFORMATION));
}

/* Applies the checked matches: one file at a time, replaces every
   occurrence of `find` with `replace` on each accepted line only (matching
   the line-level granularity the review checklist offers), then writes the
   result with write_file_atomic(). `matches` must be sorted by path, which
   the scan in do_replace_in_workspace() below already produces. */
static void apply_workspace_replace(ReplaceMatch *matches, int count, const char *find, const char *replace) {
    ReplaceTally tally = {0};
    int first = 0;
    while (first < count) {
        /* Matches are sorted by path, so each file's are one run. */
        int last = first;
        while (last < count && _wcsicmp(matches[last].path, matches[first].path) == 0) last++;
        int anyAccepted = 0;
        for (int j = first; j < last; j++) if (matches[j].accepted) { anyAccepted = 1; break; }
        if (anyAccepted) replace_in_file(matches, first, last, find, replace, &tally);
        first = last;
    }
    report_workspace_replace(&tally);
}

/* Same folder enumeration and line scan as do_find_in_folder(), but every
   match is collected up front (as a ReplaceMatch, sorted by path since the
   file loop produces them that way) and reviewed with checkboxes before
   anything is written - see show_replace_review_dialog(). */
typedef struct ReplaceMatchList {
    ReplaceMatch *matches;
    int count;
    int capacity;
} ReplaceMatchList;

static int store_replace_match(void *context, const WCHAR *path, int lineNumber, WCHAR *label) {
    ReplaceMatchList *list = (ReplaceMatchList *)context;
    WCHAR *matchPath = wcsdup_heap(path);
    if (matchPath && list->count >= list->capacity) {
        int next = list->capacity ? list->capacity * 2 : 128;
        ReplaceMatch *grown = (ReplaceMatch *)realloc(list->matches, (size_t)next * sizeof(ReplaceMatch));
        if (grown) {
            list->matches = grown;
            list->capacity = next;
        }
    }
    if (!matchPath || list->count >= list->capacity) {
        free(matchPath);
        free(label);
        return 0;
    }
    list->matches[list->count++] = (ReplaceMatch){ matchPath, label, lineNumber, 1 };
    return 1;
}

static void do_replace_in_workspace(void) {
    WorkspaceFiles files;
    if (!collect_workspace_files(&files)) return;

    WCHAR findWide[256];
    if (!show_text_input(L"Replace in Folder", L"Text to find (case-insensitive):", L"",
                         findWide, sizeof(findWide) / sizeof(findWide[0])) || !findWide[0]) {
        free_workspace_files(&files); return;
    }
    WCHAR replaceWide[256];
    if (!show_text_input(L"Replace in Folder", L"Replace with:", L"",
                         replaceWide, sizeof(replaceWide) / sizeof(replaceWide[0]))) {
        free_workspace_files(&files); return;
    }
    char *find = to_utf8(findWide);
    char *replace = to_utf8(replaceWide);
    if (!find || !replace) { free(find); free(replace); free_workspace_files(&files); return; }

    ReplaceMatchList found = {0};
    int skippedLarge = 0, readFailed = 0;
    int resultsTruncated = scan_workspace_matches(&files, find, store_replace_match, &found,
                                                  &skippedLarge, &readFailed);
    ReplaceMatch *matches = found.matches;
    int matchCount = found.count;
    int enumerationTruncated = files.truncated;
    free_workspace_files(&files);
    warn_incomplete_workspace_scan(L"Replace in Folder", enumerationTruncated, skippedLarge,
                                   readFailed, resultsTruncated);

    if (matchCount == 0) {
        MessageBoxW(g_hMainWnd, L"No matches were found in that folder.", L"Replace in Folder",
                    MB_OK | MB_ICONINFORMATION);
        free(find); free(replace);
        return;
    }

    if (show_replace_review_dialog(matches, matchCount))
        apply_workspace_replace(matches, matchCount, find, replace);

    for (int i = 0; i < matchCount; i++) { free(matches[i].path); free(matches[i].label); }
    free(matches);
    free(find);
    free(replace);
}

static int path_at_or_below(const WCHAR *path, const WCHAR *parent) {
    size_t length = path && parent ? wcslen(parent) : 0;
    if (!length || _wcsnicmp(path, parent, length) != 0) return 0;
    return path[length] == 0 || path[length] == L'\\' || path[length] == L'/';
}

static char *relative_markdown_target(const WCHAR *sourcePath, const WCHAR *targetPath,
                                      int targetIsDirectory) {
    WCHAR relative[MAX_PATH];
    if (!relative_path_from_file(sourcePath, targetPath, targetIsDirectory, relative)) return NULL;
    WCHAR *link = markdown_link_path(relative);
    char *target = link ? to_utf8(link) : NULL;
    free(link);
    return target;
}

/* Rewrites the links in `text` (the Markdown file at sourcePath) that point
   at oldPath so they point at newPath. For a renamed folder, links to files
   inside it are rewritten too. */
static char *rewrite_markdown_targets(const char *text, const char *oldTarget,
                                      const char *newTarget, int isDirectory,
                                      int *changes) {
    return isDirectory
        ? markdown_rewrite_link_directory(text, oldTarget, newTarget, changes)
        : markdown_rewrite_link_targets(text, oldTarget, newTarget, changes);
}

static char *rewrite_links_for_source(const char *text, const WCHAR *sourcePath,
                                      const WCHAR *oldPath, const WCHAR *newPath,
                                      int isDirectory, int *changes) {
    *changes = 0;
    char *oldTarget = relative_markdown_target(sourcePath, oldPath, isDirectory);
    char *newTarget = relative_markdown_target(sourcePath, newPath, isDirectory);
    if (!oldTarget || !newTarget) {
        free(oldTarget); free(newTarget);
        *changes = -1;
        return NULL;
    }
    char *rewritten = rewrite_markdown_targets(
        text, oldTarget, newTarget, isDirectory, changes);
    size_t oldLength = strlen(oldTarget), newLength = strlen(newTarget);
    if (oldTarget[0] != '.' && newTarget[0] != '.' &&
        oldLength <= SIZE_MAX - 3 && newLength <= SIZE_MAX - 3) {
        char *dotOld = (char *)malloc(oldLength + 3);
        char *dotNew = (char *)malloc(newLength + 3);
        if (dotOld && dotNew) {
            snprintf(dotOld, oldLength + 3, "./%s", oldTarget);
            snprintf(dotNew, newLength + 3, "./%s", newTarget);
            int dotChanges = 0;
            char *withDots = rewrite_markdown_targets(
                rewritten ? rewritten : text, dotOld, dotNew, isDirectory, &dotChanges);
            if (withDots) { free(rewritten); rewritten = withDots; }
            *changes += dotChanges;
        }
        free(dotOld); free(dotNew);
    }
    free(oldTarget); free(newTarget);
    return rewritten;
}

static void replace_open_document_text(int index, const char *text) {
    if (index < 0 || index >= g_documentStore.count || !text) return;
    BackgroundDocumentAccess access;
    begin_background_document_access(&access);
    if (access.document != g_documentStore.documents[index].sciDocument)
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documentStore.documents[index].sciDocument);
    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(g_hEdit, SCI_TARGETWHOLEDOCUMENT, 0, 0);
    SendMessageA(g_hEdit, SCI_REPLACETARGET, (WPARAM)-1, (LPARAM)text);
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
    end_background_document_access(&access);
    g_documentStore.documents[index].dirty = 1;
    g_recoverySnapshotStale = 1;
    update_tab_label(index);
}

static void update_workspace_links_after_rename(const WCHAR *oldPath, const WCHAR *newPath,
                                               int isDirectory) {
    if (!g_currentWorkspaceFolder) return;
    WorkspaceFiles files;
    if (!collect_workspace_files_at(&files, g_currentWorkspaceFolder)) return;
    int updatedFiles = 0, failedFiles = 0;
    for (int i = 0; i < files.count; i++) {
        if (language_for_path(files.paths[i])->id != LANG_MARKDOWN) continue;
        WorkspaceText source;
        if (load_workspace_text(files.paths[i], &source) <= 0) { failedFiles++; continue; }
        int changes = 0;
        char *rewritten = rewrite_links_for_source(source.text, files.paths[i], oldPath, newPath,
                                                   isDirectory, &changes);
        free(source.text);
        if (changes < 0) { free(rewritten); failedFiles++; continue; }
        if (!rewritten || !changes) { free(rewritten); continue; }
        if (source.openIndex >= 0) {
            replace_open_document_text(source.openIndex, rewritten);
            updatedFiles++;
        } else if (save_workspace_text(files.paths[i], rewritten, strlen(rewritten),
                                       source.encoding, source.hasBom) > 0) {
            updatedFiles++;
        } else {
            failedFiles++;
        }
        free(rewritten);
    }
    int truncated = files.truncated;
    free_workspace_files(&files);
    if (failedFiles || truncated) {
        WCHAR message[320];
        const WCHAR *renamed = isDirectory ? L"folder" : L"file";
        if (failedFiles)
            swprintf(message, sizeof(message) / sizeof(message[0]),
                     L"The %ls was renamed and %d Markdown file%ls %ls updated, but %d file%ls could not be updated%ls.",
                     renamed, updatedFiles, updatedFiles == 1 ? L"" : L"s", updatedFiles == 1 ? L"was" : L"were",
                     failedFiles, failedFiles == 1 ? L"" : L"s",
                     truncated ? L" and the workspace scan was incomplete" : L"");
        else
            swprintf(message, sizeof(message) / sizeof(message[0]),
                     L"The %ls was renamed and %d Markdown file%ls %ls updated, but the workspace scan was incomplete.",
                     renamed, updatedFiles, updatedFiles == 1 ? L"" : L"s",
                     updatedFiles == 1 ? L"was" : L"were");
        MessageBoxW(g_hMainWnd, message, L"Rename", MB_OK | MB_ICONWARNING);
    }
}

static void explorer_path_renamed(const WCHAR *oldPath, const WCHAR *newPath, int isDirectory) {
    invalidate_markdown_workspace_cache();
    if (!oldPath || !newPath) return;
    for (int i = 0; i < g_documentStore.count; i++) {
        if (!g_documentStore.documents[i].path ||
            !(isDirectory ? path_at_or_below(g_documentStore.documents[i].path, oldPath)
                           : _wcsicmp(g_documentStore.documents[i].path, oldPath) == 0)) continue;
        const WCHAR *suffix = isDirectory ? g_documentStore.documents[i].path + wcslen(oldPath) : L"";
        WCHAR *retargeted = (WCHAR *)malloc((wcslen(newPath) + wcslen(suffix) + 1) * sizeof(WCHAR));
        if (!retargeted) continue;
        wcscpy(retargeted, newPath);
        wcscat(retargeted, suffix);
        recent_list_remove(&g_recentFileList, g_documentStore.documents[i].path);
        replace_wstring(&g_documentStore.documents[i].path, retargeted);
        recent_list_add(&g_recentFileList, retargeted);
        record_document_file_time(i);
        update_tab_label(i);
        free(retargeted);
    }
    update_workspace_links_after_rename(oldPath, newPath, isDirectory);
    apply_document_language(g_currentFile);
    update_document_mapping();
    update_title();
    update_preview();
    update_outline();
    refresh_menu(g_hMainWnd);
    g_recoverySnapshotStale = 1;
}

/* g_splitterPos is a width relative to the content area to the right of the
   explorer panel, not an absolute client x - g_splitterRect (set by layout(),
   already shifted by however much the explorer panel currently occupies) is
   what's actually drawn, so hit-testing against it (rather than recomputing
   from g_splitterPos, which used to be equivalent back when there was no
   panel to shift past) keeps this in sync with what the user sees. */
static int splitter_contains_x(int x) {
    return g_splitterRect.right > g_splitterRect.left &&
           x >= g_splitterRect.left && x <= g_splitterRect.right;
}

static int explorer_splitter_contains_x(int x) {
    return g_explorerVisible && !g_focusMode &&
           x >= g_explorerSplitterRect.left && x <= g_explorerSplitterRect.right;
}

/* The output panel's splitter runs across the content area above it. */
static int output_splitter_contains(int x, int y) {
    return g_outputSplitterRect.right > g_outputSplitterRect.left &&
           x >= g_outputSplitterRect.left && x <= g_outputSplitterRect.right &&
           y >= g_outputSplitterRect.top && y <= g_outputSplitterRect.bottom;
}

/* The editor divider is nested inside the editor region and may run in
   either direction, so hit-test its complete rectangle. */
static int split_editor_splitter_contains(int x, int y) {
    return g_hSplitEdit && g_splitEditorVisible &&
           x >= g_splitEditorSplitterRect.left && x <= g_splitEditorSplitterRect.right &&
           y >= g_splitEditorSplitterRect.top && y <= g_splitEditorSplitterRect.bottom;
}

/* Applies g_previewDarkMode to the rendered Markdown preview only, leaving
   the editor and app chrome (select_theme(), below) untouched. Used both as
   the Markdown Theme control's own handler and by select_theme() itself to
   keep the preview in step with the main light/dark toggle. */
static void select_preview_theme(HWND hwnd, int darkMode) {
    g_previewDarkMode = darkMode;
    save_theme_settings();
    apply_preview_theme();
    apply_webview_background_colour();
    apply_preview_host_theme();
    refresh_menu(hwnd);
}

/* Applies darkMode to the editor and all the surrounding app chrome (menus,
   dialogs, tabs, toolbar, title bar), then syncs the rendered Markdown
   preview to match - this is the main light/dark toggle (View > Editor
   Theme, Settings > Appearance). The separate Markdown Theme control
   (select_preview_theme(), above) can still be used afterwards to override
   just the preview on its own. */
/* Repaints everything natively drawn in the current palette - after a theme
   switch, or after themes.lua changed the colours (Plugins > Reload). */
static void refresh_theme(HWND hwnd) {
    apply_menu_theme(hwnd, g_editorDarkMode);
    apply_editor_theme(g_hEdit);
    if (g_hMinimap) {
        apply_editor_theme(g_hMinimap);
        strip_minimap_bold();
        SendMessageA(g_hMinimap, SCI_SETZOOM, 0, 0);
    }
    if (g_hSplitEdit) {
        const WCHAR *splitPath = split_document_path();
        if (splitPath) {
            int mainLanguage = g_activeLanguage;
            apply_language_to_view(g_hSplitEdit, splitPath);
            g_activeLanguage = mainLanguage;
        } else {
            apply_editor_theme(g_hSplitEdit);
        }
    }
    apply_output_panel_theme();
    apply_main_theme(hwnd);
    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
        if (g_hToolbarButtons[i]) InvalidateRect(g_hToolbarButtons[i], NULL, TRUE);
    }
    apply_toolbar_tooltip_theme();
    if (g_hTabs) InvalidateRect(g_hTabs, NULL, TRUE);
    if (g_hStatus) InvalidateRect(g_hStatus, NULL, TRUE);
    apply_preview_host_theme();
    apply_webview_background_colour();
    apply_preview_theme(); /* themes.lua may have changed its scrollbar colours */
}

static void select_theme(HWND hwnd, int darkMode) {
    g_editorDarkMode = darkMode;
    save_theme_settings();
    refresh_theme(hwnd);
    select_preview_theme(hwnd, darkMode);
    fire_plugin_event("theme_changed", g_currentFile);
}

static int g_highContrastActive = 0;

/* Detects whether Windows' high-contrast accessibility mode is on and, if
   it just changed, picks whichever of the app's own two themes better
   matches the system's window background - high contrast is a distinct
   system setting from this app's own light/dark toggle, and this app has no
   dedicated third palette for it, so the closest pragmatic response is to
   follow the OS's own light/dark leaning rather than build a whole separate
   rendering path. Wired to WM_SYSCOLORCHANGE and the "ImmersiveColorSet"
   WM_SETTINGCHANGE notification, the two ways Windows announces a system
   theme/contrast change, plus once at startup. Turning high contrast back
   off leaves whichever theme was already selected alone. */
static void detect_high_contrast(void) {
    HIGHCONTRASTW hc = {0};
    hc.cbSize = sizeof(hc);
    int active = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
                 (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    if (active == g_highContrastActive) return;
    g_highContrastActive = active;
    if (!active || !g_hMainWnd) return;
    COLORREF windowColor = GetSysColor(COLOR_WINDOW);
    int luminance = (GetRValue(windowColor) * 299 + GetGValue(windowColor) * 587 +
                     GetBValue(windowColor) * 114) / 1000;
    select_theme(g_hMainWnd, luminance < 128);
}
