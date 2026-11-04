/* Some commands briefly point g_hEdit at other tabs' documents to read or
   change them (recovery snapshots, breakpoints, execution markers, link
   rewrites). SCI_SETDOCPOINTER resets the view's selection and scroll
   position, so these bracket that work and put the active tab's view back
   afterwards. */
typedef struct BackgroundDocumentAccess {
    sptr_t document;
    Sci_Position caret;
    Sci_Position anchor;
    Sci_Position firstVisibleLine;
    int xOffset;
    int oldSwitching;
} BackgroundDocumentAccess;

static void begin_background_document_access(BackgroundDocumentAccess *access) {
    access->document = (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0);
    access->caret = (Sci_Position)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    access->anchor = (Sci_Position)SendMessageA(g_hEdit, SCI_GETANCHOR, 0, 0);
    access->firstVisibleLine = (Sci_Position)SendMessageA(g_hEdit, SCI_GETFIRSTVISIBLELINE, 0, 0);
    access->xOffset = (int)SendMessageA(g_hEdit, SCI_GETXOFFSET, 0, 0);
    access->oldSwitching = g_documentStore.switching;
    g_documentStore.switching = 1;
}

static void end_background_document_access(const BackgroundDocumentAccess *access) {
    if (access->document &&
        (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0) != access->document) {
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)access->document);
    }
    Sci_Position length = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    Sci_Position caret = access->caret > length ? length : access->caret;
    Sci_Position anchor = access->anchor > length ? length : access->anchor;
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)anchor, (LPARAM)caret);
    SendMessageA(g_hEdit, SCI_SETFIRSTVISIBLELINE, (WPARAM)access->firstVisibleLine, 0);
    SendMessageA(g_hEdit, SCI_SETXOFFSET, (WPARAM)access->xOffset, 0);
    g_documentStore.switching = access->oldSwitching;
}

static char *get_document_text(int index, Sci_Position *outLength) {
    if (!g_hEdit || index < 0 || index >= g_documentStore.count) return NULL;
    sptr_t current = (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0);
    if (current == g_documentStore.documents[index].sciDocument) return get_edit_text(g_hEdit, outLength);
    BackgroundDocumentAccess access;
    begin_background_document_access(&access);
    SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documentStore.documents[index].sciDocument);
    char *text = get_edit_text(g_hEdit, outLength);
    end_background_document_access(&access);
    return text;
}

static int document_index_for_sci_document(sptr_t document) {
    if (!document) return -1;
    for (int i = 0; i < g_documentStore.count; i++)
        if (g_documentStore.documents[i].sciDocument == document) return i;
    return -1;
}

static int document_index_for_view(HWND view) {
    return view ? document_index_for_sci_document(
        (sptr_t)SendMessageA(view, SCI_GETDOCPOINTER, 0, 0)) : -1;
}

static int split_document_index(void) {
    return document_index_for_sci_document(g_splitDocument);
}

/* NULL means the split follows the selected main tab, as it always did
   before independent split documents were introduced. */
static const WCHAR *split_document_path(void) {
    int index = split_document_index();
    return g_splitEditorVisible && index >= 0 && index != g_documentStore.active
        ? g_documentStore.documents[index].path : NULL;
}


/* ================= rest of the app (menus, file I/O, find/replace, font) ================= */

static const WCHAR *document_short_name(const Document *doc) {
    return doc->path ? PathFindFileNameW(doc->path) : NULL;
}

static void format_document_label(const Document *doc, WCHAR *label, size_t capacity) {
    const WCHAR *shortName = document_short_name(doc);
    WCHAR git = git_status_for_path(doc->path);
    WCHAR suffix[8] = L"";
    if (doc->dirty) wcscat(suffix, L" *");
    if (git) {
        size_t used = wcslen(suffix);
        if (used + 2 < sizeof(suffix) / sizeof(suffix[0])) {
            suffix[used++] = L' ';
            suffix[used++] = git;
            suffix[used] = 0;
        }
    }
    if (g_splitEditorVisible && g_splitDocument == doc->sciDocument) wcscat(suffix, L" ↔");
    if (doc->isReference && !shortName) {
        swprintf(label, capacity, L"%ls%ls", reference_title(doc->isReference), suffix);
    } else if (shortName && shortName[0]) {
        swprintf(label, capacity, L"%ls%ls", shortName, suffix);
    } else if (doc->untitledNumber == 0) {
        swprintf(label, capacity, L"Untitled.txt%ls", suffix);
    } else {
        swprintf(label, capacity, L"Untitled %u%ls", doc->untitledNumber, suffix);
    }
}

static void update_tab_label(int index) {
    if (!g_hTabs || index < 0 || index >= g_documentStore.count) return;
    Document *doc = &g_documentStore.documents[index];
    WCHAR label[MAX_PATH + 32];
    format_document_label(doc, label, sizeof(label) / sizeof(label[0]));
    TCITEMW item = {0};
    item.mask = TCIF_TEXT;
    item.pszText = label;
    SendMessageW(g_hTabs, TCM_SETITEMW, (WPARAM)index, (LPARAM)&item);
    InvalidateRect(g_hTabs, NULL, FALSE);
}

static void toggle_document_pinned(int index) {
    if (index < 0 || index >= g_documentStore.count) return;
    Document *doc = &g_documentStore.documents[index];
    doc->pinned = !doc->pinned;
    update_tab_label(index);
    refresh_menu(g_hMainWnd);
    show_plugin_reload_status(doc->pinned ? L"Tab pinned" : L"Tab unpinned");
}

static int compare_tab_recency(const void *left, const void *right) {
    int a = *(const int *)left, b = *(const int *)right;
    unsigned long long first = g_documentStore.documents[a].lastActivated;
    unsigned long long second = g_documentStore.documents[b].lastActivated;
    if (first != second) return first > second ? -1 : 1;
    return a - b;
}

/* Ctrl+E: searchable open tabs, ordered most-recently-used with the current
   tab last. Enter therefore switches straight back to the previous tab. */
static void show_tab_switcher(void) {
    if (g_documentStore.count <= 1) { MessageBeep(MB_ICONASTERISK); return; }
    int order[MAX_DOCUMENTS], count = 0;
    for (int i = 0; i < g_documentStore.count; i++)
        if (i != g_documentStore.active) order[count++] = i;
    qsort(order, (size_t)count, sizeof(order[0]), compare_tab_recency);
    order[count++] = g_documentStore.active;

    WCHAR *labels[MAX_DOCUMENTS] = {0};
    for (int row = 0; row < count; row++) {
        const Document *doc = &g_documentStore.documents[order[row]];
        WCHAR name[MAX_PATH + 32];
        format_document_label(doc, name, sizeof(name) / sizeof(name[0]));
        const WCHAR *prefix = doc->pinned ? L"[Pinned] " : L"";
        size_t capacity = wcslen(prefix) + wcslen(name) +
                          (doc->path ? wcslen(doc->path) + 4 : 1);
        labels[row] = (WCHAR *)malloc(capacity * sizeof(WCHAR));
        if (!labels[row]) {
            for (int i = 0; i < row; i++) free(labels[i]);
            return;
        }
        if (doc->path) swprintf(labels[row], capacity, L"%ls%ls — %ls", prefix, name, doc->path);
        else swprintf(labels[row], capacity, L"%ls%ls", prefix, name);
    }
    int selected = show_choice_dialog(L"Open Tab", L"Type part of a tab name or path:",
                                      labels, count, L"Open");
    for (int i = 0; i < count; i++) free(labels[i]);
    if (selected >= 0 && selected < count) switch_document(order[selected]);
}

static void save_active_document_view(void) {
    if (!g_hEdit || g_documentStore.count <= 0) return;
    Document *doc = &g_documentStore.documents[g_documentStore.active];
    doc->caret = (Sci_Position)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    doc->anchor = (Sci_Position)SendMessageA(g_hEdit, SCI_GETANCHOR, 0, 0);
    doc->firstVisibleLine = (Sci_Position)SendMessageA(g_hEdit, SCI_GETFIRSTVISIBLELINE, 0, 0);
    doc->xOffset = (int)SendMessageA(g_hEdit, SCI_GETXOFFSET, 0, 0);
}

/* Puts a document's remembered selection and scroll position back on a view. */
static void restore_document_view(HWND hEdit, const Document *doc) {
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)doc->anchor, (LPARAM)doc->caret);
    SendMessageA(hEdit, SCI_SETFIRSTVISIBLELINE, (WPARAM)doc->firstVisibleLine, 0);
    SendMessageA(hEdit, SCI_SETXOFFSET, (WPARAM)doc->xOffset, 0);
}

static void record_document_file_time(int index) {
    if (index < 0 || index >= g_documentStore.count) return;
    Document *doc = &g_documentStore.documents[index];
    doc->hasFileTime = 0;
    if (!doc->path) return;
    WCHAR *ioPath = extended_path(doc->path);
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (ioPath && GetFileAttributesExW(ioPath, GetFileExInfoStandard, &data)) {
        doc->fileTime = data.ftLastWriteTime;
        doc->hasFileTime = 1;
    }
    doc->externalChanged = 0;
    doc->externalDeleted = 0;
    if (g_hTabs) InvalidateRect(g_hTabs, NULL, FALSE);
    free(ioPath);
}

/* A hidden Scintilla view still observes and lays out changes to a shared
   document. Keep the secondary editor on a private empty document until it
   is genuinely visible, then attach it to the active tab and start it at the
   main pane's current location. */
static void sync_split_editor_document(void) {
    if (!g_hSplitEdit || !g_splitScratchDocument || g_documentStore.count <= 0) return;
    int splitIndex = split_document_index();
    if (g_splitDocument && (splitIndex < 0 ||
        g_documentStore.documents[splitIndex].viewKind != DOCUMENT_VIEW_TEXT ||
        g_documentStore.documents[splitIndex].largeFile)) {
        g_splitDocument = 0;
        splitIndex = -1;
    }
    int visible = g_splitEditorVisible && editor_pane_visible() && !g_focusMode;
    sptr_t desired = visible
        ? (g_splitDocument ? g_splitDocument
                           : g_documentStore.documents[g_documentStore.active].sciDocument)
        : g_splitScratchDocument;
    sptr_t current = (sptr_t)SendMessageA(g_hSplitEdit, SCI_GETDOCPOINTER, 0, 0);
    if (current == desired) return;

    if (!visible && GetFocus() == g_hSplitEdit) SetFocus(g_hEdit);
    SendMessageA(g_hSplitEdit, SCI_SETDOCPOINTER, 0, (LPARAM)desired);
    if (visible) {
        int desiredIndex = document_index_for_sci_document(desired);
        if (desiredIndex >= 0) restore_document_view(
            g_hSplitEdit, &g_documentStore.documents[desiredIndex]);
        remeasure_linenumber_margin(g_hSplitEdit);
    } else {
        g_lastEditView = g_hEdit;
    }
}

static void set_split_document(int index) {
    if (index < 0 || index >= g_documentStore.count) return;
    Document *doc = &g_documentStore.documents[index];
    if (doc->viewKind != DOCUMENT_VIEW_TEXT || doc->largeFile) {
        MessageBoxW(g_hMainWnd, L"Images and large-file documents cannot be shown in the editor split.",
                    L"Split Editor", MB_OK | MB_ICONINFORMATION);
        return;
    }
    sptr_t previous = g_splitDocument;
    /* Choosing the selected tab means "follow the tab strip" again, which is
       also the way to release a document retained in the secondary pane. */
    g_splitDocument = index == g_documentStore.active ? 0 : doc->sciDocument;
    if (!g_splitEditorVisible) {
        g_splitEditorVertical = 1;
        g_splitEditorPos = -1;
        g_splitEditorVisible = 1;
    }
    sync_split_editor_document();
    apply_document_language(g_currentFile);
    for (int i = 0; i < g_documentStore.count; i++)
        if (g_documentStore.documents[i].sciDocument == previous || i == index)
            update_tab_label(i);
    layout(g_hMainWnd);
    update_view_menu(g_hMainWnd);
    save_layout_settings();
    SetFocus(g_hSplitEdit);
}

