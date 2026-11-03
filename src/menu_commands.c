/* The names menus.lua uses for built-in commands, and the shortcut text the
   menu shows beside each. No application state. */
#include "menu_commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "command_ids.h"

static const MenuCommand COMMANDS[] = {
    {"file.new", IDM_FILE_NEW, "Ctrl+N"},
    {"file.open", IDM_FILE_OPEN, "Ctrl+O"},
    {"file.reopen_utf8", IDM_FILE_REOPEN_UTF8, NULL},
    {"file.reopen_utf16le", IDM_FILE_REOPEN_UTF16LE, NULL},
    {"file.reopen_utf16be", IDM_FILE_REOPEN_UTF16BE, NULL},
    {"file.reopen_ansi", IDM_FILE_REOPEN_ANSI, NULL},
    {"file.quick_open", IDM_FILE_QUICK_OPEN, "Ctrl+K"},
    {"file.find_in_folder", IDM_FILE_FIND_FOLDER, NULL},
    {"file.replace_in_folder", IDM_REPLACE_WORKSPACE, NULL},
    {"file.open_workspace", IDM_OPEN_WORKSPACE, NULL},
    {"file.insert_file", IDM_FILE_INSERT, NULL},
    {"file.save", IDM_FILE_SAVE, "Ctrl+S"},
    {"file.save_as", IDM_FILE_SAVEAS, "Ctrl+Shift+S"},
    {"file.save_all", IDM_FILE_SAVE_ALL, "Ctrl+Alt+S"},
    {"file.export_html", IDM_FILE_EXPORT_HTML, NULL},
    {"file.export_pdf", IDM_FILE_EXPORT_PDF, NULL},
    {"file.print", IDM_FILE_PRINT, "Ctrl+P"},
    {"file.open_terminal", IDM_FILE_OPEN_TERMINAL, NULL},
    {"file.show_in_explorer", IDM_FILE_SHOW_EXPLORER, NULL},
    {"file.close_tab", IDM_FILE_CLOSE, "Ctrl+W"},
    {"file.reopen_closed_tab", IDM_FILE_REOPEN_CLOSED, "Ctrl+Shift+T"},
    {"file.go_back", IDM_FILE_GO_BACK, "Alt+Left"},
    {"file.exit", IDM_FILE_EXIT, NULL},

    {"edit.undo", IDM_EDIT_UNDO, "Ctrl+Z"},
    {"edit.redo", IDM_EDIT_REDO, "Ctrl+Y"},
    {"edit.cut", IDM_EDIT_CUT, "Ctrl+X"},
    {"edit.copy", IDM_EDIT_COPY, "Ctrl+C"},
    {"edit.paste", IDM_EDIT_PASTE, "Ctrl+V"},
    {"edit.delete", IDM_EDIT_DELETE, "Del"},
    {"edit.select_all", IDM_EDIT_SELECTALL, "Ctrl+A"},
    {"edit.duplicate_line", IDM_EDIT_DUPLICATE_LINE, "Ctrl+D"},
    {"edit.move_line_up", IDM_EDIT_MOVE_LINE_UP, "Alt+Up"},
    {"edit.move_line_down", IDM_EDIT_MOVE_LINE_DOWN, "Alt+Down"},
    {"edit.delete_line", IDM_EDIT_DELETE_LINE, "Ctrl+Shift+K"},
    {"edit.join_lines", IDM_EDIT_JOIN_LINES, "Ctrl+J"},
    {"edit.toggle_comment", IDM_EDIT_TOGGLE_COMMENT, "Ctrl+/"},
    {"edit.trim_whitespace", IDM_EDIT_TRIM_WHITESPACE, NULL},
    {"edit.select_next", IDM_EDIT_SELECT_NEXT, "Ctrl+Shift+D"},
    {"edit.select_all_occurrences", IDM_EDIT_SELECT_ALL_OCCURRENCES, "Ctrl+Shift+L"},
    {"edit.match_brace", IDM_EDIT_MATCH_BRACE, "Ctrl+]"},
    {"edit.bookmark_toggle", IDM_EDIT_BOOKMARK_TOGGLE, "Ctrl+F2"},
    {"edit.bookmark_next", IDM_EDIT_BOOKMARK_NEXT, "F2"},
    {"edit.bookmark_previous", IDM_EDIT_BOOKMARK_PREV, "Shift+F2"},
    {"edit.bookmark_clear", IDM_EDIT_BOOKMARK_CLEAR, NULL},
    {"edit.find", IDM_EDIT_FIND, "Ctrl+F"},
    {"edit.find_next", IDM_EDIT_FINDNEXT, "F3"},
    {"edit.find_previous", IDM_EDIT_FINDPREV, "Shift+F3"},
    {"edit.replace", IDM_EDIT_REPLACE, "Ctrl+H"},
    {"edit.go_to_line", IDM_EDIT_GOTOLINE, "Ctrl+G"},

    {"view.preview", IDM_VIEW_TOGGLEPREVIEW, "Ctrl+Shift+V"},
    {"view.editor", IDM_VIEW_TOGGLEEDITOR, "Ctrl+Shift+E"},
    {"view.next_tab", IDM_VIEW_NEXT_TAB, "Ctrl+Tab"},
    {"view.previous_tab", IDM_VIEW_PREVIOUS_TAB, "Ctrl+Shift+Tab"},
    {"view.tab_switcher", IDM_VIEW_TAB_SWITCHER, "Ctrl+E"},
    {"view.command_palette", IDM_VIEW_COMMAND_PALETTE, "Ctrl+Shift+P"},
    {"view.pin_tab", IDM_VIEW_PIN_TAB, NULL},
    {"view.cycle_panes", IDM_VIEW_CYCLE_PANE, "Ctrl+F6"},
    {"view.cycle_panes_previous", IDM_VIEW_CYCLE_PANE_PREV, "Ctrl+Shift+F6"},
    {"view.theme_light", IDM_VIEW_THEME_GITHUB_LIGHT, NULL},
    {"view.theme_dark", IDM_VIEW_THEME_GITHUB_DARK, NULL},
    {"view.preview_theme_light", IDM_VIEW_PREVIEW_THEME_LIGHT, NULL},
    {"view.preview_theme_dark", IDM_VIEW_PREVIEW_THEME_DARK, NULL},
    {"view.custom_css", IDM_VIEW_CUSTOM_CSS, NULL},
    {"view.clear_custom_css", IDM_VIEW_CLEAR_CUSTOM_CSS, NULL},
    {"view.font", IDM_FORMAT_FONT, NULL},
    {"view.preview_on_left", IDM_VIEW_PREVIEW_ON_LEFT, NULL},
    {"view.focus_mode", IDM_VIEW_FOCUS_MODE, "Ctrl+Shift+F11"},
    {"view.word_wrap", IDM_VIEW_WORD_WRAP, "Alt+Z"},
    {"view.spell_check", IDM_VIEW_SPELL_CHECK, NULL},
    {"view.spell_check_language", IDM_VIEW_SPELL_LANGUAGE, NULL},
    {"view.outline", IDM_VIEW_OUTLINE, NULL},
    {"view.file_explorer", IDM_VIEW_EXPLORER, "Ctrl+Shift+X"},
    {"view.output_panel", IDM_VIEW_OUTPUT_PANEL, "Ctrl+`"},
    {"view.terminal", IDM_VIEW_TERMINAL, "Ctrl+Shift+`"},
    {"view.problems", IDM_VIEW_PROBLEMS, "Ctrl+Shift+M"},
    {"view.minimap", IDM_VIEW_MINIMAP, NULL},
    {"view.split_editor", IDM_VIEW_TOGGLESPLIT, "Ctrl+\\"},
    {"view.split_editor_vertical", IDM_VIEW_SPLIT_VERTICAL, NULL},
    {"view.split_document", IDM_VIEW_SPLIT_DOCUMENT, NULL},
    {"view.reset_split_editor", IDM_VIEW_RESET_SPLIT_EDITOR, NULL},
    {"view.show_whitespace", IDM_VIEW_SHOW_WHITESPACE, NULL},
    {"view.indent_guides", IDM_VIEW_INDENT_GUIDES, NULL},
    {"view.changed_lines", IDM_VIEW_CHANGED_LINES, NULL},
    {"view.use_spaces", IDM_VIEW_USE_SPACES_INDENT, NULL},
    {"view.toggle_fold", IDM_VIEW_FOLD_TOGGLE, NULL},
    {"view.fold_all", IDM_VIEW_FOLD_ALL, NULL},
    {"view.unfold_all", IDM_VIEW_UNFOLD_ALL, NULL},
    {"view.zoom_in", IDM_VIEW_ZOOM_IN, "Ctrl+="},
    {"view.zoom_out", IDM_VIEW_ZOOM_OUT, "Ctrl+-"},
    {"view.zoom_reset", IDM_VIEW_ZOOM_RESET, "Ctrl+0"},

    {"markdown.bold", IDM_FMT_BOLD, NULL},
    {"markdown.italic", IDM_FMT_ITALIC, NULL},
    {"markdown.strikethrough", IDM_FMT_STRIKETHROUGH, NULL},
    {"markdown.inline_code", IDM_FMT_CODE, NULL},
    {"markdown.heading1", IDM_FMT_H1, NULL},
    {"markdown.heading2", IDM_FMT_H2, NULL},
    {"markdown.heading3", IDM_FMT_H3, NULL},
    {"markdown.blockquote", IDM_FMT_QUOTE, NULL},
    {"markdown.bullet_list", IDM_FMT_BULLETLIST, NULL},
    {"markdown.numbered_list", IDM_FMT_NUMBERLIST, NULL},
    {"markdown.task_list", IDM_FMT_TASKLIST, NULL},
    {"markdown.link", IDM_FMT_LINK, NULL},
    {"markdown.image", IDM_FMT_IMAGE, NULL},
    {"markdown.table", IDM_FMT_TABLE, NULL},
    {"markdown.table_editor", IDM_FMT_TABLE_EDITOR, NULL},
    {"markdown.table_row_above", IDM_FMT_TABLE_ROW_ABOVE, NULL},
    {"markdown.table_row_below", IDM_FMT_TABLE_ROW_BELOW, NULL},
    {"markdown.table_row_delete", IDM_FMT_TABLE_ROW_DELETE, NULL},
    {"markdown.table_column_left", IDM_FMT_TABLE_COL_LEFT, NULL},
    {"markdown.table_column_right", IDM_FMT_TABLE_COL_RIGHT, NULL},
    {"markdown.table_column_delete", IDM_FMT_TABLE_COL_DELETE, NULL},
    {"markdown.table_sort_ascending", IDM_TABLE_SORT_ASC, NULL},
    {"markdown.table_sort_descending", IDM_TABLE_SORT_DESC, NULL},
    {"markdown.table_align_left", IDM_TABLE_ALIGN_LEFT, NULL},
    {"markdown.table_align_center", IDM_TABLE_ALIGN_CENTER, NULL},
    {"markdown.table_align_right", IDM_TABLE_ALIGN_RIGHT, NULL},
    {"markdown.code_block", IDM_FMT_CODEBLOCK, NULL},
    {"markdown.footnote", IDM_FMT_FOOTNOTE, NULL},
    {"markdown.horizontal_rule", IDM_FMT_HRULE, NULL},
    {"markdown.alert", IDM_FMT_ALERT, NULL},

    {"tools.settings", IDM_SETTINGS, NULL},
    {"tools.markdown_check_workspace_links", IDM_MARKDOWN_CHECK_WORKSPACE_LINKS, NULL},
    {"tools.markdown_backlinks", IDM_MARKDOWN_BACKLINKS, NULL},
    {"git.status", IDM_GIT_REFRESH, NULL},
    {"git.diff_file", IDM_GIT_DIFF_FILE, NULL},
    {"git.diff_file_staged", IDM_GIT_DIFF_FILE_STAGED, NULL},
    {"git.diff_workspace", IDM_GIT_DIFF_WORKSPACE, NULL},
    {"git.diff_staged", IDM_GIT_DIFF_STAGED, NULL},
    {"git.panel", IDM_VIEW_GIT, "Ctrl+Shift+G"},
    {"git.stage_all", IDM_GIT_STAGE_ALL, NULL},
    {"git.unstage_all", IDM_GIT_UNSTAGE_ALL, NULL},
    {"git.discard_all", IDM_GIT_DISCARD_ALL, NULL},
    {"git.commit", IDM_GIT_COMMIT, NULL},
    {"git.push", IDM_GIT_PUSH, NULL},
    {"git.pull", IDM_GIT_PULL, NULL},
    {"git.fetch", IDM_GIT_FETCH, NULL},
    {"git.switch_branch", IDM_GIT_SWITCH_BRANCH, NULL},
    {"git.new_branch", IDM_GIT_NEW_BRANCH, NULL},
    {"git.delete_branch", IDM_GIT_DELETE_BRANCH, NULL},
    {"git.merge", IDM_GIT_MERGE, NULL},
    {"git.rebase", IDM_GIT_REBASE, NULL},
    {"git.publish", IDM_GIT_PUBLISH, NULL},
    {"git.merge_continue", IDM_GIT_MERGE_CONTINUE, NULL},
    {"git.merge_abort", IDM_GIT_MERGE_ABORT, NULL},
    {"git.rebase_continue", IDM_GIT_REBASE_CONTINUE, NULL},
    {"git.rebase_skip", IDM_GIT_REBASE_SKIP, NULL},
    {"git.rebase_abort", IDM_GIT_REBASE_ABORT, NULL},
    {"git.stash_push", IDM_GIT_STASH_PUSH, NULL},
    {"git.stash_apply", IDM_GIT_STASH_APPLY, NULL},
    {"git.stash_pop", IDM_GIT_STASH_POP, NULL},
    {"git.stash_drop", IDM_GIT_STASH_DROP, NULL},
    {"git.log_file", IDM_GIT_LOG_FILE, NULL},
    {"git.blame_file", IDM_GIT_BLAME_FILE, NULL},

    {"run.run_file", IDM_RUN_FILE, "Ctrl+F5"},
    {"run.debug_file", IDM_DEBUG_FILE, "F5"},
    {"run.continue", IDM_DEBUG_CONTINUE, "F6"},
    {"run.step_over", IDM_DEBUG_STEP_OVER, "F10"},
    {"run.step_into", IDM_DEBUG_STEP_INTO, "F11"},
    {"run.step_out", IDM_DEBUG_STEP_OUT, "Shift+F11"},
    {"run.toggle_breakpoint", IDM_DEBUG_BREAKPOINT, "F9"},
    {"run.clear_breakpoints", IDM_DEBUG_CLEAR_BREAKPOINTS, NULL},
    {"run.use_console", IDM_RUN_USE_CONSOLE, NULL},

    {"plugins.reload", IDM_PLUGIN_RELOAD, NULL},
    {"plugins.customize_menus", IDM_PLUGIN_CUSTOMIZE_MENUS, NULL},
    {"plugins.customize_theme", IDM_PLUGIN_CUSTOMIZE_THEME, NULL},
    {"plugins.open_folder", IDM_PLUGIN_OPEN_FOLDER, NULL},
    {"plugins.new", IDM_PLUGIN_NEW, NULL},
    {"plugins.manage", IDM_PLUGIN_MANAGE, NULL},
    {"plugins.performance", IDM_PLUGIN_PERFORMANCE, NULL},
    {"plugins.clear_performance", IDM_PLUGIN_CLEAR_PERFORMANCE, NULL},

    {"help.markdown_reference", IDM_HELP_REFERENCE, NULL},
    {"help.keyboard_shortcuts", IDM_HELP_SHORTCUTS, NULL},
    {"help.plugin_guide", IDM_HELP_PLUGIN_GUIDE, NULL},
    {"help.about", IDM_HELP_ABOUT, NULL},
};

