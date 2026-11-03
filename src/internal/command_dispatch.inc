static int handle_file_command(HWND hwnd, WORD cmd) {
    switch (cmd) {
        case IDM_FILE_NEW:
            create_document_tab();
            break;
        case IDM_FILE_OPEN:
            do_open();
            break;
        case IDM_FILE_INSERT:
            do_insert_file();
            break;
        case IDM_FILE_QUICK_OPEN:
            do_quick_open();
            break;
        case IDM_FILE_FIND_FOLDER:
            do_find_in_folder();
            break;
        case IDM_REPLACE_WORKSPACE:
            do_replace_in_workspace();
            break;
        case IDM_OPEN_WORKSPACE:
            do_open_workspace();
            break;
        case IDM_RECENT_WORKSPACE_CLEAR:
            recent_list_clear(&g_recentFolderList);
            refresh_menu(hwnd);
            break;
        case IDM_FILE_SAVE:
            if (GetFocus() == g_hSplitEdit && split_document_index() >= 0 &&
                split_document_index() != g_documentStore.active)
                do_save_tab_at(split_document_index());
            else do_save();
            break;
        case IDM_FILE_SAVEAS:
            if (GetFocus() == g_hSplitEdit && split_document_index() >= 0 &&
                split_document_index() != g_documentStore.active)
                do_saveas_tab_at(split_document_index());
            else do_saveas();
            break;
        case IDM_FILE_SAVE_ALL:
            do_save_all();
            break;
        case IDM_FILE_EXPORT_HTML:
            do_export_html();
            break;
        case IDM_FILE_EXPORT_PDF:
            prepare_rendered_print(1);
            break;
        case IDM_FILE_PRINT:
            prepare_rendered_print(0);
            break;
        case IDM_FILE_OPEN_TERMINAL:
            open_terminal_here();
            break;
        case IDM_FILE_SHOW_EXPLORER:
            reveal_in_explorer();
            break;
        case IDM_FILE_CLOSE:
            do_close_document_at(g_documentStore.active);
            break;
        case IDM_FILE_REOPEN_CLOSED:
            do_reopen_closed_tab();
            break;
        case IDM_FILE_GO_BACK:
            go_wiki_back();
            break;
        case IDM_FILE_RECENT_CLEAR:
            recent_list_clear(&g_recentFileList);
            refresh_menu(hwnd);
            break;
        case IDM_SETTINGS:
            show_settings_dialog();
            break;
        case IDM_MARKDOWN_CHECK_WORKSPACE_LINKS:
            check_workspace_markdown_links();
            break;
        case IDM_MARKDOWN_BACKLINKS:
            show_markdown_backlinks();
            break;
        case IDM_FILE_EXIT:
            do_exit();
            break;
        default:
            return 0;
    }
    return 1;
}

static int handle_edit_command(WORD cmd) {
    switch (cmd) {
        case IDM_EDIT_UNDO:
            SendMessageA(active_edit_view(), SCI_UNDO, 0, 0);
            break;
        case IDM_EDIT_REDO:
            SendMessageA(active_edit_view(), SCI_REDO, 0, 0);
            break;
        case IDM_EDIT_CUT:
            SendMessageA(active_edit_view(), SCI_CUT, 0, 0);
            break;
        case IDM_EDIT_COPY:
            SendMessageA(active_edit_view(), SCI_COPY, 0, 0);
            break;
        case IDM_EDIT_PASTE:
            /* Image paste always lands in the main pane - it can prompt to
               save the document first, which isn't worth threading through
               to the split pane for what's a rare combination. */
            if (!paste_image_from_clipboard()) SendMessageA(active_edit_view(), SCI_PASTE, 0, 0);
            break;
        case IDM_EDIT_DELETE:
            SendMessageA(active_edit_view(), SCI_CLEAR, 0, 0);
            break;
        case IDM_EDIT_SELECTALL:
            SendMessageA(active_edit_view(), SCI_SELECTALL, 0, 0);
            break;
        case IDM_EDIT_DUPLICATE_LINE:
            SendMessageA(active_edit_view(), SCI_LINEDUPLICATE, 0, 0);
            break;
        case IDM_EDIT_MOVE_LINE_UP:
            SendMessageA(active_edit_view(), SCI_MOVESELECTEDLINESUP, 0, 0);
            break;
        case IDM_EDIT_MOVE_LINE_DOWN:
            SendMessageA(active_edit_view(), SCI_MOVESELECTEDLINESDOWN, 0, 0);
            break;
        case IDM_EDIT_DELETE_LINE:
            SendMessageA(active_edit_view(), SCI_LINEDELETE, 0, 0);
            break;
        case IDM_EDIT_JOIN_LINES:
            SendMessageA(active_edit_view(), SCI_LINESJOIN, 0, 0);
            break;
        case IDM_EDIT_SELECT_NEXT:
            select_next_occurrence(active_edit_view());
            break;
        case IDM_EDIT_SELECT_ALL_OCCURRENCES:
            select_all_occurrences(active_edit_view());
            break;
        case IDM_EDIT_MATCH_BRACE:
            jump_to_matching_brace(active_edit_view());
            break;
        case IDM_EDIT_BOOKMARK_TOGGLE:
            toggle_bookmark_at_cursor(active_edit_view());
            break;
        case IDM_EDIT_BOOKMARK_NEXT:
            goto_bookmark(active_edit_view(), 1);
            break;
        case IDM_EDIT_BOOKMARK_PREV:
            goto_bookmark(active_edit_view(), 0);
            break;
        case IDM_EDIT_BOOKMARK_CLEAR:
            clear_all_bookmarks();
            break;
        case IDM_FILE_REOPEN_UTF8:
            reopen_with_encoding(TEXT_ENCODING_UTF8);
            break;
        case IDM_FILE_REOPEN_UTF16LE:
            reopen_with_encoding(TEXT_ENCODING_UTF16LE);
            break;
        case IDM_FILE_REOPEN_UTF16BE:
            reopen_with_encoding(TEXT_ENCODING_UTF16BE);
            break;
        case IDM_FILE_REOPEN_ANSI:
            reopen_with_encoding(TEXT_ENCODING_ANSI);
            break;
        case IDM_EDIT_FIND:
            show_find_dialog(0);
            break;
        case IDM_EDIT_REPLACE:
            show_find_dialog(1);
            break;
        case IDM_EDIT_GOTOLINE:
            show_goto_line_dialog();
            break;
        case IDM_EDIT_FINDNEXT:
            find_again(1);
            break;
        case IDM_EDIT_FINDPREV:
            find_again(0);
            break;
        default:
            return 0;
    }
    return 1;
}