static void show_split_document_dialog(void) {
    WCHAR *labels[MAX_DOCUMENTS] = {0};
    int indices[MAX_DOCUMENTS], count = 0;
    for (int i = 0; i < g_documentStore.count; i++) {
        const Document *doc = &g_documentStore.documents[i];
        if (doc->viewKind != DOCUMENT_VIEW_TEXT || doc->largeFile) continue;
        WCHAR name[MAX_PATH + 32];
        format_document_label(doc, name, sizeof(name) / sizeof(name[0]));
        size_t capacity = wcslen(name) + (doc->path ? wcslen(doc->path) : 0) + 32;
        labels[count] = (WCHAR *)malloc(capacity * sizeof(WCHAR));
        if (!labels[count]) continue;
        swprintf(labels[count], capacity, doc->path ? L"%ls — %ls" : L"%ls%ls",
                 name, doc->path ? doc->path : (i == g_documentStore.active ? L" (current)" : L""));
        indices[count++] = i;
    }
    if (!count) { MessageBeep(MB_ICONASTERISK); return; }
    int selected = show_choice_dialog(L"Show Document in Split",
        L"Choose a document for the split; choosing the current tab follows it:", labels, count, L"Show");
    for (int i = 0; i < count; i++) free(labels[i]);
    if (selected >= 0 && selected < count) set_split_document(indices[selected]);
}

/* Swaps the main editor to the active tab and refreshes the split editor,
   which may follow that tab or retain its own document. The minimap owns a
   separate snapshot document so normal edits do not make its Scintilla view
   process every byte change before the throttled refresh. Not used for the
   transient peeks in get_document_text() and clear_debug_execution_markers(),
   which restore the original pointer before returning. */
static void set_active_editor_document(sptr_t doc) {
    SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)doc);
    sync_split_editor_document();
}

static void switch_document(int index) {
    if (!g_hEdit || index < 0 || index >= g_documentStore.count || index == g_documentStore.active) return;
    save_active_document_view();
    g_documentStore.switching = 1;
    g_documentStore.active = index;
    g_documentStore.documents[index].lastActivated = ++g_tabActivationSerial;
    set_active_editor_document(g_documentStore.documents[index].sciDocument);
    apply_preview_host_theme();
    apply_webview_background_colour();
    apply_editor_font();
    apply_document_language(g_documentStore.documents[index].path);
    restore_document_view(g_hEdit, &g_documentStore.documents[index]);
    update_linenumber_margin_width(g_hEdit);
    if (g_hSplitEdit) {
        if (g_splitEditorVisible)
            update_linenumber_margin_width(g_hSplitEdit);
    }
    g_documentStore.switching = 0;
    if (g_hTabs) TabCtrl_SetCurSel(g_hTabs, index);
    update_document_mapping();
    update_preview();
    layout(g_hMainWnd);
    update_title();
    update_status_bar();
    update_outline();
    prompt_reload_active_external_change();
    /* Switching documents can be triggered from outside the editor area
       itself - a plugin activating a tab, or do_save_all()/do_exit()'s
       switch-to-each-tab loops - and shouldn't steal focus from a pane like
       the terminal the user is actively using when that happens. */
    if (!active_document_is_image() && !focus_outside_editor_area()) SetFocus(g_hEdit);
}

static int create_document_tab_internal(int deferPresentation) {
    if (g_documentStore.count >= MAX_DOCUMENTS) {
        MessageBoxW(g_hMainWnd, L"Tinker Editor can keep up to 32 documents open at once.", L"Tinker Editor", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (g_documentStore.count > 0) save_active_document_view();
    sptr_t sciDoc = (sptr_t)SendMessageA(g_hEdit, SCI_CREATEDOCUMENT, 0, 0);
    if (!sciDoc) return 0;
    int index = g_documentStore.count++;
    memset(&g_documentStore.documents[index], 0, sizeof(g_documentStore.documents[index]));
    g_documentStore.documents[index].sciDocument = sciDoc;
    g_documentStore.documents[index].untitledNumber = g_documentStore.nextUntitledNumber++;
    g_documentStore.documents[index].lastActivated = ++g_tabActivationSerial;
    g_documentStore.active = index;
    g_documentStore.switching = 1;
    set_active_editor_document(sciDoc);
    configure_scintilla(g_hEdit);
    apply_editor_font();
    reset_undo_history(g_hEdit);
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    g_documentStore.switching = 0;
    TCITEMW item = {0};
    item.mask = TCIF_TEXT;
    item.pszText = L"";
    SendMessageW(g_hTabs, TCM_INSERTITEMW, (WPARAM)index, (LPARAM)&item);
    TabCtrl_SetCurSel(g_hTabs, index);
    update_tab_label(index);
    if (!deferPresentation) {
        update_document_mapping();
        update_preview();
        layout(g_hMainWnd);
        update_title();
    }
    return 1;
}

static int create_document_tab(void) {
    return create_document_tab_internal(0);
}

/* The document plugins were last told is active, and whether a check for a
   change is already queued. */
static sptr_t g_pluginActiveDocument = 0;
static int g_pluginTabCheckPosted = 0;

/* Queued, so a sweep through the tabs and back again (Save All, exit)
   reaches plugins as no change at all. */
static void queue_plugin_tab_check(void) {
    if (g_hMainWnd && !g_pluginTabCheckPosted &&
        g_documentStore.documents[g_documentStore.active].sciDocument != g_pluginActiveDocument) {
        g_pluginTabCheckPosted = 1;
        PostMessageW(g_hMainWnd, WM_APP_PLUGIN_TAB_CHECK, 0, 0);
    }
}

static void update_title(void) {
    /* Every change of active document (switching, a new tab, closing one)
       ends up here. */
    queue_plugin_tab_check();
    const Document *active = &g_documentStore.documents[g_documentStore.active];
    const WCHAR *name = g_currentFile ? g_currentFile
        : active->isReference ? reference_title(active->isReference)
        : (active->untitledNumber == 0 ? L"Untitled.txt" : L"Untitled");
    size_t cap = wcslen(name) + 32;
    WCHAR *title = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (!title) return;
    swprintf(title, cap, L"%ls%ls - Tinker Editor", name, g_dirty ? L"*" : L"");
    SetWindowTextW(g_hMainWnd, title);
    free(title);
}

static void set_dirty(int dirty) {
    g_dirty = dirty;
    g_recoverySnapshotStale = 1;
    update_tab_label(g_documentStore.active);
    update_title();
    update_status_position();
}

/* Swaps the active tab onto a different Scintilla document and releases the
   one it had. A document's storage options are fixed when it's created, so
   load_file() needs this whenever a file's large-file mode differs from the
   one the tab's current document was made for - and on every large-file
   load, since the loader always builds a new document. Configured the same
   way create_document_tab() sets up a new tab's document. */
static void replace_active_scintilla_document(sptr_t sciDoc, int largeFile) {
    sptr_t old = g_documentStore.documents[g_documentStore.active].sciDocument;
    int oldSwitching = g_documentStore.switching;
    g_documentStore.switching = 1;
    g_documentStore.documents[g_documentStore.active].sciDocument = sciDoc;
    if (g_splitDocument == old) g_splitDocument = sciDoc;
    g_largeFile = largeFile;
    set_active_editor_document(sciDoc);
    configure_scintilla(g_hEdit);
    apply_editor_font();
    SendMessageA(g_hEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)old);
    g_documentStore.switching = oldSwitching;
}

/* A preview-only layout cannot show a large document because its rendered
   preview is deliberately disabled. Reveal the editor automatically; for a
   command-line/file-association launch, replace the preview pane outright so
   the initial window contains the document rather than only an explanation. */
static void ensure_large_file_editor_visible(void) {
    if (!g_largeFile || g_editorVisible) return;
    g_editorVisible = 1;
    if (g_commandLinePreviewMode) g_previewVisible = 0;
    update_view_menu(g_hMainWnd);
    layout(g_hMainWnd);
    InvalidateRect(g_hMainWnd, NULL, TRUE);
}

/* Scintilla's ILoader (ILoader.h) is a C++ interface. It has no virtual
   destructor, so its vtable is exactly these three slots in declaration
   order and can be called from C the same way the COM interfaces are. */
typedef struct SciLoader SciLoader;
typedef struct SciLoaderVtbl {
    int (SCI_METHOD *Release)(SciLoader *self);
    int (SCI_METHOD *AddData)(SciLoader *self, const char *data, Sci_Position length);
    void *(SCI_METHOD *ConvertToDocument)(SciLoader *self);
} SciLoaderVtbl;
struct SciLoader { const SciLoaderVtbl *lpVtbl; };

#define LARGE_FILE_READ_CHUNK (4 * 1024 * 1024)
#define ORDINARY_FILE_READ_CHUNK (1024 * 1024)

/* Streams `file` into a new large-file document (see LARGE_FILE_FEATURES)
   through SCI_CREATELOADER, so the text is only ever held once - by
   Scintilla - rather than in a read buffer and then copied into the
   document by SCI_SETTEXT. The document stores no styles (halving its
   memory; it's never lexed) and uses 64-bit positions, which is what lets
   files past 2 GB open at all. Any encoding other than UTF-8 is converted
   a chunk at a time on the way in. `forcedEncoding` is a TextEncoding, or
   -1 to detect it: from the first chunk, then - for a file that looks like
   UTF-8 - by validating the rest as it streams, starting over as ANSI if a
   later chunk turns out not to be. Returns 0 on failure, with *outOfMemory
   set when Scintilla couldn't allocate the text. */
static sptr_t read_large_file_document(HANDLE file, size_t length, int forcedEncoding,
                                       int *outEncoding, int *outHasBom, int *outOfMemory,
                                       LargeOperationProgress *progress) {
    *outOfMemory = 0;
    char *chunk = (char *)malloc(LARGE_FILE_READ_CHUNK);
    char *decoded = NULL;
    size_t decodedCapacity = text_decode_capacity(LARGE_FILE_READ_CHUNK);
    if (!chunk || !decodedCapacity) {
        free(chunk);
        *outOfMemory = 1;
        return 0;
    }
    sptr_t result = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        LARGE_INTEGER start = {0};
        if (attempt && !SetFilePointerEx(file, start, NULL, FILE_BEGIN)) break;
        if (!large_operation_progress_update(progress, 0, length)) break;
        SciLoader *loader = (SciLoader *)SendMessageA(g_hEdit, SCI_CREATELOADER, (WPARAM)length,
            (LPARAM)(SC_DOCUMENTOPTION_STYLES_NONE | SC_DOCUMENTOPTION_TEXT_LARGE));
        if (!loader) { *outOfMemory = 1; break; }
        TextCodecState codec = { TEXT_ENCODING_UTF8, {0}, 0 };
        Utf8Validator validator;
        utf8_validator_init(&validator);
        int validating = 0, ok = 1, restart = 0;
        *outHasBom = 0;
        size_t total = 0;
        while (ok && total < length) {
            DWORD want = (DWORD)((length - total) > LARGE_FILE_READ_CHUNK ? LARGE_FILE_READ_CHUNK : (length - total));
            DWORD got = 0;
            if (!ReadFile(file, chunk, want, &got, NULL) || got == 0) { ok = 0; break; }
            const char *data = chunk;
            size_t dataLength = got;
            if (total == 0) {
                size_t bom = 0;
                if (forcedEncoding >= 0) {
                    codec.encoding = (TextEncoding)forcedEncoding;
                    bom = text_bom_length(chunk, got, codec.encoding);
                } else {
                    codec.encoding = detect_text_encoding(chunk, got, got == length, &bom);
                    /* A first chunk that looks like UTF-8 only says so much. */
                    validating = codec.encoding == TEXT_ENCODING_UTF8 && !bom;
                }
                *outHasBom = bom > 0;
                data += bom;
                dataLength -= bom;
            }
            total += got;
            if (!large_operation_progress_update(progress, total, length)) { ok = 0; break; }
            if (validating && !utf8_validator_feed(&validator, data, dataLength)) { restart = 1; break; }
            if (codec.encoding != TEXT_ENCODING_UTF8) {
                if (!decoded) {
                    decoded = (char *)malloc(decodedCapacity);
                    if (!decoded) { *outOfMemory = 1; ok = 0; break; }
                }
                size_t produced = 0;
                if (!text_decode(&codec, data, dataLength, total == length, decoded,
                                 decodedCapacity, &produced)) { ok = 0; break; }
                data = decoded;
                dataLength = produced;
            }
            int status = dataLength ? loader->lpVtbl->AddData(loader, data, (Sci_Position)dataLength) : SC_STATUS_OK;
            if (status != SC_STATUS_OK) {
                *outOfMemory = status == SC_STATUS_BADALLOC;
                ok = 0;
            }
        }
        if (ok && !restart && validating && !utf8_validator_finish(&validator)) restart = 1;
        if (!ok || restart) {
            loader->lpVtbl->Release(loader);
            if (!ok) break;
            forcedEncoding = TEXT_ENCODING_ANSI;
            continue;
        }
        *outEncoding = codec.encoding;
        result = (sptr_t)loader->lpVtbl->ConvertToDocument(loader);
        break;
    }
    free(chunk);
    free(decoded);
    return result;
}