#define COMMAND_COUNT ((int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])))

static const char *const LISTS[] = { "recent_files", "recent_workspaces", "plugins" };

const MenuCommand *menu_command_find(const char *name) {
    for (int i = 0; i < COMMAND_COUNT; i++)
        if (strcmp(COMMANDS[i].name, name) == 0) return &COMMANDS[i];
    return NULL;
}

const MenuCommand *menu_command_for_id(WORD id) {
    for (int i = 0; i < COMMAND_COUNT; i++)
        if (COMMANDS[i].id == id) return &COMMANDS[i];
    return NULL;
}

const MenuCommand *menu_command_at(int index) {
    return index >= 0 && index < COMMAND_COUNT ? &COMMANDS[index] : NULL;
}

/* Whether { list = name } in menus.lua names a generated list. */
static int menu_list_known(const char *name) {
    for (size_t i = 0; i < sizeof(LISTS) / sizeof(LISTS[0]); i++)
        if (strcmp(LISTS[i], name) == 0) return 1;
    return 0;
}

/* Appends "<what> '<name>'" to a newline-separated report. */
static char *report_unknown(char *report, const char *what, const char *name) {
    size_t used = report ? strlen(report) : 0;
    size_t needed = used + strlen(what) + strlen(name) + 32;
    char *grown = (char *)realloc(report, needed);
    if (!grown) return report;
    snprintf(grown + used, needed - used, "%sunknown %s '%s'", used ? "\n" : "", what, name);
    return grown;
}

static char *collect_problems(char *report, const PluginMenuItem *items, int count) {
    for (int i = 0; i < count; i++) {
        const PluginMenuItem *item = &items[i];
        if (item->kind == PLUGIN_MENU_COMMAND && !menu_command_find(item->name))
            report = report_unknown(report, "command", item->name);
        else if (item->kind == PLUGIN_MENU_LIST && !menu_list_known(item->name))
            report = report_unknown(report, "list", item->name);
        else if (item->kind == PLUGIN_MENU_SUBMENU)
            report = collect_problems(report, item->items, item->itemCount);
    }
    return report;
}

char *menu_definition_problems(const PluginMenuItem *items, int count) {
    return collect_problems(NULL, items, count);
}