static int handle_format_command(WORD cmd) {
    switch (cmd) {
        case IDM_FORMAT_FONT:
            do_choose_font();
            break;
        default:
            return 0;
    }
    return 1;
}

static int handle_debug_command(WORD cmd) {
    switch (cmd) {
        case IDM_RUN_FILE:
            launch_active_source_file(0);
            break;
        case IDM_RUN_USE_CONSOLE:
            g_runInConsole = !g_runInConsole;
            update_view_menu(g_hMainWnd);
            save_layout_settings();
            break;
        case IDM_DEBUG_FILE:
            launch_active_source_file(1);
            break;
        case IDM_DEBUG_CONTINUE: case IDM_DEBUG_STEP_OVER:
        case IDM_DEBUG_STEP_INTO: case IDM_DEBUG_STEP_OUT:
            run_debugger_command(cmd);
            break;
        case IDM_DEBUG_BREAKPOINT:
            toggle_breakpoint_at_cursor();
            break;
        case IDM_DEBUG_CLEAR_BREAKPOINTS:
            clear_debug_breakpoints();
            break;
        default:
            return 0;
    }
    return 1;
}

static int handle_view_command(HWND hwnd, WORD cmd) {
    switch (cmd) {
        case IDM_VIEW_TOGGLEPREVIEW:
            toggle_preview(hwnd);
            break;
        case IDM_VIEW_TOGGLEEDITOR:
            toggle_editor(hwnd);
            break;
        case IDM_VIEW_PREVIEW_ON_LEFT:
            toggle_panel_sides(hwnd);
            break;
        case IDM_VIEW_CUSTOM_CSS:
            do_choose_custom_css();
            break;
        case IDM_VIEW_CLEAR_CUSTOM_CSS:
            clear_custom_css();
            break;
        case IDM_VIEW_FOCUS_MODE:
            toggle_focus_mode(hwnd);
            break;
        case IDM_VIEW_WORD_WRAP:
            toggle_word_wrap(hwnd);
            break;
        case IDM_VIEW_SPELL_CHECK:
            toggle_spell_check(hwnd);
            break;
        case IDM_VIEW_SPELL_LANGUAGE:
            choose_spell_language();
            break;
        case IDM_VIEW_SHOW_WHITESPACE:
            toggle_show_whitespace(hwnd);
            break;
        case IDM_VIEW_CHANGED_LINES:
            toggle_changed_lines(hwnd);
            break;
        case IDM_VIEW_INDENT_GUIDES:
            toggle_indent_guides(hwnd);
            break;
        case IDM_VIEW_USE_SPACES_INDENT:
            toggle_indent_use_spaces(hwnd);
            break;
        case IDM_VIEW_FOLD_TOGGLE:
            fold_toggle_at_cursor();
            break;
        case IDM_VIEW_FOLD_ALL:
            fold_all_lines(0);
            break;
        case IDM_VIEW_UNFOLD_ALL:
            fold_all_lines(1);
            break;
        case IDM_VIEW_OUTLINE:
            toggle_outline(hwnd);
            break;
        case IDM_VIEW_MINIMAP:
            toggle_minimap(hwnd);
            break;
        case IDM_VIEW_EXPLORER:
            toggle_explorer_panel(hwnd);
            break;
        case IDM_VIEW_OUTPUT_PANEL:
            toggle_output_panel();
            break;
        case IDM_VIEW_TERMINAL:
            show_terminal();
            break;
        case IDM_VIEW_PROBLEMS:
            show_problems();
            break;
        case IDM_VIEW_TOGGLESPLIT:
            toggle_split_editor(hwnd, 0);
            for (int i = 0; i < g_documentStore.count; i++) update_tab_label(i);
            break;
        case IDM_VIEW_SPLIT_VERTICAL:
            toggle_split_editor(hwnd, 1);
            for (int i = 0; i < g_documentStore.count; i++) update_tab_label(i);
            break;
        case IDM_VIEW_RESET_SPLIT_EDITOR:
            reset_split_editor(hwnd);
            break;
        case IDM_VIEW_ZOOM_IN:
            adjust_editor_zoom(1);
            break;
        case IDM_VIEW_ZOOM_OUT:
            adjust_editor_zoom(-1);
            break;
        case IDM_VIEW_ZOOM_RESET:
            reset_editor_zoom();
            break;
        case IDM_VIEW_NEXT_TAB:
            if (g_documentStore.count > 1) switch_document((g_documentStore.active + 1) % g_documentStore.count);
            break;
        case IDM_VIEW_PREVIOUS_TAB:
            if (g_documentStore.count > 1) switch_document((g_documentStore.active + g_documentStore.count - 1) % g_documentStore.count);
            break;
        case IDM_VIEW_TAB_SWITCHER:
            show_tab_switcher();
            break;
        case IDM_VIEW_COMMAND_PALETTE:
            show_command_palette();
            break;
        case IDM_VIEW_PIN_TAB:
            toggle_document_pinned(g_documentStore.active);
            break;
        case IDM_VIEW_SPLIT_DOCUMENT:
            show_split_document_dialog();
            break;
        case IDM_VIEW_CYCLE_PANE:
            cycle_pane_focus(0);
            break;
        case IDM_VIEW_CYCLE_PANE_PREV:
            cycle_pane_focus(1);
            break;
        case IDM_VIEW_THEME_GITHUB_LIGHT:
        case IDM_VIEW_THEME_GITHUB_DARK:
            select_theme(hwnd, cmd == IDM_VIEW_THEME_GITHUB_DARK);
            break;
        case IDM_VIEW_PREVIEW_THEME_LIGHT:
        case IDM_VIEW_PREVIEW_THEME_DARK:
            select_preview_theme(hwnd, cmd == IDM_VIEW_PREVIEW_THEME_DARK);
            break;
        default:
            return 0;
    }
    return 1;
}