/* Why a file didn't load, for the message load_file_as() shows. */
typedef enum {
    LOAD_OK, LOAD_READ_FAILED, LOAD_OUT_OF_MEMORY, LOAD_TOO_LARGE, LOAD_CANCELLED
} LoadResult;

/* Streams `file` into a new large-file document for the active tab (see
   read_large_file_document()). */
static LoadResult load_large_file(HANDLE file, size_t length, int forcedEncoding) {
    /* SCI_CREATELOADER resets g_hEdit's per-view line state as a side
       effect (Editor.cxx's CreateLoader). A successful load swaps in the
       new document, which rebuilds it; a failed one re-attaches the
       tab's own document below to do the same, so the view is saved
       first to put the caret and scroll position back. */
    save_active_document_view();
    HCURSOR oldCursor = SetCursor(LoadCursorA(NULL, IDC_WAIT));
    int hasBom = 0, encoding = TEXT_ENCODING_UTF8, outOfMemory = 0;
    LargeOperationProgress progress;
    large_operation_progress_begin(&progress, L"Opening large file", L"Loading text...");
    sptr_t sciDoc = read_large_file_document(file, length, forcedEncoding, &encoding,
                                             &hasBom, &outOfMemory, &progress);
    int cancelled = progress.cancelled;
    large_operation_progress_end(&progress);
    SetCursor(oldCursor);
    if (!sciDoc) {
        const Document *doc = &g_documentStore.documents[g_documentStore.active];
        g_documentStore.switching = 1;
        set_active_editor_document(doc->sciDocument);
        restore_document_view(g_hEdit, doc);
        g_documentStore.switching = 0;
        return cancelled ? LOAD_CANCELLED : outOfMemory ? LOAD_OUT_OF_MEMORY : LOAD_READ_FAILED;
    }
    g_documentStore.switching = 1;
    replace_active_scintilla_document(sciDoc, 1);
    apply_detected_eol_mode(g_hEdit);
    /* A loader-built document starts with undo collection off. */
    SendMessageA(g_hEdit, SCI_SETUNDOCOLLECTION, TRUE, 0);
    reset_undo_history(g_hEdit);
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    g_documentStore.switching = 0;
    g_hasBom = hasBom;
    g_encoding = encoding;
    g_documentStore.documents[g_documentStore.active].encodingForced = forcedEncoding >= 0;
    /* Swapping in a loader-built document raises no SCN_MODIFIED, which is
       what refreshes these after an ordinary load. */
    update_linenumber_margin_width(g_hEdit);
    if (g_hSplitEdit) update_linenumber_margin_width(g_hSplitEdit);
    update_status_bar();
    /* No convert_indentation() here, unlike set_loaded_text(): rewriting
       every line of a large file on load is the kind of whole-document pass
       this mode exists to avoid. */
    return LOAD_OK;
}

/* Builds a style-less, 64-bit-position document from text that was decoded by
   the ordinary loader but expanded past the large-file threshold. The loader
   takes ownership of its own storage; `content` remains the caller's. */
static sptr_t large_document_from_utf8(const char *content, size_t length, int *outOfMemory) {
    *outOfMemory = 0;
    SciLoader *loader = (SciLoader *)SendMessageA(g_hEdit, SCI_CREATELOADER, (WPARAM)length,
        (LPARAM)(SC_DOCUMENTOPTION_STYLES_NONE | SC_DOCUMENTOPTION_TEXT_LARGE));
    if (!loader) {
        *outOfMemory = 1;
        return 0;
    }
    int status = length ? loader->lpVtbl->AddData(loader, content, (Sci_Position)length)
                        : SC_STATUS_OK;
    if (status != SC_STATUS_OK) {
        *outOfMemory = status == SC_STATUS_BADALLOC;
        loader->lpVtbl->Release(loader);
        return 0;
    }
    return (sptr_t)loader->lpVtbl->ConvertToDocument(loader);
}

/* Puts already-decoded UTF-8 text into the active tab as its loaded,
   unmodified content. */
static LoadResult set_loaded_text(const char *content, size_t contentLength, int hasBom,
                                  TextEncoding encoding, int forcedEncoding) {
    /* The threshold describes editor size, not merely bytes on disk: ANSI
       text can expand to three UTF-8 bytes per source byte. */
    int largeMode = (long long)contentLength >= large_file_threshold_bytes();
    int loadedThroughLoader = 0;
    if (largeMode && !g_largeFile) {
        int outOfMemory = 0;
        sptr_t sciDoc = large_document_from_utf8(content, contentLength, &outOfMemory);
        if (!sciDoc) return outOfMemory ? LOAD_OUT_OF_MEMORY : LOAD_READ_FAILED;
        replace_active_scintilla_document(sciDoc, 1);
        loadedThroughLoader = 1;
    } else if (!largeMode && g_largeFile) {
        /* Reloading a tab that was in large-file mode with a file that's since
           shrunk needs an ordinary, lexable document again. */
        sptr_t sciDoc = (sptr_t)SendMessageA(g_hEdit, SCI_CREATEDOCUMENT, 0, 0);
        if (sciDoc) replace_active_scintilla_document(sciDoc, 0);
        else largeMode = 1; /* keep the existing style-less document mode */
    }
    g_largeFile = largeMode;
    g_hasBom = hasBom;
    g_encoding = encoding;
    g_documentStore.documents[g_documentStore.active].encodingForced = forcedEncoding >= 0;
    g_documentStore.switching = 1;
    if (loadedThroughLoader) {
        apply_detected_eol_mode(g_hEdit);
        SendMessageA(g_hEdit, SCI_SETUNDOCOLLECTION, TRUE, 0);
    } else {
        set_edit_text(g_hEdit, content, (Sci_Position)contentLength);
    }
    /* Matches toggle_indent_use_spaces(): when the setting is on, a file
       opened (or dropped) with tab-indentation should come in already
       converted rather than leaving a mix of tabs and spaces. Done inside the
       g_documentStore.switching window and before reset_undo_history() so
       it's folded into the loaded state - no spurious dirty flag and nothing
       to undo back into tabs. */
    if (g_indentUseSpaces && !g_largeFile) convert_indentation(g_hEdit, 1);
    reset_undo_history(g_hEdit);
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    g_documentStore.switching = 0;
    return LOAD_OK;
}

/* Reads all of `file` into memory, converts it to UTF-8, and loads it into
   the active tab. */
static LoadResult load_ordinary_file(HANDLE file, size_t length, int forcedEncoding) {
    char *buf = (char *)malloc(length + 1);
    if (!buf) return LOAD_OUT_OF_MEMORY;
    size_t total = 0;
    while (total < length) {
        DWORD chunk = (DWORD)((length - total) > ORDINARY_FILE_READ_CHUNK ? ORDINARY_FILE_READ_CHUNK : (length - total));
        DWORD got = 0;
        if (!ReadFile(file, buf + total, chunk, &got, NULL) || got == 0) break;
        total += got;
    }
    if (total != length) {
        free(buf);
        return LOAD_READ_FAILED;
    }
    buf[length] = 0;
    size_t bom = 0;
    TextEncoding encoding = forcedEncoding >= 0 ? (TextEncoding)forcedEncoding
                                                : detect_text_encoding(buf, length, 1, &bom);
    if (forcedEncoding >= 0) bom = text_bom_length(buf, length, encoding);
    LoadResult result = LOAD_OK;
    char *decoded = NULL;
    const char *content = buf + bom;
    size_t contentLength = length - bom;
    if (encoding != TEXT_ENCODING_UTF8) {
        decoded = text_decode_all(encoding, content, contentLength, &contentLength);
        if (!decoded) result = LOAD_OUT_OF_MEMORY;
        /* ANSI text can triple in size as UTF-8, past what an ordinary
           (32-bit) document holds; only large-file mode can take that. */
        else if (contentLength > INT_MAX) result = LOAD_TOO_LARGE;
        content = decoded;
    }
    if (result == LOAD_OK)
        result = set_loaded_text(content, contentLength, bom > 0, encoding, forcedEncoding);
    free(decoded);
    free(buf);
    return result;
}

/* Reads the last bytes before `size` into the document's loadedTail, which
   append_file_growth() compares to tell a file that grew from one that was
   rewritten. */
static void read_loaded_tail(HANDLE file, long long size, Document *doc) {
    long long count = size < (long long)sizeof(doc->loadedTail) ? size : (long long)sizeof(doc->loadedTail);
    LARGE_INTEGER at;
    at.QuadPart = size - count;
    DWORD got = 0;
    doc->loadedTailLength = 0;
    if (SetFilePointerEx(file, at, NULL, FILE_BEGIN) &&
        ReadFile(file, doc->loadedTail, (DWORD)count, &got, NULL) && got == (DWORD)count)
        doc->loadedTailLength = (int)got;
    doc->loadedBytes = size;
    doc->loadedLength = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
}

static void record_loaded_file_end(HANDLE file, long long size) {
    Document *doc = &g_documentStore.documents[g_documentStore.active];
    TextCodecState codec = { (TextEncoding)doc->encoding, {0}, 0 };
    doc->appendCodec = codec;
    read_loaded_tail(file, size, doc);
}

/* The tab's text no longer comes straight from a load (it was saved, or
   replaced some other way), so growth has to be reloaded in full. */
static void forget_loaded_file_end(Document *doc) {
    doc->loadedTailLength = 0;
}

static int file_ends_with_loaded_tail(HANDLE file, const Document *doc) {
    unsigned char bytes[sizeof(doc->loadedTail)];
    LARGE_INTEGER at;
    at.QuadPart = doc->loadedBytes - doc->loadedTailLength;
    DWORD got = 0;
    return SetFilePointerEx(file, at, NULL, FILE_BEGIN) &&
           ReadFile(file, bytes, (DWORD)doc->loadedTailLength, &got, NULL) &&
           got == (DWORD)doc->loadedTailLength &&
           memcmp(bytes, doc->loadedTail, got) == 0;
}

/* Appends the active document's file from loadedBytes to `size`, decoding
   it a chunk at a time; a sequence split across the end carries over to
   the next append in appendCodec. Undo isn't collected, as for a load. */
static int append_file_bytes(HANDLE file, Document *doc, long long size) {
    int decode = doc->encoding != TEXT_ENCODING_UTF8;
    long long growth = size - doc->loadedBytes;
    size_t chunkCapacity = growth < LARGE_FILE_READ_CHUNK ? (size_t)growth : LARGE_FILE_READ_CHUNK;
    size_t decodedCapacity = decode ? text_decode_capacity(chunkCapacity) : 0;
    char *chunk = chunkCapacity ? (char *)malloc(chunkCapacity) : NULL;
    char *decoded = decode && decodedCapacity ? (char *)malloc(decodedCapacity) : NULL;
    LARGE_INTEGER at;
    at.QuadPart = doc->loadedBytes;
    int ok = chunk && (!decode || decoded) && SetFilePointerEx(file, at, NULL, FILE_BEGIN);
    TextCodecState codec = doc->appendCodec;
    long long total = doc->loadedBytes;
    SendMessageA(g_hEdit, SCI_SETUNDOCOLLECTION, FALSE, 0);
    while (ok && total < size) {
        DWORD want = (DWORD)((size - total) > (long long)chunkCapacity ? chunkCapacity : (size - total));
        DWORD got = 0;
        if (!ReadFile(file, chunk, want, &got, NULL) || got == 0) { ok = 0; break; }
        total += got;
        const char *data = chunk;
        size_t dataLength = got;
        if (decode) {
            if (!text_decode(&codec, chunk, got, 0, decoded,
                             decodedCapacity, &dataLength)) { ok = 0; break; }
            data = decoded;
        }
        if (dataLength) SendMessageA(g_hEdit, SCI_APPENDTEXT, (WPARAM)dataLength, (LPARAM)data);
    }
    SendMessageA(g_hEdit, SCI_SETUNDOCOLLECTION, TRUE, 0);
    /* The append ran with undo collection off, so Scintilla's change history
       already classifies the new bytes as original file content. Only old
       undo actions need discarding. reset_undo_history() must not be used
       here: toggling change history off and on rebuilds its position map for
       the whole document, making every tiny Follow EOF update O(file size). */
    SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
    free(chunk);
    free(decoded);
    if (ok) {
        doc->appendCodec = codec;
        read_loaded_tail(file, size, doc);
    }
    return ok;
}

/* A followed log (plugins/tail.lua) only grows. When the file still has
   the bytes the tab last read from it, this reads just what was added and
   appends it, instead of reloading the whole file - which for a large file
   means re-reading and re-decoding all of it, behind a progress dialog,
   every time it changes. Returns 0 when the file changed some other way or
   the tab's text isn't what was loaded, for a full reload; that also
   replaces anything a failed append left behind. */
static int append_file_growth(void) {
    Document *doc = &g_documentStore.documents[g_documentStore.active];
    if (!doc->path || doc->loadedTailLength <= 0 || g_dirty ||
        (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0) != doc->loadedLength) return 0;
    WCHAR *ioPath = extended_path(doc->path);
    HANDLE file = ioPath ? CreateFileW(ioPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL)
        : INVALID_HANDLE_VALUE;
    free(ioPath);
    if (file == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size;
    int appended = 0;
    if (GetFileSizeEx(file, &size) && size.QuadPart > doc->loadedBytes &&
        /* An ordinary document has 32-bit positions; ANSI can triple in UTF-8. */
        (g_largeFile || doc->loadedLength + (size.QuadPart - doc->loadedBytes) * 3 <= INT_MAX) &&
        file_ends_with_loaded_tail(file, doc))
        appended = append_file_bytes(file, doc, size.QuadPart);
    CloseHandle(file);
    if (!appended) return 0;
    /* The appended text is the file's, not an edit. */
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    set_dirty(0);
    record_document_file_time(g_documentStore.active);
    update_preview();
    fire_plugin_event("reload", g_currentFile);
    return 1;
}

/* The active tab now holds `path`'s text: point the tab at the file and
   bring everything that depends on it up to date. A reload of the file
   already open leaves the recent-files list alone; reload_active_document()
   fires "reload" once it has put the view back. */
static void finish_file_load(const WCHAR *path, int reloading) {
    g_documentStore.documents[g_documentStore.active].viewKind = DOCUMENT_VIEW_TEXT;
    SendMessageA(g_hEdit, SCI_SETREADONLY, FALSE, 0);
    replace_wstring(&g_currentFile, path);
    apply_document_language(g_currentFile);
    restore_persisted_breakpoints(g_currentFile);
    ensure_large_file_editor_visible();
    /* An ordinary load's SCN_MODIFIED schedules this; a large-file load
       raises none, so clear the outline the old text built. */
    if (g_largeFile) update_outline();
    record_document_file_time(g_documentStore.active);
    update_document_mapping();
    update_preview();
    set_dirty(0);
    if (!reloading) {
        recent_list_add(&g_recentFileList, path);
        refresh_menu(g_hMainWnd);
    }
    fire_plugin_event("open", g_currentFile);
}

static void report_load_failure(LoadResult result) {
    /* A cancelled large-file read is an expected outcome, not an error. */
    if (result == LOAD_CANCELLED) return;
    const WCHAR *message =
        result == LOAD_OUT_OF_MEMORY ? L"Not enough memory to open this file."
        : result == LOAD_TOO_LARGE ? L"This file is too large to open as text below the large-file "
                                     L"threshold. Lower View.LargeFileThresholdMB in settings.json "
                                     L"to open it in large-file mode."
        : L"Could not read file.";
    show_error(message);
}

/* `forcedEncoding` is a TextEncoding to read the file as (File > Reopen
   with Encoding), or -1 to detect it. `reloading` is set when the file is
   the one already in the tab (see finish_file_load()). */
static int load_file_into_tab(const WCHAR *path, int forcedEncoding, int reloading) {
    WCHAR *ioPath = extended_path(path);
    HANDLE file = ioPath ? CreateFileW(ioPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL)
        : INVALID_HANDLE_VALUE;
    free(ioPath);
    if (file == INVALID_HANDLE_VALUE) {
        show_error(L"Could not open file.");
        recent_list_remove(&g_recentFileList, path);
        refresh_menu(g_hMainWnd);
        return 0;
    }
    LARGE_INTEGER size;
    LoadResult result = LOAD_READ_FAILED;
    if (GetFileSizeEx(file, &size) && size.QuadPart >= 0) {
        /* Past INT_MAX a file has to open in large-file mode whatever the
           threshold is set to: only a large-file document has 64-bit positions. */
        int largeFile = size.QuadPart >= large_file_threshold_bytes() ||
                        size.QuadPart > INT_MAX;
        result = largeFile ? load_large_file(file, (size_t)size.QuadPart, forcedEncoding)
                           : load_ordinary_file(file, (size_t)size.QuadPart, forcedEncoding);
        if (result == LOAD_OK) record_loaded_file_end(file, size.QuadPart);
    }
    CloseHandle(file);
    if (result != LOAD_OK) {
        report_load_failure(result);
        return 0;
    }
    WCHAR *displayPath = absolute_path(path);
    finish_file_load(displayPath ? displayPath : path, reloading);
    free(displayPath);
    return 1;
}

static int load_file_as(const WCHAR *path, int forcedEncoding) {
    return load_file_into_tab(path, forcedEncoding, 0);
}

static int load_file(const WCHAR *path) {
    return load_file_as(path, -1);
}

/* Reads the active document's file again after another program changed it,
   keeping the selection and the scroll position where the new text allows,
   then fires "reload" - after the view is back, so a handler can move it. */
static int reload_active_document(void) {
    Document *doc = &g_documentStore.documents[g_documentStore.active];
    if (doc->viewKind == DOCUMENT_VIEW_IMAGE) {
        record_document_file_time(g_documentStore.active);
        update_preview();
        fire_plugin_event("reload", g_currentFile);
        return 1;
    }
    WCHAR *path = doc->path ? wcsdup_heap(doc->path) : NULL;
    if (!path) return 0;
    Sci_Position caret = (Sci_Position)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position anchor = (Sci_Position)SendMessageA(g_hEdit, SCI_GETANCHOR, 0, 0);
    Sci_Position firstLine = (Sci_Position)SendMessageA(g_hEdit, SCI_GETFIRSTVISIBLELINE, 0, 0);
    int loaded = load_file_into_tab(path, doc->encodingForced ? doc->encoding : -1, 1);
    free(path);
    if (!loaded) return 0;
    Sci_Position length = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)(anchor < length ? anchor : length),
                 (LPARAM)(caret < length ? caret : length));
    SendMessageA(g_hEdit, SCI_SETFIRSTVISIBLELINE, (WPARAM)firstLine, 0);
    fire_plugin_event("reload", g_currentFile);
    return 1;
}

/* SCN_MODIFIED is delivered from inside Scintilla's edit operation, so the
   notification only flips the cheap per-document flag and posts this work.
   Reconfiguring lexers/views after control returns avoids replacing view
   state re-entrantly while preserving the ordinary document's undo history. */
static void apply_large_file_mode_after_growth(int documentIndex) {
    if (documentIndex < 0 || documentIndex >= g_documentStore.count ||
        documentIndex != g_documentStore.active || !g_largeFile) return;
    KillTimer(g_hMainWnd, IDT_PREVIEW_UPDATE);
    KillTimer(g_hMainWnd, IDT_SPELL_CHECK);
    KillTimer(g_hMainWnd, IDT_OUTLINE_UPDATE);
    apply_document_language(g_currentFile);
    ensure_large_file_editor_visible();
    update_outline();
    update_status_bar();
    if (g_previewVisible) update_preview();
    /* Remove any now-stale recovery copy made while the document was still
       below the threshold; large-file documents are intentionally excluded. */
    write_recovery_snapshot();
    refresh_menu(g_hMainWnd);
}

/* File > Reopen with Encoding: rereads the file from disk as `encoding`,
   for when detection guessed wrong. */