static int handle_git_command(WORD cmd) {
    switch (cmd) {
        case IDM_GIT_REFRESH: show_git_status(); break;
        case IDM_GIT_DIFF_FILE: show_git_file_diff(); break;
        case IDM_GIT_DIFF_FILE_STAGED: show_git_staged_file_diff(); break;
        case IDM_GIT_DIFF_WORKSPACE: show_git_workspace_diff(); break;
        case IDM_GIT_DIFF_STAGED: show_git_staged_diff(); break;
        case IDM_GIT_LOG_FILE: show_git_file_log(); break;
        case IDM_GIT_BLAME_FILE: show_git_file_blame(); break;
        case IDM_VIEW_GIT: show_git_panel(); break;
        case IDM_GIT_STAGE_ALL: git_stage_all(); break;
        case IDM_GIT_UNSTAGE_ALL: git_unstage_all(); break;
        case IDM_GIT_DISCARD_ALL: git_discard_all(); break;
        case IDM_GIT_COMMIT: git_commit_prompt(); break;
        case IDM_GIT_PUSH: git_push(); break;
        case IDM_GIT_PULL: git_pull(); break;
        case IDM_GIT_FETCH: git_fetch(); break;
        case IDM_GIT_SWITCH_BRANCH: git_switch_branch_prompt(); break;
        case IDM_GIT_NEW_BRANCH: git_new_branch_prompt(); break;
        case IDM_GIT_DELETE_BRANCH: git_delete_branch_prompt(); break;
        case IDM_GIT_MERGE: git_merge_prompt(); break;
        case IDM_GIT_REBASE: git_rebase_prompt(); break;
        case IDM_GIT_PUBLISH: git_publish_branch(); break;
        case IDM_GIT_MERGE_CONTINUE: git_merge_continue(); break;
        case IDM_GIT_MERGE_ABORT: git_merge_abort(); break;
        case IDM_GIT_REBASE_CONTINUE: git_rebase_continue(); break;
        case IDM_GIT_REBASE_SKIP: git_rebase_skip(); break;
        case IDM_GIT_REBASE_ABORT: git_rebase_abort(); break;
        case IDM_GIT_STASH_PUSH: git_stash_push_prompt(); break;
        case IDM_GIT_STASH_APPLY: git_stash_apply_prompt(); break;
        case IDM_GIT_STASH_POP: git_stash_pop(); break;
        case IDM_GIT_STASH_DROP: git_stash_drop_prompt(); break;
        default: return 0;
    }
    return 1;
}