static void reopen_with_encoding(TextEncoding encoding) {
    if (!g_currentFile) {
        MessageBoxW(g_hMainWnd, L"Save the document first - there's no file to reopen yet.",
                    L"Tinker Editor", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_dirty && MessageBoxW(g_hMainWnd, L"Reopening discards your unsaved changes. Continue?",
                               L"Tinker Editor", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    WCHAR *path = wcsdup_heap(g_currentFile);
    if (path) load_file_as(path, encoding);
    free(path);
}

/* Changes how the next save stores the active document. The plugin API uses
   this primitive; conversion commands themselves live in plugins/. */
static void set_document_encoding(TextEncoding encoding, int hasBom) {
    if (g_encoding == (int)encoding && g_hasBom == hasBom) return;
    g_encoding = encoding;
    g_hasBom = hasBom;
    set_dirty(1);
}

static int active_document_is_reusable(void) {
    return !g_currentFile && !g_dirty &&
           !g_documentStore.documents[g_documentStore.active].pinned &&
           (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0) == 0;
}

/* Opens an image as a lightweight, read-only document. Its Scintilla document
   stays empty and hidden; the preview loads the file through document.local. */
static int load_image_into_tab(const WCHAR *path) {
    WCHAR *displayPath = absolute_path(path);
    const WCHAR *filePath = displayPath ? displayPath : path;
    WCHAR *ioPath = extended_path(filePath);
    DWORD attributes = GetFileAttributesW(ioPath ? ioPath : filePath);
    free(ioPath);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        free(displayPath);
        show_error(L"Could not open image file.");
        return 0;
    }
    replace_wstring(&g_currentFile, filePath);
    free(displayPath);
    Document *doc = &g_documentStore.documents[g_documentStore.active];
    doc->viewKind = DOCUMENT_VIEW_IMAGE;
    doc->encodingForced = 0;
    doc->largeFile = 0;
    SendMessageA(g_hEdit, SCI_CLEARALL, 0, 0);
    reset_undo_history(g_hEdit);
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    SendMessageA(g_hEdit, SCI_SETREADONLY, TRUE, 0);
    apply_preview_host_theme();
    apply_webview_background_colour();
    apply_document_language(g_currentFile);
    record_document_file_time(g_documentStore.active);
    update_document_mapping();
    recent_list_add(&g_recentFileList, g_currentFile);
    set_dirty(0);
    update_tab_label(g_documentStore.active);
    update_preview();
    layout(g_hMainWnd);
    update_status_bar();
    update_outline();
    refresh_menu(g_hMainWnd);
    fire_plugin_event("open", g_currentFile);
    return 1;
}

static int open_file_in_tab(const WCHAR *path) {
    int imageFile = image_file_path(path);
    WCHAR *full = absolute_path(path);
    if (full) {
        for (int i = 0; i < g_documentStore.count; i++) {
            if (g_documentStore.documents[i].path && _wcsicmp(g_documentStore.documents[i].path, full) == 0) {
                switch_document(i);
                free(full);
                return 1;
            }
        }
    }
    free(full);
    int created = 0;
    if (!active_document_is_reusable()) {
        /* Loading an image immediately replaces every visible property of the
           new tab. Avoid rendering and laying out an intermediate Untitled
           text tab, especially when that would navigate back to the shell. */
        if (!create_document_tab_internal(imageFile)) return 0;
        created = 1;
    }
    if (imageFile ? load_image_into_tab(path) : load_file(path)) return 1;
    if (created && g_documentStore.count > 1) {
        sptr_t failedDoc = g_documentStore.documents[g_documentStore.active].sciDocument;
        int removeIndex = g_documentStore.active;
        int target = removeIndex > 0 ? removeIndex - 1 : 0;
        TabCtrl_DeleteItem(g_hTabs, removeIndex);
        g_documentStore.count--;
        g_documentStore.active = target;
        set_active_editor_document(g_documentStore.documents[target].sciDocument);
        apply_editor_font();
        apply_document_language(g_currentFile);
        restore_document_view(g_hEdit, &g_documentStore.documents[target]);
        SendMessageA(g_hEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)failedDoc);
        TabCtrl_SetCurSel(g_hTabs, target);
        update_document_mapping();
        update_preview();
        layout(g_hMainWnd);
        update_title();
    }
    return 0;
}

static void save_session_documents(void) {
    if (g_commandLinePreviewMode) return;
    WCHAR *settings = get_settings_path();
    if (!settings) return;
    int savedCount = 0;
    for (int i = 0; i < g_documentStore.count; i++) {
        if (!g_documentStore.documents[i].path) continue;
        WCHAR key[24], pinnedKey[24];
        swprintf(key, sizeof(key) / sizeof(key[0]), L"File%d", savedCount++);
        write_setting_string(L"Session", key, g_documentStore.documents[i].path, settings);
        swprintf(pinnedKey, sizeof(pinnedKey) / sizeof(pinnedKey[0]), L"Pinned%d", savedCount - 1);
        write_setting_string(L"Session", pinnedKey,
                             g_documentStore.documents[i].pinned ? L"1" : L"0", settings);
    }
    for (int i = savedCount; i < MAX_DOCUMENTS; i++) {
        WCHAR key[24], pinnedKey[24];
        swprintf(key, sizeof(key) / sizeof(key[0]), L"File%d", i);
        write_setting_string(L"Session", key, NULL, settings);
        swprintf(pinnedKey, sizeof(pinnedKey) / sizeof(pinnedKey[0]), L"Pinned%d", i);
        write_setting_string(L"Session", pinnedKey, NULL, settings);
    }
    WCHAR count[24];
    swprintf(count, sizeof(count) / sizeof(count[0]), L"%d", savedCount);
    write_setting_string(L"Session", L"Count", count, settings);
    write_setting_string(L"Session", L"ActiveFile",
                         g_currentFile ? g_currentFile : L"", settings);
    int splitIndex = split_document_index();
    write_setting_string(L"Session", L"SplitFile",
        splitIndex >= 0 && g_documentStore.documents[splitIndex].path
            ? g_documentStore.documents[splitIndex].path : NULL, settings);
    free(settings);
}

static void restore_session_documents(void) {
    WCHAR *settings = get_settings_path();
    if (!settings) return;
    int count = read_setting_int(L"Session", L"Count", 0, settings);
    if (count < 0) count = 0;
    if (count > MAX_DOCUMENTS) count = MAX_DOCUMENTS;
    WCHAR *activeFile = read_profile_string(L"Session", L"ActiveFile", settings);
    WCHAR *splitFile = read_profile_string(L"Session", L"SplitFile", settings);
    for (int i = 0; i < count; i++) {
        WCHAR key[24], pinnedKey[24];
        swprintf(key, sizeof(key) / sizeof(key[0]), L"File%d", i);
        swprintf(pinnedKey, sizeof(pinnedKey) / sizeof(pinnedKey[0]), L"Pinned%d", i);
        WCHAR *file = read_profile_string(L"Session", key, settings);
        int pinned = read_setting_int(L"Session", pinnedKey, 0, settings) != 0;
        if (file && open_file_in_tab(file)) {
            g_documentStore.documents[g_documentStore.active].pinned = pinned;
            update_tab_label(g_documentStore.active);
        }
        free(file);
    }
    if (activeFile) {
        for (int i = 0; i < g_documentStore.count; i++) {
            if (g_documentStore.documents[i].path && _wcsicmp(g_documentStore.documents[i].path, activeFile) == 0) {
                switch_document(i);
                break;
            }
        }
    }
    if (splitFile) {
        for (int i = 0; i < g_documentStore.count; i++) {
            Document *doc = &g_documentStore.documents[i];
            if (doc->path && !doc->largeFile && doc->viewKind == DOCUMENT_VIEW_TEXT &&
                _wcsicmp(doc->path, splitFile) == 0) {
                g_splitDocument = doc->sciDocument;
                sync_split_editor_document();
                apply_document_language(g_currentFile);
                update_tab_label(i);
                break;
            }
        }
    }
    free(splitFile);
    free(activeFile);
    free(settings);
}

/* Prompt used when closing a dirty tab or exiting with dirty documents.
   Returns 0 if the caller should abort. */
static int confirm_discard_if_dirty(void) {
    if (!g_dirty) return 1;
    int r = MessageBoxW(g_hMainWnd, L"Save changes to the current document?", L"Tinker Editor", MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == IDCANCEL) return 0;
    if (r == IDYES) {
        if (!do_save()) return 0;
    }
    return 1;
}

static void do_open_recent(int index) {
    if (index < 0 || index >= g_recentFileCount) return;
    WCHAR *path = wcsdup_heap(g_recentFiles[index]);
    if (path) open_file_in_tab(path); /* opening may mutate the recent-files array */
    free(path);
}

static void do_open(void) {
    DWORD cap = LONG_PATH_CAPACITY;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"Markdown Files (*.md;*.markdown)\0*.md;*.markdown\0Python Files (*.py;*.pyw)\0*.py;*.pyw\0C/C++ Files (*.c;*.cc;*.cpp;*.cxx)\0*.c;*.cc;*.cpp;*.cxx\0Text Files (*.txt)\0*.txt\0Image Files (*.png;*.jpg;*.gif;*.webp;*.svg)\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.svg;*.ico;*.avif\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 6; /* "All Files (*.*)" - the dialog otherwise defaults to entry 1 */
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    ofn.lpstrDefExt = L"md";
    if (GetOpenFileNameW(&ofn)) {
        WCHAR *next = path + wcslen(path) + 1;
        if (!*next) {
            open_file_in_tab(path);
        } else {
            while (*next) {
                WCHAR *full = path_join(path, next);
                if (full) open_file_in_tab(full);
                free(full);
                next += wcslen(next) + 1;
            }
        }
    }
    free(path);
}

/* Inserts the contents of a chosen file at the current caret position in the
   active editor, replacing any selection - the same semantics SCI_REPLACESEL
   gives every other insert command in this file. */
static void do_insert_file(void) {
    if (!g_hEdit) return;
    DWORD cap = LONG_PATH_CAPACITY;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"Markdown Files (*.md;*.markdown)\0*.md;*.markdown\0Python Files (*.py;*.pyw)\0*.py;*.pyw\0C/C++ Files (*.c;*.cc;*.cpp;*.cxx)\0*.c;*.cc;*.cpp;*.cxx\0Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 5; /* "All Files (*.*)" - the dialog otherwise defaults to entry 1 */
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrDefExt = L"md";
    if (GetOpenFileNameW(&ofn)) {
        char *text = read_text_file(path, NULL, NULL, NULL);
        if (text) {
            SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
            SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)text);
            SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
            SetFocus(g_hEdit);
        } else {
            show_error(L"Could not read the selected file.");
        }
        free(text);
    }
    free(path);
}

static int is_image_file(const WCHAR *path) {
    return image_file_path(path);
}

/* Create a collision-safe destination in an "images" folder next to the
   current Markdown file. The returned relative path always uses URL-style
   slashes so it can be inserted directly into Markdown. */
static int create_image_destination(const WCHAR *preferredName, const WCHAR *defaultExtension,
                                    WCHAR **destinationOut, WCHAR **relativeOut) {
    *destinationOut = NULL;
    *relativeOut = NULL;
    if (!g_currentFile) {
        if (!do_save()) return 0;
    }
    WCHAR *documentDirectory = path_directory(g_currentFile);
    WCHAR *imagesDirectory = documentDirectory ? path_join(documentDirectory, L"images") : NULL;
    free(documentDirectory);
    if (!imagesDirectory) return 0;
    WCHAR *ioDirectory = extended_path(imagesDirectory);
    BOOL made = CreateDirectoryW(ioDirectory ? ioDirectory : imagesDirectory, NULL);
    DWORD directoryError = made ? ERROR_SUCCESS : GetLastError();
    free(ioDirectory);
    if (!made && directoryError != ERROR_ALREADY_EXISTS) {
        free(imagesDirectory);
        show_error(L"Could not create the document's images folder.");
        return 0;
    }

    const WCHAR *leaf = preferredName && preferredName[0] ? PathFindFileNameW(preferredName) : L"image";
    size_t leafLength = wcslen(leaf);
    WCHAR *safe = (WCHAR *)malloc((leafLength + 16) * sizeof(WCHAR));
    if (!safe) { free(imagesDirectory); return 0; }
    size_t o = 0;
    for (size_t i = 0; i < leafLength; i++) {
        WCHAR c = leaf[i];
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
        safe[o++] = c;
    }
    safe[o] = 0;
    WCHAR *extension = PathFindExtensionW(safe);
    if (!extension || !extension[0]) {
        wcscat(safe, defaultExtension ? defaultExtension : L".png");
        extension = PathFindExtensionW(safe);
    }
    size_t extensionOffset = (size_t)(extension - safe);
    WCHAR *extensionCopy = wcsdup_heap(extension);
    safe[extensionOffset] = 0;
    if (!safe[0]) wcscpy(safe, L"image");

    enum { MAX_NAME_ATTEMPTS = 10000 }; /* image.png, image-1.png, ... */
    for (unsigned suffix = 0; suffix < MAX_NAME_ATTEMPTS; suffix++) {
        size_t nameCap = wcslen(safe) + (extensionCopy ? wcslen(extensionCopy) : 0) + 32;
        WCHAR *name = (WCHAR *)malloc(nameCap * sizeof(WCHAR));
        if (!name) break;
        if (suffix == 0) swprintf(name, nameCap, L"%ls%ls", safe, extensionCopy ? extensionCopy : L"");
        else swprintf(name, nameCap, L"%ls-%u%ls", safe, suffix, extensionCopy ? extensionCopy : L"");
        WCHAR *candidate = path_join(imagesDirectory, name);
        WCHAR *ioCandidate = candidate ? extended_path(candidate) : NULL;
        DWORD attrs = candidate ? GetFileAttributesW(ioCandidate ? ioCandidate : candidate) : INVALID_FILE_ATTRIBUTES;
        free(ioCandidate);
        if (candidate && attrs == INVALID_FILE_ATTRIBUTES) {
            size_t relativeCap = wcslen(name) + 8;
            WCHAR *relative = (WCHAR *)malloc(relativeCap * sizeof(WCHAR));
            if (relative) {
                swprintf(relative, relativeCap, L"images/%ls", name);
                *destinationOut = candidate;
                *relativeOut = relative;
                free(name);
                free(extensionCopy);
                free(safe);
                free(imagesDirectory);
                return 1;
            }
        }
        free(candidate);
        free(name);
    }
    free(extensionCopy);
    free(safe);
    free(imagesDirectory);
    return 0;
}

static int insert_markdown_reference(const WCHAR *relativePath, const WCHAR *altText, int isImage) {
    char *relative = to_utf8(relativePath);
    char *alt = to_utf8(altText && altText[0] ? altText : (isImage ? L"image" : L"file"));
    if (!relative || !alt) { free(relative); free(alt); return 0; }
    size_t escapedCap = strlen(alt) * 2 + 1;
    char *escapedAlt = (char *)malloc(escapedCap);
    if (!escapedAlt) { free(relative); free(alt); return 0; }
    size_t o = 0;
    for (size_t i = 0; alt[i]; i++) {
        if (alt[i] == '\\' || alt[i] == '[' || alt[i] == ']') escapedAlt[o++] = '\\';
        escapedAlt[o++] = alt[i];
    }
    escapedAlt[o] = 0;
    size_t cap = o + strlen(relative) + 16;
    char *markdown = (char *)malloc(cap);
    int inserted = markdown != NULL;
    if (markdown) {
        snprintf(markdown, cap, "%s[%s](<%s>)", isImage ? "!" : "", escapedAlt, relative);
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)markdown);
        SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
    }
    free(markdown);
    free(escapedAlt);
    free(relative);
    free(alt);
    return inserted;
}

static int insert_markdown_image(const WCHAR *relativePath, const WCHAR *altText) {
    return insert_markdown_reference(relativePath, altText, 1);
}

/* Writes `target`'s path relative to the folder holding `sourceFile` into
   `relative` (PathRelativePathToW()'s MAX_PATH-bounded buffer). Returns 0
   when there's no relative path, e.g. the two are on different drives. */
static int relative_path_from_file(const WCHAR *sourceFile, const WCHAR *target,
                                   int targetIsDirectory, WCHAR relative[MAX_PATH]) {
    WCHAR *directory = path_directory(sourceFile);
    int ok = directory && PathRelativePathToW(relative, directory, FILE_ATTRIBUTE_DIRECTORY, target,
                                              targetIsDirectory ? FILE_ATTRIBUTE_DIRECTORY
                                                                : FILE_ATTRIBUTE_NORMAL);
    free(directory);
    return ok;
}

/* A heap copy of `path` written the way Markdown links spell it: forward
   slashes, with no leading "./". Sized off `path` itself, since an absolute
   fallback (from DragQueryFileW(), say) isn't MAX_PATH-limited. */
static WCHAR *markdown_link_path(const WCHAR *path) {
    if (path[0] == L'.' && (path[1] == L'\\' || path[1] == L'/')) path += 2;
    WCHAR *link = wcsdup_heap(path);
    if (link)
        for (WCHAR *character = link; *character; character++)
            if (*character == L'\\') *character = L'/';
    return link;
}

/* Links to a dropped file where it already sits on disk (relative to the
   current document's directory) instead of copying it into images/ - used
   for Shift+drop, which is meant for "point at this file" rather than
   "import this image into the document". Images still get an image link;
   everything else gets a plain link. */
static int shift_drop_link(const WCHAR *path) {
    /* Without a current document, or across drives, the link falls back to
       the dropped file's absolute path. */
    WCHAR relative[MAX_PATH];
    const WCHAR *target = g_currentFile && relative_path_from_file(g_currentFile, path, 0, relative)
        ? relative : path;
    WCHAR *normalized = markdown_link_path(target);
    if (!normalized) return 0;

    WCHAR *alt = wcsdup_heap(PathFindFileNameW(path));
    if (alt) PathRemoveExtensionW(alt);
    int inserted = insert_markdown_reference(normalized, alt, is_image_file(path));
    free(alt);
    free(normalized);
    return inserted;
}

static int import_image_file(const WCHAR *source) {
    WCHAR *destination = NULL, *relative = NULL;
    if (!create_image_destination(source, PathFindExtensionW(source), &destination, &relative)) return 0;
    WCHAR *ioSource = extended_path(source);
    WCHAR *ioDestination = extended_path(destination);
    BOOL copied = CopyFileW(ioSource ? ioSource : source,
                            ioDestination ? ioDestination : destination, TRUE);
    free(ioSource);
    free(ioDestination);
    if (!copied) {
        show_error(L"Could not copy the dropped image into the document's images folder.");
        free(destination);
        free(relative);
        return 0;
    }
    WCHAR *alt = wcsdup_heap(PathFindFileNameW(source));
    if (alt) PathRemoveExtensionW(alt);
    int inserted = insert_markdown_image(relative, alt ? alt : L"image");
    free(alt);
    free(destination);
    free(relative);
    return inserted;
}

static int save_hbitmap_as_png(HBITMAP bitmap, const WCHAR *path) {
    IWICImagingFactory *factory = NULL;
    IWICBitmap *source = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *properties = NULL;
    UINT width = 0, height = 0;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void **)&factory);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateBitmapFromHBITMAP(
        factory, bitmap, NULL, WICBitmapIgnoreAlpha, &source);
    if (SUCCEEDED(hr)) hr = IWICBitmap_GetSize(source, &width, &height);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(factory, &stream);
    WCHAR *ioPath = extended_path(path);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(stream, ioPath ? ioPath : path, GENERIC_WRITE);
    free(ioPath);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &properties);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(frame, properties);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(frame, width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WriteSource(frame, (IWICBitmapSource *)source, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(encoder);
    if (properties) IPropertyBag2_Release(properties);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    if (source) IWICBitmap_Release(source);
    if (factory) IWICImagingFactory_Release(factory);
    return SUCCEEDED(hr);
}

static int paste_image_from_clipboard(void) {
    if (!IsClipboardFormatAvailable(CF_BITMAP)) return 0;
    /* Rich sources (browsers, Word, Excel) offer text alongside a rendered
       bitmap. Text is what the user means to paste into Markdown source. */
    if (IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT)) return 0;
    if (!g_currentFile) {
        if (!do_save()) return 1;
    }
    if (!OpenClipboard(g_hMainWnd)) return 1;
    HBITMAP bitmap = (HBITMAP)GetClipboardData(CF_BITMAP);
    if (!bitmap) { CloseClipboard(); return 1; }
    SYSTEMTIME now;
    GetLocalTime(&now);
    WCHAR name[64];
    swprintf(name, sizeof(name) / sizeof(name[0]), L"image-%04u%02u%02u-%02u%02u%02u.png",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    WCHAR *destination = NULL, *relative = NULL;
    int made = create_image_destination(name, L".png", &destination, &relative);
    int saved = made && save_hbitmap_as_png(bitmap, destination);
    CloseClipboard();
    if (!saved) {
        show_error(L"Could not save the clipboard image.");
        free(destination);
        free(relative);
        return 1;
    }
    insert_markdown_image(relative, L"image");
    free(destination);
    free(relative);
    return 1;
}

static void handle_dropfiles(HDROP hDrop) {
    /* Held at message-processing time, which in practice still reflects the
       physical key held through the drop gesture - not a hard real-time
       guarantee, but Shift-drop is a deliberate two-handed action so the
       window is wide enough to be reliable. */
    int shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
    int insertedImage = 0;
    int undoActive = 0;
    for (UINT i = 0; i < count; i++) {
        UINT cap = DragQueryFileW(hDrop, i, NULL, 0) + 1;
        WCHAR *path = (WCHAR *)malloc((size_t)cap * sizeof(WCHAR));
        if (path && DragQueryFileW(hDrop, i, path, cap)) {
            if (editor_pane_visible() && shiftHeld) {
                if (!undoActive) {
                    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
                    undoActive = 1;
                }
                if (insertedImage) SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)editor_eol_sequence());
                if (shift_drop_link(path)) insertedImage = 1;
            } else if (editor_pane_visible() && is_image_file(path)) {
                if (!undoActive) {
                    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
                    undoActive = 1;
                }
                if (insertedImage) SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)editor_eol_sequence());
                if (import_image_file(path)) insertedImage = 1;
            } else {
                if (undoActive) {
                    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
                    undoActive = 0;
                }
                open_file_in_tab(path);
                insertedImage = 0;
            }
        }
        free(path);
    }
    if (undoActive) SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
    DragFinish(hDrop);
}

/* Some of the text can't be stored in `*encoding`: asks whether to save as
   UTF-8 instead (which updates *encoding and *hasBom) or as it is, with
   those characters replaced. Returns 0 if the user cancelled. */
static int confirm_lossy_encoding(TextEncoding *encoding, int *hasBom) {
    WCHAR message[400];
    if (*encoding == TEXT_ENCODING_ANSI)
        swprintf(message, sizeof(message) / sizeof(message[0]),
                 L"Some characters in this document can't be stored in ANSI (code page %u).\n\n"
                 L"Yes: save it as UTF-8 instead.\n"
                 L"No: save as ANSI anyway, replacing those characters with '?'.",
                 text_ansi_code_page());
    else
        swprintf(message, sizeof(message) / sizeof(message[0]),
                 L"This document contains bytes that aren't valid text - it was probably "
                 L"reopened with the wrong encoding - and they can't be stored in UTF-16.\n\n"
                 L"Yes: save it as UTF-8 instead, keeping those bytes as they are.\n"
                 L"No: save as UTF-16 anyway, replacing them with \x201C\xFFFD\x201D.");
    int answer = MessageBoxW(g_hMainWnd, message, L"Tinker Editor", MB_YESNOCANCEL | MB_ICONWARNING);
    if (answer == IDCANCEL) return 0;
    if (answer == IDYES) {
        *encoding = TEXT_ENCODING_UTF8;
        *hasBom = 0;
    }
    return 1;
}

/* The active document was just written to `path`: point the tab at it and
   bring everything that depends on the saved file up to date. */
static void finish_document_save(const WCHAR *path, TextEncoding encoding, int hasBom) {
    g_encoding = encoding;
    g_hasBom = hasBom;
    /* Record the path before replace_wstring(): when saving the current file,
       `path` may alias g_currentFile, which replace_wstring() can free. */
    recent_list_add(&g_recentFileList, path);
    replace_wstring(&g_currentFile, path);
    apply_document_language(g_currentFile);
    /* Save As changes the persistence key from the old path to the new one;
       write the current markers immediately instead of waiting for exit. */
    save_breakpoints_for_active_document(g_currentFile);
    forget_loaded_file_end(&g_documentStore.documents[g_documentStore.active]);
    record_document_file_time(g_documentStore.active);
    update_document_mapping();
    /* HTML's preview (see show_html_file_preview()) is a real navigation to
       the saved file, not a re-render of the live buffer like Markdown's -
       it only reflects this save's content once it's refreshed here. */
    if (g_activeLanguage == LANG_HTML) update_preview();
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    set_dirty(0);
    refresh_menu(g_hMainWnd);
}

/* save_to_path()'s write, once the save handlers have run. */
static int write_active_document(const WCHAR *path) {
    if (active_document_is_image()) {
        show_error(L"Image tabs are read-only.");
        return 0;
    }
    /* Written straight from Scintilla's own buffer: SCI_GETCHARACTERPOINTER
       closes the gap so the text is contiguous, and the pointer stays valid
       until the document is next modified. Only the encoding prompt below
       can let that happen (its message loop runs the file-watch timer), so
       the pointer is fetched again after it. A copy via get_edit_text()
       would need a second document-sized allocation, which a large-file
       document may not have room for. */
    Sci_Position len = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    const char *text = (const char *)SendMessageA(g_hEdit, SCI_GETCHARACTERPOINTER, 0, 0);
    if (!text) {
        show_error(L"Not enough memory to save the document.");
        return 0;
    }
    TextEncoding encoding = (TextEncoding)g_encoding;
    int hasBom = g_hasBom;
    LargeOperationProgress progress = {0};
    if (g_largeFile)
        large_operation_progress_begin(&progress, L"Saving large file", L"Checking text encoding...");
    int lossless = text_encoding_is_lossless_progress(text, (size_t)len, encoding,
        large_operation_progress_callback, &progress);
    if (lossless == -2) {
        large_operation_progress_end(&progress);
        return 0;
    }
    if (lossless < 0) {
        large_operation_progress_end(&progress);
        show_error(L"Not enough memory to save the document.");
        return 0;
    }
    if (!lossless) {
        large_operation_progress_end(&progress);
        if (!confirm_lossy_encoding(&encoding, &hasBom)) return 0;
        /* The prompt's message loop runs timers - an external-change reload
           can replace the text under it - so the buffer is fetched again. */
        len = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
        text = (const char *)SendMessageA(g_hEdit, SCI_GETCHARACTERPOINTER, 0, 0);
        if (!text) {
            show_error(L"Not enough memory to save the document.");
            return 0;
        }
        if (g_largeFile)
            large_operation_progress_begin(&progress, L"Saving large file", L"Writing text...");
    } else {
        large_operation_progress_set_line(&progress, L"Writing text...");
    }
    WCHAR *ioPath = extended_path(path);
    int ok = write_file_atomic_progress(ioPath ? ioPath : path, text, (size_t)len, encoding, hasBom,
        large_operation_progress_callback, &progress);
    free(ioPath);
    int cancelled = progress.cancelled;
    large_operation_progress_end(&progress);
    if (!ok) {
        if (!cancelled)
            show_error(L"Could not write the complete file; the original was left unchanged.");
        return 0;
    }
    WCHAR *displayPath = absolute_path(path);
    finish_document_save(displayPath ? displayPath : path, encoding, hasBom);
    free(displayPath);
    return 1;
}

static int save_to_path(const WCHAR *requestedPath) {
    /* The save handlers and the encoding prompt run message loops, where an
       external-change reload can replace (and free) g_currentFile - which
       requestedPath usually is - so the save works from a copy. */
    WCHAR *path = wcsdup_heap(requestedPath);
    if (!path) {
        show_error(L"Not enough memory to save the document.");
        return 0;
    }
    /* First, so whatever the handlers change is what gets saved. `save` is
       retained as the compatibility alias for before_save. */
    fire_plugin_event("before_save", path);
    fire_plugin_event("save", path);
    int ok = write_active_document(path);
    if (ok) {
        fire_plugin_event("after_save", path);
        refresh_git_status();
    }
    free(path);
    return ok;
}

static int do_saveas(void) {
    if (active_document_is_image()) {
        show_error(L"Image tabs are read-only.");
        return 0;
    }
    DWORD cap = LONG_PATH_CAPACITY;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return 0;
    if (g_currentFile) wcsncpy(path, g_currentFile, cap - 1);
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"Markdown Files (*.md)\0*.md\0Python Files (*.py;*.pyw)\0*.py;*.pyw\0C/C++ Files (*.c;*.cc;*.cpp;*.cxx)\0*.c;*.cc;*.cpp;*.cxx\0Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!g_currentFile && g_activeLanguage == LANG_TEXT) {
        /* An untitled document defaults to plain text now (see
           language_for_path()); default Save As to match instead of always
           offering "Untitled.md", which would contradict the plain-text tab
           label and preview the user has been shown all along. */
        ofn.nFilterIndex = 4; /* "Text Files (*.txt)", the 4th filter entry above */
        ofn.lpstrDefExt = L"txt";
    } else {
        ofn.lpstrDefExt = L"md";
    }
    int result = GetSaveFileNameW(&ofn) ? save_to_path(path) : 0;
    free(path);
    return result;
}