/* Help > Plugin Guide: plugins\README.md beside tinkeredit.exe, in a tab. */
static void show_plugin_guide(void) {
    WCHAR *directory = module_directory();
    WCHAR *guide = existing_file_in_directory(directory, L"plugins\\README.md");
    if (guide) {
        open_file_in_tab(guide);
    } else {
        MessageBoxW(g_hMainWnd, L"The plugin guide (plugins\\README.md beside tinkeredit.exe) is missing.",
                    L"Tinker Editor", MB_OK | MB_ICONINFORMATION);
    }
    free(guide);
    free(directory);
}

static int handle_help_command(WORD cmd) {
    switch (cmd) {
        case IDM_HELP_REFERENCE:
            show_help_page(REFERENCE_MARKDOWN);
            break;
        case IDM_HELP_SHORTCUTS:
            show_help_page(REFERENCE_SHORTCUTS);
            break;
        case IDM_HELP_ABOUT:
            show_about_box();
            break;
        case IDM_HELP_PLUGIN_GUIDE:
            show_plugin_guide();
            break;
        default:
            return 0;
    }
    return 1;
}

static void show_plugin_error(const char *report) {
    WCHAR *text = to_wide(report);
    MessageBoxW(g_hMainWnd, text ? text : L"A plugin failed; its error message isn't valid UTF-8.",
        L"Tinker Editor Plugins", MB_OK | MB_ICONWARNING);
    free(text);
}

/* A failing Lua call rolls its Scintilla undo group back. SCN_MODIFIED has
   already marked the document dirty by then, and this application does not
   otherwise derive that flag from save-point notifications, so reconcile it
   explicitly after every plugin call. This also covers a successful handler
   that edits a freshly loaded/saved document. */
static void sync_dirty_after_plugin(HWND editor, int wasDirty) {
    int index = document_index_for_view(editor);
    int modified = wasDirty || g_pluginChangedSaveEncoding ||
                   SendMessageA(editor, SCI_GETMODIFY, 0, 0) != 0;
    if (index == g_documentStore.active) {
        if (modified != (g_dirty != 0)) set_dirty(modified);
    } else if (index >= 0 && modified != (g_documentStore.documents[index].dirty != 0)) {
        g_documentStore.documents[index].dirty = modified;
        g_recoverySnapshotStale = 1;
        update_tab_label(index);
    }
}

/* Handlers act on the main pane, like plugin commands. A failing handler is
   reported but doesn't stop what raised the event (a save still happens). */
static void fire_plugin_event(const char *event, const WCHAR *path) {
    int wasDirty = g_dirty;
    g_pluginChangedSaveEncoding = 0;
    g_pluginConfigChanged = 0;
    char *argument = path ? to_utf8(path) : NULL;
    char *report = plugin_host_fire(event, argument, g_hEdit);
    refresh_plugin_performance_problems();
    free(argument);
    sync_dirty_after_plugin(g_hEdit, wasDirty);
    if (report) show_plugin_error(report);
    free(report);
    /* Most high-frequency text/selection handlers only read configuration.
       Rewalk the complete menu bar only when one actually changed it. */
    if (g_pluginConfigChanged) update_view_menu(g_hMainWnd);
    /* A tab check that arrived while the handlers ran was put off. */
    if (!plugin_host_is_busy()) queue_plugin_tab_check();
}

static void check_plugin_active_document(void) {
    g_pluginTabCheckPosted = 0;
    sptr_t active = g_documentStore.documents[g_documentStore.active].sciDocument;
    if (active == g_pluginActiveDocument) return;
    /* Plugin code is waiting in a dialog (a save handler during Save All,
       say), so the event couldn't run: leave the change to be seen again
       once that code finishes, rather than recording it unannounced. */
    if (plugin_host_is_busy()) return;
    g_pluginActiveDocument = active;
    fire_plugin_event("tab_changed", g_currentFile);
}