static int do_save(void) {
    if (active_document_is_image()) return 1;
    return g_currentFile ? save_to_path(g_currentFile) : do_saveas();
}

/* Saves every dirty tab in turn, the same switch-to-it-then-act pattern
   do_exit() uses to visit every document through the single shared editor.
   An untitled dirty document still prompts for a path via do_save()'s own
   Save As fallback, same as saving it individually would. Stop on the first
   cancellation or failure, and return to the originally selected tab. */
static int do_save_all(void) {
    int original = g_documentStore.active;
    int ok = 1;
    for (int i = 0; i < g_documentStore.count; i++) {
        if (!g_documentStore.documents[i].dirty) continue;
        switch_document(i);
        if (!do_save()) { ok = 0; break; }
    }
    if (original < g_documentStore.count) switch_document(original);
    return ok;
}

static void do_export_html(void) {
    if (!g_webview || !g_webviewReady) {
        MessageBoxW(g_hMainWnd, L"The preview is not ready yet.", L"Tinker Editor", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_exportPath) {
        MessageBoxW(g_hMainWnd, L"An HTML export is already in progress.", L"Tinker Editor", MB_OK | MB_ICONINFORMATION);
        return;
    }

    DWORD cap = LONG_PATH_CAPACITY;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    if (g_currentFile) {
        wcsncpy(path, g_currentFile, cap - 1);
        PathRenameExtensionW(path, L".html");
    } else {
        wcsncpy(path, L"Untitled.html", cap - 1);
    }
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"HTML Files (*.html;*.htm)\0*.html;*.htm\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"html";
    if (!GetSaveFileNameW(&ofn)) { free(path); return; }

    WCHAR *directory = document_or_module_directory();
    WCHAR *directorySlash = directory ? path_join(directory, L"") : NULL; /* adds the trailing '\' */
    WCHAR url[LONG_PATH_CAPACITY];
    DWORD urlLength = sizeof(url) / sizeof(url[0]);
    if (!directorySlash || FAILED(UrlCreateFromPathW(directorySlash, url, &urlLength, 0))) wcscpy(url, L"");
    free(directorySlash);
    free(directory);

    const WCHAR *titleWide = document_short_name(&g_documentStore.documents[g_documentStore.active]);
    if (!titleWide) titleWide = L"Untitled";
    char *baseUtf8 = to_utf8(url);
    char *titleUtf8 = to_utf8(titleWide);
    char *baseEscaped = baseUtf8 ? json_escape(baseUtf8) : NULL;
    char *titleEscaped = titleUtf8 ? json_escape(titleUtf8) : NULL;
    if (!baseEscaped || !titleEscaped) {
        free(path); free(baseUtf8); free(titleUtf8); free(baseEscaped); free(titleEscaped);
        return;
    }
    size_t scriptCap = strlen(baseEscaped) + strlen(titleEscaped) + 512;
    char *script = (char *)malloc(scriptCap);
    if (!script) {
        free(path); free(baseUtf8); free(titleUtf8); free(baseEscaped); free(titleEscaped);
        return;
    }
    g_exportRequest++;
    snprintf(script, scriptCap,
        "(()=>{if(!window.exportRenderedHTML)return false;"
        "Promise.resolve().then(()=>window.exportRenderedHTML('%s','%s'))"
        ".then(html=>chrome.webview.postMessage('export-html:%lu:'+JSON.stringify(html)))"
        ".catch(()=>chrome.webview.postMessage('export-html:%lu:null'));return true;})()",
        baseEscaped, titleEscaped, g_exportRequest, g_exportRequest);
    g_exportPath = path;
    SetTimer(g_hMainWnd, IDT_HTML_EXPORT, HTML_EXPORT_TIMEOUT_MS, NULL);
    update_preview();
    HRESULT hr = execute_webview_script(script, &g_exportScriptHandler);
    if (FAILED(hr)) {
        KillTimer(g_hMainWnd, IDT_HTML_EXPORT);
        free(g_exportPath);
        g_exportPath = NULL;
        show_error(L"Could not start the HTML export.");
    }
    free(script); free(baseUtf8); free(titleUtf8); free(baseEscaped); free(titleEscaped);
}

/* File > Export as Text.../RTF...: renders the active document's Markdown
   source directly (markdown_export.c), unlike Export Rendered HTML/PDF above,
   which goes through the live preview's webview. No preview is required. */
static void do_export_as(const WCHAR *extension, const WCHAR *filterName, char *(*render)(const char *)) {
    DWORD cap = LONG_PATH_CAPACITY;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    if (g_currentFile) {
        wcsncpy(path, g_currentFile, cap - 1);
        PathRenameExtensionW(path, extension);
    } else {
        swprintf(path, cap, L"Untitled%ls", extension);
    }

    /* OPENFILENAMEW wants a double-NUL-terminated list of NUL-separated
       strings; each swprintf below stops at the NUL it writes, so the next
       segment is written just past it, same shape as the literal filter
       strings used elsewhere (e.g. do_export_html's L"HTML Files...\0..."). */
    WCHAR filter[160];
    int filterPos = swprintf(filter, 160, L"%ls Files (*%ls)", filterName, extension) + 1;
    filterPos += swprintf(filter + filterPos, 160 - filterPos, L"*%ls", extension) + 1;
    filterPos += swprintf(filter + filterPos, 160 - filterPos, L"All Files (*.*)") + 1;
    filterPos += swprintf(filter + filterPos, 160 - filterPos, L"*.*") + 1;
    filter[filterPos] = L'\0';

    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = extension + 1;
    if (!GetSaveFileNameW(&ofn)) { free(path); return; }

    Sci_Position length = 0;
    char *text = get_document_text(g_documentStore.active, &length);
    char *rendered = render(text ? text : "");
    free(text);
    if (!rendered) { free(path); show_error(L"Could not render the document for export."); return; }

    WCHAR *ioPath = extended_path(path);
    int ok = write_bytes_file(ioPath ? ioPath : path, rendered, strlen(rendered));
    free(ioPath);
    free(rendered);
    free(path);
    if (!ok) show_error(L"Could not write the export file.");
}

static void do_export_text(void) { do_export_as(L".txt", L"Text", markdown_to_plaintext); }
static void do_export_rtf(void) { do_export_as(L".rtf", L"Rich Text", markdown_to_rtf); }

static const char *builtin_script(int id, size_t *length); /* plugin_bridge.inc */

/* File > Export as Word (.docx): unlike do_export_as() above, a .docx is a
   ZIP of XML parts, not one text buffer, so this builds those parts
   (markdown_export.c) and hands them to docx_zip_build() (src/docx_zip.c),
   which runs src/lua/ziptool.lua in its own dedicated Lua state to do the
   actual ZIP packaging - see that file's header comment for why. */
static void do_export_docx(void) {
    DWORD cap = LONG_PATH_CAPACITY;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    if (g_currentFile) {
        wcsncpy(path, g_currentFile, cap - 1);
        PathRenameExtensionW(path, L".docx");
    } else {
        wcsncpy(path, L"Untitled.docx", cap - 1);
    }
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"Word Documents (*.docx)\0*.docx\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"docx";
    if (!GetSaveFileNameW(&ofn)) { free(path); return; }

    Sci_Position length = 0;
    char *text = get_document_text(g_documentStore.active, &length);
    char *documentXml = markdown_to_docx_document_xml(text ? text : "");
    free(text);
    if (!documentXml) { free(path); show_error(L"Could not render the document for export."); return; }

    size_t fixedCount = 0;
    const DocxFixedPart *fixedParts = docx_export_fixed_parts(&fixedCount);
    DocxZipPart *parts = (DocxZipPart *)malloc((fixedCount + 1) * sizeof(DocxZipPart));
    if (!parts) { free(documentXml); free(path); return; }
    for (size_t i = 0; i < fixedCount; i++)
        parts[i] = (DocxZipPart){ fixedParts[i].name, fixedParts[i].content, strlen(fixedParts[i].content) };
    parts[fixedCount] = (DocxZipPart){ "word/document.xml", documentXml, strlen(documentXml) };

    size_t ziptoolLength = 0;
    const char *ziptoolSource = builtin_script(IDR_ZIPTOOL_LUA, &ziptoolLength);
    char *zipBytes = NULL;
    size_t zipLength = 0;
    char *error = ziptoolSource
        ? docx_zip_build(ziptoolSource, ziptoolLength, parts, fixedCount + 1, &zipBytes, &zipLength)
        : _strdup("The .docx packaging script is missing from tinkeredit.exe.");
    free(parts);
    free(documentXml);

    if (!error) {
        WCHAR *ioPath = extended_path(path);
        int ok = write_bytes_file(ioPath ? ioPath : path, zipBytes, zipLength);
        free(ioPath);
        free(zipBytes);
        if (!ok) show_error(L"Could not write the export file.");
    } else {
        size_t messageCapacity = strlen(error) + 64;
        char *combined = (char *)malloc(messageCapacity);
        if (combined) {
            snprintf(combined, messageCapacity, "Could not build the .docx file: %s", error);
            WCHAR *wide = to_wide(combined);
            if (wide) { show_error(wide); free(wide); }
            free(combined);
        }
        free(error);
    }
    free(path);
}

static void prepare_rendered_print(int exportPdf) {
    if (!g_webview || !g_webviewReady) {
        MessageBoxW(g_hMainWnd, L"The rendered preview is not ready yet.", L"Tinker Editor",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_pendingPrintAction != PRINT_ACTION_NONE) {
        MessageBoxW(g_hMainWnd, L"A print or PDF operation is already in progress.", L"Tinker Editor",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (exportPdf) {
        DWORD capacity = LONG_PATH_CAPACITY;
        WCHAR *path = (WCHAR *)calloc(capacity, sizeof(WCHAR));
        if (!path) return;
        if (g_currentFile) {
            wcsncpy(path, g_currentFile, capacity - 1);
            PathRenameExtensionW(path, L".pdf");
        } else wcscpy(path, L"Untitled.pdf");
        OPENFILENAMEW dialog = {0};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = g_hMainWnd;
        dialog.lpstrFilter = L"PDF Documents (*.pdf)\0*.pdf\0All Files (*.*)\0*.*\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = capacity;
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        dialog.lpstrDefExt = L"pdf";
        if (!GetSaveFileNameW(&dialog)) { free(path); return; }
        g_pendingPdfPath = path;
        g_pendingPrintAction = PRINT_ACTION_EXPORT_PDF;
    } else {
        g_pendingPrintAction = PRINT_ACTION_DIALOG;
    }
    if (!g_previewVisible) {
        g_previewVisible = 1;
        update_view_menu(g_hMainWnd);
        layout(g_hMainWnd);
    }
    update_preview();
    SetTimer(g_hMainWnd, IDT_PRINT_PREVIEW, PRINT_PREVIEW_DELAY_MS, NULL);
}

static void run_pending_print(void) {
    KillTimer(g_hMainWnd, IDT_PRINT_PREVIEW);
    if (!g_webview || g_pendingPrintAction == PRINT_ACTION_NONE) return;
    if (g_pendingPrintAction == PRINT_ACTION_DIALOG) {
        ICoreWebView2_16 *printWebView = NULL;
        if (SUCCEEDED(ICoreWebView2_QueryInterface(g_webview, &IID_ICoreWebView2_16,
                                                    (void **)&printWebView)) && printWebView) {
            HRESULT result = ICoreWebView2_16_ShowPrintUI(printWebView,
                COREWEBVIEW2_PRINT_DIALOG_KIND_SYSTEM);
            ICoreWebView2_16_Release(printWebView);
            if (FAILED(result))
                show_error(L"Could not open the print dialog.");
        } else {
            show_error(L"This WebView2 Runtime does not support printing.");
        }
        g_pendingPrintAction = PRINT_ACTION_NONE;
    } else if (g_pendingPrintAction == PRINT_ACTION_EXPORT_PDF && g_pendingPdfPath) {
        ICoreWebView2_7 *pdfWebView = NULL;
        if (SUCCEEDED(ICoreWebView2_QueryInterface(g_webview, &IID_ICoreWebView2_7,
                                                   (void **)&pdfWebView)) && pdfWebView) {
            HRESULT result = ICoreWebView2_7_PrintToPdf(pdfWebView, g_pendingPdfPath, NULL,
                                                        &g_pdfHandler);
            ICoreWebView2_7_Release(pdfWebView);
            if (SUCCEEDED(result)) return;
        }
        show_error(L"This WebView2 Runtime could not start PDF export.");
        free(g_pendingPdfPath);
        g_pendingPdfPath = NULL;
        g_pendingPrintAction = PRINT_ACTION_NONE;
    }
}

static void release_document_reference(Document *doc) {
    if (doc->sciDocument && g_hEdit)
        SendMessageA(g_hEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)doc->sciDocument);
    free(doc->path);
    memset(doc, 0, sizeof(*doc));
}

static void push_closed_document(Document removed, int index) {
    removed.closedIndex = index;
    /* Holding a large-file tab's document for Reopen Closed Tab would keep
       the whole file in memory; it keeps only its path and is read from
       disk again on reopen (see do_reopen_closed_tab()). */
    if (removed.largeFile && removed.sciDocument) {
        SendMessageA(g_hEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)removed.sciDocument);
        removed.sciDocument = 0;
    }
    if (g_closedDocumentCount == MAX_CLOSED_DOCUMENTS) {
        release_document_reference(&g_closedDocuments[0]);
        for (int i = 0; i + 1 < g_closedDocumentCount; i++)
            g_closedDocuments[i] = g_closedDocuments[i + 1];
        g_closedDocumentCount--;
    }
    g_closedDocuments[g_closedDocumentCount++] = removed;
}

static void do_close_document_at(int index) {
    if (index < 0 || index >= g_documentStore.count) return;
    if (g_documentStore.documents[index].pinned) {
        MessageBeep(MB_ICONASTERISK);
        show_plugin_reload_status(L"Unpin the tab before closing it");
        return;
    }
    if (index != g_documentStore.active) switch_document(index);
    fire_plugin_event("close", g_currentFile);
    if (!confirm_discard_if_dirty()) return;
    save_active_document_view();
    /* Still dirty here means the user chose not to save, so its markers
       follow discarded edits and the persisted copy is left as it was. */
    if (!g_dirty) save_breakpoints_for_active_document(g_currentFile);
    int removeIndex = g_documentStore.active;
    Document removed = g_documentStore.documents[removeIndex];
    if (g_splitDocument == removed.sciDocument) g_splitDocument = 0;

    if (g_documentStore.count <= 1) {
        sptr_t newDoc = (sptr_t)SendMessageA(g_hEdit, SCI_CREATEDOCUMENT, 0, 0);
        if (!newDoc) return;
        memset(&g_documentStore.documents[0], 0, sizeof(g_documentStore.documents[0]));
        g_documentStore.documents[0].sciDocument = newDoc;
        /* Closing the final tab leaves one canonical, reusable blank document.
           Numbered names are reserved for additional unsaved tabs. */
        g_documentStore.documents[0].untitledNumber = 0;
        g_documentStore.switching = 1;
        set_active_editor_document(newDoc);
        configure_scintilla(g_hEdit);
        apply_editor_font();
        if (g_hSplitEdit) apply_language_to_view(g_hSplitEdit, g_currentFile);
        SendMessageA(g_hEdit, SCI_CLEARALL, 0, 0);
        reset_undo_history(g_hEdit);
        SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
        g_documentStore.switching = 0;
        push_closed_document(removed, 0);
        g_recoverySnapshotStale = 1;
        write_recovery_snapshot();
        update_tab_label(0);
        update_document_mapping();
        update_preview();
        layout(g_hMainWnd);
        update_title();
        refresh_menu(g_hMainWnd);
        SetFocus(g_hEdit);
        return;
    }

    TabCtrl_DeleteItem(g_hTabs, removeIndex);
    for (int i = removeIndex; i + 1 < g_documentStore.count; i++) g_documentStore.documents[i] = g_documentStore.documents[i + 1];
    memset(&g_documentStore.documents[g_documentStore.count - 1], 0, sizeof(g_documentStore.documents[0]));
    g_documentStore.count--;
    g_documentStore.active = removeIndex < g_documentStore.count ? removeIndex : g_documentStore.count - 1;
    g_documentStore.documents[g_documentStore.active].lastActivated = ++g_tabActivationSerial;
    g_documentStore.switching = 1;
    set_active_editor_document(g_documentStore.documents[g_documentStore.active].sciDocument);
    apply_editor_font();
    apply_document_language(g_currentFile);
    restore_document_view(g_hEdit, &g_documentStore.documents[g_documentStore.active]);
    g_documentStore.switching = 0;
    push_closed_document(removed, removeIndex);
    g_recoverySnapshotStale = 1;
    write_recovery_snapshot();
    TabCtrl_SetCurSel(g_hTabs, g_documentStore.active);
    update_linenumber_margin_width(g_hEdit);
    update_document_mapping();
    update_preview();
    layout(g_hMainWnd);
    update_title();
    refresh_menu(g_hMainWnd);
    prompt_reload_active_external_change();
    if (!active_document_is_image()) SetFocus(g_hEdit);
}

static void do_reopen_closed_tab(void) {
    if (g_closedDocumentCount <= 0) {
        MessageBeep(MB_ICONASTERISK);
        return;
    }
    Document restored = g_closedDocuments[--g_closedDocumentCount];
    memset(&g_closedDocuments[g_closedDocumentCount], 0, sizeof(g_closedDocuments[0]));
    if (!restored.sciDocument) { /* a large-file tab, see push_closed_document() */
        if (restored.path) open_file_in_tab(restored.path);
        free(restored.path);
        return;
    }
    save_active_document_view();

    int replaceBlank = g_documentStore.count == 1 && active_document_is_reusable();
    int index = restored.closedIndex;
    if (index < 0) index = 0;
    if (index > g_documentStore.count) index = g_documentStore.count;
    if (replaceBlank) {
        Document blank = g_documentStore.documents[0];
        g_documentStore.documents[0] = restored;
        index = 0;
        g_documentStore.active = 0;
        g_documentStore.switching = 1;
        set_active_editor_document(restored.sciDocument);
        g_documentStore.switching = 0;
        release_document_reference(&blank);
    } else {
        if (g_documentStore.count >= MAX_DOCUMENTS) {
            g_closedDocuments[g_closedDocumentCount++] = restored;
            MessageBoxW(g_hMainWnd, L"Close a tab before reopening this document.", L"Tinker Editor", MB_OK | MB_ICONINFORMATION);
            return;
        }
        for (int i = g_documentStore.count; i > index; i--) g_documentStore.documents[i] = g_documentStore.documents[i - 1];
        g_documentStore.documents[index] = restored;
        g_documentStore.count++;
        g_documentStore.active = index;
        g_documentStore.documents[index].lastActivated = ++g_tabActivationSerial;
        g_documentStore.switching = 1;
        set_active_editor_document(restored.sciDocument);
        g_documentStore.switching = 0;
        TCITEMW item = {0};
        item.mask = TCIF_TEXT;
        item.pszText = L"";
        SendMessageW(g_hTabs, TCM_INSERTITEMW, (WPARAM)index, (LPARAM)&item);
    }
    apply_editor_font();
    apply_document_language(g_currentFile);
    restore_document_view(g_hEdit, &restored);
    TabCtrl_SetCurSel(g_hTabs, index);
    update_tab_label(index);
    update_linenumber_margin_width(g_hEdit);
    g_recoverySnapshotStale = 1;
    write_recovery_snapshot();
    update_document_mapping();
    update_preview();
    layout(g_hMainWnd);
    update_title();
    refresh_menu(g_hMainWnd);
    prompt_reload_active_external_change();
    if (!active_document_is_image()) SetFocus(g_hEdit);
}

static void prompt_reload_active_external_change(void) {
    /* The file-watch timer keeps firing inside the message box's modal loop,
       so a file written several times would otherwise stack up prompts. */
    static int prompting = 0;
    if (prompting) return;
    if (g_documentStore.count <= 0 || !g_currentFile || !g_documentStore.documents[g_documentStore.active].externalChanged) return;
    prompting = 1;
    prompt_reload_external_change(g_documentStore.active);
    /* One answer covers every write made while the prompt was open. */
    if (g_documentStore.active < g_documentStore.count && !g_documentStore.documents[g_documentStore.active].externalDeleted)
        record_document_file_time(g_documentStore.active);
    prompting = 0;
}

static void prompt_reload_external_change(int index) {
    Document *doc = &g_documentStore.documents[index];
    doc->externalChanged = 0;
    InvalidateRect(g_hTabs, NULL, FALSE);
    if (doc->externalDeleted) {
        if (doc->viewKind == DOCUMENT_VIEW_IMAGE) {
            MessageBoxW(g_hMainWnd,
                        L"This image was deleted or moved on disk. Close the tab or restore the file to the same path.",
                        L"Tinker Editor", MB_OK | MB_ICONWARNING);
            return;
        }
        MessageBoxW(g_hMainWnd,
                    g_dirty
                        ? L"This file was deleted or moved on disk. Your unsaved text is still open; saving will recreate the old path."
                        : L"This file was deleted or moved on disk. Its text remains open; saving will recreate the old path.",
                    L"Tinker Editor", MB_OK | MB_ICONWARNING);
        return;
    }
    /* A document set to reload by itself (documents.auto_reload()) doesn't
       ask, unless reloading would throw away unsaved edits. */
    if (doc->autoReload && !g_dirty) {
        if (!append_file_growth()) reload_active_document();
        return;
    }
    const WCHAR *message = g_dirty
        ? L"This file changed on disk. Reload it and discard your unsaved changes?"
        : L"This file changed on disk. Reload the updated version?";
    if (MessageBoxW(g_hMainWnd, message, L"Tinker Editor", MB_YESNO | MB_ICONQUESTION) == IDYES)
        reload_active_document();
}

static void check_external_file_change(void) {
    int changedVisual = 0;
    for (int i = 0; i < g_documentStore.count; i++) {
        Document *doc = &g_documentStore.documents[i];
        if (!doc->path) continue;
        WCHAR *ioPath = extended_path(doc->path);
        if (!ioPath) continue;
        WIN32_FILE_ATTRIBUTE_DATA data;
        int exists = GetFileAttributesExW(ioPath, GetFileExInfoStandard, &data);
        DWORD attributeError = exists ? ERROR_SUCCESS : GetLastError();
        free(ioPath);
        if (!exists) {
            if (attributeError != ERROR_FILE_NOT_FOUND && attributeError != ERROR_PATH_NOT_FOUND)
                continue;
            if (doc->hasFileTime && !doc->externalDeleted) {
                doc->externalDeleted = 1;
                doc->externalChanged = 1;
                changedVisual = 1;
            }
            doc->hasFileTime = 0;
            continue;
        }
        if (!doc->hasFileTime) {
            doc->fileTime = data.ftLastWriteTime;
            doc->hasFileTime = 1;
            if (doc->externalDeleted) {
                doc->externalDeleted = 0;
                doc->externalChanged = 1;
                changedVisual = 1;
            }
            continue;
        }
        if (CompareFileTime(&doc->fileTime, &data.ftLastWriteTime) != 0) {
            doc->fileTime = data.ftLastWriteTime;
            if (!doc->externalChanged) changedVisual = 1;
            doc->externalChanged = 1;
        }
    }
    if (changedVisual) InvalidateRect(g_hTabs, NULL, FALSE);
    prompt_reload_active_external_change();
}

static void do_exit(void) {
    int original = g_documentStore.active;
    for (int i = 0; i < g_documentStore.count; i++) {
        if (!g_documentStore.documents[i].dirty) continue;
        switch_document(i);
        if (!confirm_discard_if_dirty()) {
            if (original < g_documentStore.count) switch_document(original);
            return;
        }
    }
    if (original < g_documentStore.count) switch_document(original);
    save_all_document_breakpoints();
    save_layout_settings();
    save_session_documents();
    close_recovery_session();
    for (int i = 0; i < g_documentStore.count; i++) {
        SendMessageA(g_hEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)g_documentStore.documents[i].sciDocument);
        g_documentStore.documents[i].sciDocument = 0;
    }
    for (int i = 0; i < g_closedDocumentCount; i++)
        release_document_reference(&g_closedDocuments[i]);
    g_closedDocumentCount = 0;
    DestroyWindow(g_hMainWnd);
}
