#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/plugin_host.h"
#include "../src/plugin_zip.h"
#include "../src/file_io.h"
#include "../src/path_utils.h"
#include "../third_party/scintilla/include/Scintilla.h"

static int s_ok = 1;

static void expect(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", what);
        s_ok = 0;
    }
}

static void set_document(HWND editor, const char *text) {
    SendMessageA(editor, SCI_SETTEXT, 0, (LPARAM)text);
    SendMessageA(editor, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessageA(editor, SCI_SETSAVEPOINT, 0, 0);
}

/* The whole document, however large, so a comparison never sees only its
   start. Caller frees; NULL when out of memory. */
static char *document_text(HWND editor) {
    size_t length = (size_t)SendMessageA(editor, SCI_GETLENGTH, 0, 0);
    char *text = (char *)malloc(length + 1);
    if (text) SendMessageA(editor, SCI_GETTEXT, length + 1, (LPARAM)text);
    return text;
}

static int document_is(HWND editor, const char *expected) {
    char *text = document_text(editor);
    int same = text && strcmp(text, expected) == 0;
    free(text);
    return same;
}

static int document_contains(HWND editor, const char *expected) {
    char *text = document_text(editor);
    int found = text && strstr(text, expected) != NULL;
    free(text);
    return found;
}

/* Loads a chunk that must succeed and returns the index of its first command. */
static int load_ok(const char *name, const char *code) {
    int first = plugin_host_command_count();
    char *error = plugin_host_load_chunk(name, code, strlen(code));
    if (error) fprintf(stderr, "%s\n", error);
    expect(error == NULL, name);
    free(error);
    return first;
}

static int run_fails(int index, HWND editor, const char *expectedText) {
    char *error = plugin_host_run(index, editor);
    int failed = error && strstr(error, expectedText);
    if (!failed) fprintf(stderr, "Unexpected result: %s\n", error ? error : "(success)");
    free(error);
    return failed;
}

static int run_ok(int index, HWND editor);

static void test_commands(HWND editor) {
    int first = load_ok("commands.lua",
        "menu.add('Upper', function() editor.replace_selection(editor.get_selection():upper()) end)\n"
        "menu.add('Number', function()\n"
        "  local out, n = {}, 0\n"
        "  for line in (editor.get_text() .. editor.eol()):gmatch('(.-)' .. editor.eol()) do\n"
        "    n = n + 1; out[n] = n .. ' ' .. line\n"
        "  end\n"
        "  editor.set_text(table.concat(out, editor.eol()))\n"
        "end)\n"
        "menu.add('Edit then fail', function() editor.set_text('partial'); error('boom') end)\n"
        "menu.add('Fail', function() error('nothing changed') end)\n");
    expect(plugin_host_command_count() == first + 4, "four commands registered");
    expect(strcmp(plugin_host_command_label(first), "Upper") == 0, "label kept");
    expect(plugin_host_command_label(first + 4) == NULL, "out-of-range label is NULL");

    set_document(editor, "abc def");
    SendMessageA(editor, SCI_SETSEL, 0, 3);
    char *error = plugin_host_run(first, editor);
    expect(!error && document_is(editor, "ABC def"), "replace_selection");
    free(error);
    SendMessageA(editor, SCI_UNDO, 0, 0);
    expect(document_is(editor, "abc def") && !SendMessageA(editor, SCI_CANUNDO, 0, 0),
           "a command is one undo step");

    SendMessageA(editor, SCI_SETEOLMODE, SC_EOL_LF, 0);
    set_document(editor, "a\nb");
    error = plugin_host_run(first + 1, editor);
    expect(!error && document_is(editor, "1 a\n2 b"), "get_text/set_text/eol");
    free(error);

    set_document(editor, "keep");
    expect(run_fails(first + 2, editor, "boom"), "error is reported");
    expect(document_is(editor, "keep"), "a failing command's edits are undone");
    expect(!SendMessageA(editor, SCI_GETMODIFY, 0, 0),
           "rolling back a failing command restores the save point");

    /* A failing command that changed nothing mustn't undo the user's edit. */
    set_document(editor, "");
    SendMessageA(editor, SCI_REPLACESEL, 0, (LPARAM)"typed");
    expect(run_fails(first + 3, editor, "nothing changed"), "error without edits is reported");
    expect(document_is(editor, "typed"), "the user's own edit survives");

    expect(run_fails(-1, editor, "no longer loaded"), "bad index rejected");
}

static void test_menu_enabled(HWND editor) {
    int first = load_ok("menu-enabled.lua",
        "menu.add('Needs selection', function() end, nil, function()\n"
        "  return editor.get_selection() ~= ''\n"
        "end)\n"
        "menu.add('Always', function() end)\n"
        "menu.add('Broken state', function() end, nil, function() error('state failed') end)\n");
    expect(plugin_host_command_has_enabled(first) &&
           !plugin_host_command_has_enabled(first + 1) &&
           plugin_host_command_has_enabled(first + 2),
           "menu.add keeps optional enabled predicates");
    int enabled = -1;
    set_document(editor, "text");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    char *error = plugin_host_command_enabled(first, editor, &enabled);
    expect(!error && !enabled, "menu command is disabled without a selection");
    free(error);
    SendMessageA(editor, SCI_SETSEL, 0, 2);
    error = plugin_host_command_enabled(first, editor, &enabled);
    expect(!error && enabled, "menu command is enabled with a selection");
    free(error);
    error = plugin_host_command_enabled(first + 1, editor, &enabled);
    expect(!error && enabled, "a menu command without a predicate is enabled");
    free(error);
    error = plugin_host_command_enabled(first + 2, editor, &enabled);
    expect(error && strstr(error, "state failed") && !enabled,
           "a failing menu predicate is reported and disables the command");
    free(error);

    int before = plugin_host_command_count();
    error = plugin_host_load_chunk("bad-enabled.lua",
        "menu.add('Bad', function() end, nil, true)", 42);
    expect(error && strstr(error, "function") && plugin_host_command_count() == before,
           "menu.add rejects a non-function enabled predicate atomically");
    free(error);
}

/* menu.separator entries keep their place among the commands, carry their
   menu path as the label, and run nothing. */
static void test_separators(HWND editor) {
    int first = load_ok("separators.lua",
        "menu.add('Lines/A', function() end)\n"
        "menu.separator('Lines')\n"
        "menu.add('Lines/B', function() end)\n"
        "menu.separator()\n"
        "menu.separator('/Snippets/Markdown')\n");
    expect(plugin_host_command_count() == first + 5, "separators are listed with the commands");
    expect(!plugin_host_command_is_separator(first) && plugin_host_command_is_separator(first + 1) &&
           !plugin_host_command_is_separator(first + 2), "separators keep their order");
    expect(strcmp(plugin_host_command_label(first + 1), "Lines") == 0, "a separator's label is its path");
    expect(strcmp(plugin_host_command_label(first + 3), "") == 0, "no path means the Plugins menu");
    expect(strcmp(plugin_host_command_label(first + 4), "/Snippets/Markdown") == 0, "menu-bar path kept");
    expect(!plugin_host_command_is_placed(first + 1) && !plugin_host_command_overrides(first + 1),
           "a separator is placed by its path");
    expect(!plugin_host_command_is_separator(first + 5), "out-of-range is no separator");
    set_document(editor, "same");
    char *error = plugin_host_run(first + 1, editor);
    expect(!error && document_is(editor, "same"), "running a separator does nothing");
    free(error);

    error = plugin_host_load_chunk("bad.lua", "menu.separator({})", 18);
    expect(error && strstr(error, "bad argument"), "a separator path must be a string");
    free(error);
}

static void test_context_menu(HWND editor) {
    int first = load_ok("context.lua",
        "local item, option, broken, badstate\n"
        "item = contextmenu.add('Formatting/Inline/Wrap in brackets', function()\n"
        "  editor.replace_selection('[' .. editor.get_selection() .. ']')\n"
        "end, function() return editor.get_selection() ~= '' end)\n"
        "option = contextmenu.checkbox('Formatting/Keep Brackets', 'keep_brackets', false, function(checked)\n"
        "  assert(type(checked) == 'boolean')\n"
        "end, function() return editor.get_selection() == '' end)\n"
        "broken = contextmenu.checkbox('Formatting/Broken Option', 'broken_option', false, function()\n"
        "  error('context checkbox failed')\n"
        "end)\n"
        "badstate = contextmenu.add('Formatting/Broken State', function() end, function()\n"
        "  error('context state failed')\n"
        "end)\n"
        "menu.add('Remove context item', function()\n"
        "  assert(contextmenu.remove(item))\n"
        "  assert(contextmenu.remove(option))\n"
        "  assert(contextmenu.remove(broken))\n"
        "  assert(contextmenu.remove(badstate))\n"
        "  assert(not contextmenu.remove(item))\n"
        "end)\n");
    expect(plugin_host_context_menu_item_count() == 4,
           "contextmenu.add and contextmenu.checkbox register items");
    expect(strcmp(plugin_host_context_menu_item_label(0),
                  "Formatting/Inline/Wrap in brackets") == 0,
           "context menu submenu path is kept");
    int id = plugin_host_context_menu_item_id(0);
    int checkboxId = plugin_host_context_menu_item_id(1);
    int brokenCheckboxId = plugin_host_context_menu_item_id(2);
    int brokenStateId = plugin_host_context_menu_item_id(3);
    expect(id > 0 && checkboxId > 0 && brokenCheckboxId > 0 &&
           brokenStateId > 0 && plugin_host_context_menu_item_id(4) == 0,
           "context menu items have stable ids");
    expect(!plugin_host_context_menu_item_is_checkable(0) &&
           plugin_host_context_menu_item_is_checkable(1) &&
           !plugin_host_context_menu_item_is_checked(1),
           "a context checkbox exposes its default state");

    set_document(editor, "word");
    int enabled = 0;
    char *error = plugin_host_context_menu_item_enabled(0, editor, &enabled);
    expect(!error && !enabled, "a context item predicate sees an empty selection");
    free(error);
    error = plugin_host_context_menu_item_enabled(1, editor, &enabled);
    expect(!error && enabled, "a checkbox predicate can enable without a selection");
    free(error);
    error = plugin_host_context_menu_item_enabled(3, editor, &enabled);
    expect(error && strstr(error, "context state failed") && !enabled,
           "a failing context item predicate disables the item and reports its error");
    free(error);

    SendMessageA(editor, SCI_SETSEL, 0, 4);
    error = plugin_host_context_menu_item_enabled(0, editor, &enabled);
    expect(!error && enabled, "a context item predicate sees selected text");
    free(error);
    error = plugin_host_context_menu_item_enabled(1, editor, &enabled);
    expect(!error && !enabled, "a checkbox predicate can disable for selected text");
    free(error);
    error = plugin_host_run_context_menu_item(id, editor);
    expect(!error && document_is(editor, "[word]"),
           "a context menu callback edits as one plugin command");
    free(error);

    error = plugin_host_run_context_menu_item(checkboxId, editor);
    expect(!error && plugin_host_context_menu_item_is_checked(1),
           "a context checkbox saves and exposes its toggled state");
    free(error);
    error = plugin_host_run_context_menu_item(checkboxId, editor);
    expect(!error && !plugin_host_context_menu_item_is_checked(1),
           "a context checkbox toggles off again");
    free(error);
    error = plugin_host_run_context_menu_item(brokenCheckboxId, editor);
    expect(error && strstr(error, "context checkbox failed") &&
           !plugin_host_context_menu_item_is_checked(2),
           "a failing context checkbox callback restores its old state");
    free(error);

    const char *tooDeep =
        "contextmenu.add('1/2/3/4/5/6/7/8/9', function() end)";
    error = plugin_host_load_chunk("deep-context.lua", tooDeep, strlen(tooDeep));
    expect(error && strstr(error, "too many parts"),
           "context menu nesting is limited to eight parts");
    free(error);

    char foreign[160];
    snprintf(foreign, sizeof(foreign),
             "menu.add('Foreign remove', function() assert(not contextmenu.remove(%d)) end)", id);
    int foreignCommand = load_ok("foreign.lua", foreign);
    expect(run_ok(foreignCommand, editor) && plugin_host_context_menu_item_count() == 4,
           "a plugin cannot remove another plugin's context item");

    int before = plugin_host_context_menu_item_count();
    const char *bad = "contextmenu.add('Temporary', function() end); error('stop')";
    error = plugin_host_load_chunk("bad-context.lua", bad, strlen(bad));
    expect(error && plugin_host_context_menu_item_count() == before,
           "a failed plugin load rolls back context menu items");
    free(error);

    expect(run_ok(first, editor) && plugin_host_context_menu_item_count() == 0,
           "contextmenu.remove removes its item and reports repeat removal");
    set_document(editor, "same");
    error = plugin_host_run_context_menu_item(id, editor);
    expect(!error && document_is(editor, "same"), "a removed context item no longer runs");
    free(error);
}

/* ---- stand-ins for the application's dialogs and document details ---- */
static char s_lastTitle[64], s_lastMessage[1024];
static char s_configPlugin[64], s_configKey[64], s_configValue[4096];
static char s_binaryName[64], s_binaryData[256];
static char s_binaryApplication[256], s_binaryArguments[256];
static size_t s_binaryLength;
static int s_binaryOpenAfterSave;
static int s_deferredToken;
static char s_documentEncoding[16] = "utf-8";
static const char *s_alignDelimiter = "=";
static const char *s_calculatorExpression =
    "0x10 + 0b11 + (5 << 1) + min(4, 9)";
static const char *s_z80EmulatorPath;
static const char *s_manualSql;
static const char *s_z80EmulatorArguments;

static char *fake_config_get(const char *plugin, const char *key) {
    const char *special = NULL;
    if (strcmp(plugin, "assemblers") == 0) {
        if (strcmp(key, "z80_emulator_path") == 0) special = s_z80EmulatorPath;
        else if (strcmp(key, "z80_emulator_arguments") == 0) special = s_z80EmulatorArguments;
    }
    if (special) {
        char *copy = (char *)malloc(strlen(special) + 1);
        if (copy) strcpy(copy, special);
        return copy;
    }
    if (strcmp(plugin, s_configPlugin) != 0 || strcmp(key, s_configKey) != 0 || !s_configValue[0])
        return NULL;
    size_t length = strlen(s_configValue) + 1;
    char *copy = (char *)malloc(length);
    if (copy) memcpy(copy, s_configValue, length);
    return copy;
}

static int fake_config_set(const char *plugin, const char *key, const char *value) {
    snprintf(s_configPlugin, sizeof(s_configPlugin), "%s", plugin);
    snprintf(s_configKey, sizeof(s_configKey), "%s", key);
    snprintf(s_configValue, sizeof(s_configValue), "%s", value ? value : "");
    return 1;
}

static char *fake_prompt(const char *title, const char *label, const char *initial) {
    snprintf(s_lastTitle, sizeof(s_lastTitle), "%s", title);
    if (strcmp(label, "cancel") == 0) return NULL;
    if (strcmp(label, "Keep events containing:") == 0) {
        char *reply = (char *)malloc(4);
        if (reply) strcpy(reply, "ADA");
        return reply;
    }
    if (strcmp(label, "Align the selected lines on:") == 0) {
        char *reply = (char *)malloc(strlen(s_alignDelimiter) + 1);
        if (reply) strcpy(reply, s_alignDelimiter);
        return reply;
    }
    if (strcmp(label, "Remove the lines containing:") == 0) {
        char *reply = (char *)malloc(5);
        if (reply) strcpy(reply, "drop");
        return reply;
    }
    if (strncmp(label, "Expression ", 11) == 0) {
        char *reply = (char *)malloc(strlen(s_calculatorExpression) + 1);
        if (reply) strcpy(reply, s_calculatorExpression);
        return reply;
    }
    if (strcmp(label, "New database file name:") == 0) {
        char *reply = (char *)malloc(16);
        if (reply) strcpy(reply, "test-browser.db");
        return reply;
    }
    if (strcmp(label, "Table name:") == 0) {
        char *reply = (char *)malloc(7);
        if (reply) strcpy(reply, "people");
        return reply;
    }
    if (strcmp(label, "SQL to run:") == 0) {
        if (!s_manualSql) return NULL;
        char *reply = (char *)malloc(strlen(s_manualSql) + 1);
        if (reply) strcpy(reply, s_manualSql);
        return reply;
    }
    char *reply = (char *)malloc(strlen(label) + strlen(initial) + 16);
    if (reply) sprintf(reply, "reply to %s %s", label, initial);
    return reply;
}

static int fake_choose(const char *title, const char *label, const char *const *items, int count) {
    snprintf(s_lastTitle, sizeof(s_lastTitle), "%s", title);
    if (strstr(label, " = ") && count == 2 && strcmp(items[0], "Insert result") == 0)
        return 0;
    if (strcmp(label, "Include headings at:") == 0 && count == 4) return 1;
    if (strcmp(label, "Keep events at or above:") == 0 && count == 6) return 4;
    if ((strstr(label, "which key?") || strstr(label, "which table?")) && count >= 1) return 0;
    return strcmp(label, "Level:") == 0 && count == 3 && strcmp(items[1], "h2") == 0 ? 1 : -1;
}

static int fake_save_binary(const char *title, const char *defaultName,
                            const char *data, size_t length, int openAfterSave,
                            const char *application, const char *arguments) {
    snprintf(s_lastTitle, sizeof(s_lastTitle), "%s", title);
    if (strcmp(defaultName, "cancel.tap") == 0) return 0;
    snprintf(s_binaryName, sizeof(s_binaryName), "%s", defaultName);
    s_binaryLength = length < sizeof(s_binaryData) ? length : sizeof(s_binaryData);
    memcpy(s_binaryData, data, s_binaryLength);
    s_binaryOpenAfterSave = openAfterSave;
    snprintf(s_binaryApplication, sizeof(s_binaryApplication), "%s", application ? application : "");
    snprintf(s_binaryArguments, sizeof(s_binaryArguments), "%s", arguments ? arguments : "");
    if (strcmp(defaultName, "open-fail.tap") == 0) return -2;
    return 1;
}

static int fake_schedule_deferred(int token, unsigned delayMs, int cancel) {
    (void)delayMs;
    if (cancel) { if (s_deferredToken == token) s_deferredToken = 0; return 1; }
    s_deferredToken = token;
    return 1;
}

static void fake_message(const char *title, const char *text, const char *kind) {
    (void)kind;
    snprintf(s_lastTitle, sizeof(s_lastTitle), "%s", title);
    snprintf(s_lastMessage, sizeof(s_lastMessage), "%s", text);
}

static void fake_status(const char *text) {
    snprintf(s_lastMessage, sizeof(s_lastMessage), "%s", text);
}

static char *fake_file_path(void) {
    const char *path = "C:\\docs\\note.md";
    char *copy = (char *)malloc(strlen(path) + 1);
    if (copy) strcpy(copy, path);
    return copy;
}

static const char *fake_language(void) {
    return "markdown";
}

static const char *fake_document_encoding(void) {
    return s_documentEncoding;
}

static int fake_document_set_encoding(const char *encoding) {
    snprintf(s_documentEncoding, sizeof(s_documentEncoding), "%s", encoding);
    return 1;
}

static int run_ok(int index, HWND editor) {
    char *error = plugin_host_run(index, editor);
    if (error) fprintf(stderr, "%s\n", error);
    free(error);
    return error == NULL;
}

static void test_checkboxes_and_config(HWND editor) {
    s_configPlugin[0] = s_configKey[0] = s_configValue[0] = 0;
    int first = load_ok("checkbox.lua",
        "assert(config.get('enabled', false) == false)\n"
        "menu.checkbox('Options/Enabled', 'enabled', false, function(checked)\n"
        "  assert(checked == true)\n"
        "end)\n"
        "menu.add('Options/Save Value', function()\n"
        "  config.set('answer', 42)\n"
        "  assert(config.get('answer', 0) == 42)\n"
        "  config.set('options', {enabled=true, names={'one','two'}, nested={count=3}})\n"
        "  local value = config.get('options', {})\n"
        "  assert(value.enabled and value.names[2] == 'two' and value.nested.count == 3)\n"
        "  -- Large enough to grow the encoder's buffers, and nested near the\n"
        "  -- depth limit, so stack misuse in the encoder or decoder shows up.\n"
        "  local deep = 'bottom'\n"
        "  for _ = 1, 30 do deep = { deep } end\n"
        "  config.set('big', { text = string.rep('x', 1000), list = { string.rep('y', 600) }, deep = deep })\n"
        "  local big = config.get('big', {})\n"
        "  assert(big.text == string.rep('x', 1000) and big.list[1] == string.rep('y', 600))\n"
        "  for _ = 1, 30 do big.deep = big.deep[1] end\n"
        "  assert(big.deep == 'bottom')\n"
        "  config.set('options', {enabled=true, names={'one','two'}, nested={count=3}})\n"
        "end)\n");
    expect(plugin_host_command_is_checkable(first), "checkbox command is checkable");
    expect(!plugin_host_command_is_checked(first), "checkbox uses its default state");
    expect(run_ok(first, editor), "checkbox command runs with its new state");
    expect(plugin_host_command_is_checked(first) && strcmp(s_configPlugin, "checkbox.lua") == 0 &&
           strcmp(s_configKey, "enabled") == 0 && strcmp(s_configValue, "1") == 0,
           "checkbox state is persisted in its plugin namespace");
    expect(run_ok(first + 1, editor) && strcmp(s_configKey, "options") == 0 &&
           strncmp(s_configValue, "@ME1:", 5) == 0,
           "config.set persists primitive and structured values");

    int failing = load_ok("bad_checkbox.lua",
        "menu.checkbox('Broken', 'enabled', true, function() error('nope') end)");
    expect(run_fails(failing, editor, "nope"), "failing checkbox command reports its error");
    expect(plugin_host_command_is_checked(failing), "a failing checkbox command restores its setting");
}

static void test_editor_api(HWND editor) {
    int first = load_ok("api.lua",
        "menu.add('Lines', function()\n"
        "  assert(editor.line_count() == 3 and editor.get_line(2) == 'two')\n"
        "  assert(editor.tab_width() > 0)\n"
        "  local s, f = editor.line_range(2)\n"
        "  assert(s == 5 and f == 7 and editor.get_text():sub(s, f) == 'two')\n"
        "  editor.set_selection(s, f)\n"
        "  assert(editor.get_selection() == 'two' and editor.current_line() == 2)\n"
        "  local a, b = editor.get_selection_range()\n"
        "  assert(a == 5 and b == 7)\n"
        "  editor.replace_selection('TWO')\n"
        "  assert(editor.get_caret() == 8, 'caret after replaced text')\n"
        "  editor.replace_range(1, 3, 'ONE')\n"
        "  editor.insert('>', 1)\n"
        "  editor.set_caret(#editor.get_text() + 1)\n"
        "  editor.insert('!')\n"
        "  editor.set_selection(3, 2)\n"
        "  assert(editor.get_selection() == '', 'empty range')\n"
        "end)\n"
        "menu.add('Bounds', function()\n"
        "  assert(not pcall(editor.get_line, 4), 'line past the end')\n"
        "  assert(not pcall(editor.get_line, 0), 'line zero')\n"
        "  assert(not pcall(editor.set_selection, 3, 1), 'finish before start')\n"
        "  assert(not pcall(editor.replace_range, 0, 1, 'x'), 'start before the document')\n"
        "  assert(not pcall(editor.set_caret, #editor.get_text() + 2), 'caret past the end')\n"
        "end)\n"
        "menu.add('Binary', function() editor.replace_range(1, 0, 'a\\0b') end)\n"
        "menu.add('Document', function()\n"
        "  assert(editor.file_path() == 'C:\\\\docs\\\\note.md')\n"
        "  assert(editor.language() == 'markdown')\n"
        "end)\n"
        "menu.add('Undo balance', function() editor.begin_undo(); editor.insert('nested ') end)\n"
        "menu.add('Multiple selections', function()\n"
        "  assert(editor.selection_count() == 1)\n"
        "  editor.set_selection(1, 4)\n"
        "  local second = editor.add_selection(10, 12)\n"
        "  assert(second == 2 and editor.selection_count() == 2)\n"
        "  local all = editor.get_selections()\n"
        "  assert(#all == 2 and all[1].start == 1 and all[1].finish == 4)\n"
        "  assert(all[2].caret == 13 and all[2].anchor == 10)\n"
        "  assert(editor.get_selection(1) == 'zero' and editor.get_selection(2) == 'two')\n"
        "  local s, f = editor.get_selection_range(2)\n"
        "  assert(s == 10 and f == 12 and editor.get_caret(2) == 13)\n"
        "  editor.replace_selection('TWO', 2)\n"
        "  assert(editor.selection_count() == 2 and editor.get_caret(2) == 13)\n"
        "  editor.set_selection(6, 8, 1)\n"
        "  editor.set_main_selection(2)\n"
        "  assert(editor.get_main_selection() == 2 and editor.get_selection() == '')\n"
        "  editor.set_caret(12, 2)\n"
        "  assert(editor.get_caret() == 12 and editor.selection_count() == 2)\n"
        "  editor.drop_selection(1)\n"
        "  assert(editor.selection_count() == 1 and editor.get_main_selection() == 1)\n"
        "  assert(not pcall(editor.drop_selection, 1), 'cannot drop only selection')\n"
        "  assert(not pcall(editor.get_caret, 2), 'selection past the end')\n"
        "end)\n");

    SendMessageA(editor, SCI_SETEOLMODE, SC_EOL_LF, 0);
    set_document(editor, "one\ntwo\nthree");
    expect(run_ok(first, editor) && document_is(editor, ">ONE\nTWO\nthree!"), "line and range editing");
    set_document(editor, "one\ntwo\nthree");
    expect(run_ok(first + 1, editor) && document_is(editor, "one\ntwo\nthree"), "out-of-range arguments raise errors");
    set_document(editor, "");
    expect(run_ok(first + 2, editor) && SendMessageA(editor, SCI_GETLENGTH, 0, 0) == 3,
           "text containing NUL is written whole");
    expect(run_ok(first + 3, editor), "file_path and language");
    set_document(editor, "plain");
    expect(run_ok(first + 4, editor) && document_is(editor, "nested plain"),
           "unmatched explicit undo groups are closed after a callback");
    SendMessageA(editor, SCI_UNDO, 0, 0);
    expect(document_is(editor, "plain"), "explicit undo nesting remains one command undo step");
    set_document(editor, "zero one two");
    expect(run_ok(first + 5, editor) && document_is(editor, "zero one TWO"),
           "multiple selections and carets through the Lua API");

    int bulk = load_ok("bulk-selections.lua",
        "menu.add('Bulk selections', function()\n"
        "  assert(editor.selection_count() == 3)\n"
        "  assert(not pcall(editor.replace_selections, {'only one'}))\n"
        "  assert(not pcall(editor.replace_selections, {'A', 2, 'C'}))\n"
        "  editor.replace_selections({'A', 'middle', 'C'})\n"
        "  assert(editor.selection_count() == 3)\n"
        "  local all = editor.get_selections()\n"
        "  assert(all[1].caret == 2 and all[2].caret == 9 and all[3].caret == 11)\n"
        "  assert(editor.get_main_selection() == 2)\n"
        "  editor.replace_selections('!')\n"
        "  assert(editor.selection_count() == 3 and editor.get_main_selection() == 2)\n"
        "end)");
    set_document(editor, "aa bb cc");
    SendMessageA(editor, SCI_SETSEL, 0, 2);
    SendMessageA(editor, SCI_ADDSELECTION, 5, 3);
    SendMessageA(editor, SCI_ADDSELECTION, 8, 6);
    SendMessageA(editor, SCI_SETMAINSELECTION, 1, 0);
    expect(run_ok(bulk, editor) && document_is(editor, "A! middle! C!") &&
           SendMessageA(editor, SCI_GETSELECTIONS, 0, 0) == 3,
           "bulk replacement keeps multiple carets stable");

    int positions = load_ok("find-scroll.lua",
        "menu.add('Find and scroll', function()\n"
        "  local s, f = editor.find('abc', 1)\n"
        "  assert(s == 1 and f == 3, 'find from the first position')\n"
        "  s, f = editor.find('abc', 2)\n"
        "  assert(s == 4 and f == 6, 'find starts at the given position')\n"
        "  assert(editor.find('abc', 5) == nil, 'find skips matches before the start')\n"
        "  editor.scroll_to(4)\n"
        "  assert(editor.get_caret() == 4, 'scroll_to moves the caret to the position')\n"
        "  editor.scroll_to(1)\n"
        "  assert(editor.get_caret() == 1, 'scroll_to the first position')\n"
        "end)");
    set_document(editor, "abcabc");
    expect(run_ok(positions, editor), "find and scroll_to take 1-based positions");

    int regex = load_ok("find-regex.lua",
        "menu.add('Regex find', function()\n"
        "  local s, f = editor.find('a+b', 1, {regex = true})\n"
        "  assert(s == 1 and f == 4, 'a harmless regex still matches')\n"
        "end)\n"
        "menu.add('Catastrophic regex', function()\n"
        "  editor.find('(a+)+b', 1, {regex = true})\n"
        "end)\n"
        "menu.add('Catastrophic regex, star', function()\n"
        "  editor.find('([ab]*)*b', 1, {regex = true})\n"
        "end)\n");
    set_document(editor, "aaab");
    expect(run_ok(regex, editor), "a non-catastrophic regex pattern is still allowed");
    expect(run_fails(regex + 1, editor, "catastrophic backtracking"),
           "a classic catastrophic-backtracking pattern is rejected before it reaches Scintilla");
    expect(run_fails(regex + 2, editor, "catastrophic backtracking"),
           "a star-quantified nested group is also rejected");
}

static void test_zip_api(HWND editor) {
    int first = load_ok("zip.lua",
        "menu.add('Round trip', function()\n"
        "  local bytes, err = zip.pack({\n"
        "    { name = 'a.txt', data = 'hello' },\n"
        "    { name = 'dir/b.txt', data = 'world' },\n"
        "  })\n"
        "  assert(bytes and not err, err)\n"
        "  assert(#bytes > 0)\n"
        "  local entries, uerr = zip.unpack(bytes)\n"
        "  assert(entries and not uerr, uerr)\n"
        "  assert(#entries == 2, 'expected 2 entries, got ' .. #entries)\n"
        "  local byName = {}\n"
        "  for _, e in ipairs(entries) do byName[e.name] = e.data end\n"
        "  assert(byName['a.txt'] == 'hello')\n"
        "  assert(byName['dir/b.txt'] == 'world')\n"
        "end)\n"
        /* Not round-tripped through zip.unpack: ziptool.lua's unzip_file()
           treats any central directory under 46 bytes as truncated, with no
           exception for the legitimately-0-byte one a zero-entry archive
           has - an upstream quirk, not a bug in this integration, so this
           only checks the pack side of the empty case. */
        "menu.add('Empty archive', function()\n"
        "  local bytes = assert(zip.pack({}))\n"
        "  assert(#bytes > 0)\n"
        "end)\n"
        "menu.add('Rejects non-table', function()\n"
        "  assert(not pcall(zip.pack, 'nope'))\n"
        "  assert(not pcall(zip.unpack, {}))\n" /* a number would coerce to a string and just fail gracefully instead */
        "end)\n"
        "menu.add('Unpack rejects garbage', function()\n"
        "  local entries, err = zip.unpack('not a zip file')\n"
        "  assert(entries == nil and err)\n"
        "end)\n");
    expect(run_ok(first, editor), "zip.pack/zip.unpack round-trip preserves names and bytes");
    expect(run_ok(first + 1, editor), "zip.pack/zip.unpack handle an empty archive");
    expect(run_ok(first + 2, editor), "zip.pack/zip.unpack validate their argument types");
    expect(run_ok(first + 3, editor), "zip.unpack reports an error instead of crashing on non-zip input");
}

/* sqlite.open() really does touch disk (plugin_sqlite.c's one deliberate
   exception to the sandbox); back it with a real temp file rather than
   stubbing anything. */
static void test_sqlite_api(HWND editor) {
    WCHAR tempRoot[MAX_PATH], dbPath[MAX_PATH];
    expect(GetTempPathW(MAX_PATH, tempRoot) > 0, "temp path available");
    expect(GetTempFileNameW(tempRoot, L"sql", 0, dbPath) != 0, "temp db name reserved");
    DeleteFileW(dbPath); /* sqlite.open() should create it fresh */
    char *dbPathUtf8 = to_utf8(dbPath);
    for (char *c = dbPathUtf8; *c; c++)
        if (*c == '\\') *c = '/'; /* avoids escaping backslashes in the Lua literal below */

    char script[4096];
    snprintf(script, sizeof(script),
        "menu.add('Round trip', function()\n"
        "  local db, err = sqlite.open('%s')\n"
        "  assert(db and not err, err)\n"
        "  assert(db:exec('create table notes (id integer primary key, text text)'))\n"
        "  local ok, changed = db:exec('insert into notes (text) values (?)', 'hello')\n"
        "  assert(ok and changed == 1, 'one row changed')\n"
        "  db:exec('insert into notes (text) values (?)', 'world')\n"
        "  local rows, columns = db:query('select id, text from notes order by id')\n"
        "  assert(rows and columns, 'query returns rows and column names')\n"
        "  assert(#columns == 2 and columns[1] == 'id' and columns[2] == 'text',"
        " 'columns come back in declared order')\n"
        "  assert(#rows == 2)\n"
        "  assert(rows[1].id == 1 and rows[1].text == 'hello')\n"
        "  assert(rows[2].id == 2 and rows[2].text == 'world')\n"
        "  db:close()\n"
        "end)\n"
        "menu.add('Reopens persisted data', function()\n"
        "  local db = assert(sqlite.open('%s'))\n"
        "  local rows = assert(db:query('select count(*) as n from notes'))\n"
        "  assert(rows[1].n == 2, 'data survived close/reopen')\n"
        "  db:close()\n"
        "end)\n"
        "menu.add('Closed database errors', function()\n"
        "  local db = assert(sqlite.open('%s'))\n"
        "  db:close()\n"
        "  assert(not pcall(function() db:exec('select 1') end))\n"
        "end)\n"
        "menu.add('Query error is reported, not raised', function()\n"
        "  local db = assert(sqlite.open('%s'))\n"
        "  local rows, err = db:query('select * from no_such_table')\n"
        "  assert(rows == nil and err)\n"
        "  db:close()\n"
        "end)\n",
        dbPathUtf8, dbPathUtf8, dbPathUtf8, dbPathUtf8);
    int first = load_ok("sqlite.lua", script);
    expect(run_ok(first, editor), "sqlite.open/exec/query round-trip a real database file");
    expect(run_ok(first + 1, editor), "data written by one connection is visible after reopening");
    expect(run_ok(first + 2, editor), "a method on a closed database raises an error");
    expect(run_ok(first + 3, editor), "a failing query returns nil, err instead of raising");

    free(dbPathUtf8);
    DeleteFileW(dbPath);
}

static void test_ui(HWND editor) {
    int first = load_ok("ui.lua",
        "menu.add('Ask', function()\n"
        "  assert(ui.prompt('Width:', '80') == 'reply to Width: 80')\n"
        "  assert(ui.prompt('cancel') == nil, 'cancel gives nil')\n"
        "  local i, item = ui.choose({'h1', 'h2', 'h3'}, 'Level:')\n"
        "  assert(i == 2 and item == 'h2')\n"
        "  assert(ui.choose({'x'}) == nil, 'cancelled choice gives nil')\n"
        "  assert(not pcall(ui.choose, {}), 'no items')\n"
        "  assert(not pcall(ui.choose, {'a', 5}), 'non-string item')\n"
        "end)\n"
        "menu.add('Prompt', function() ui.prompt('Width:') end)\n"
        "menu.add('Binary', function()\n"
        "  assert(ui.save_binary('output.tap', string.char(0, 1, 255)))\n"
        "  assert(ui.save_binary('cancel.tap', 'x') == false)\n"
        "end)\n"
        "menu.add('Binary and open', function()\n"
        "  assert(ui.save_binary('run.tap', 'run', nil, true))\n"
        "end)\n"
        "menu.add('Binary and configured runner', function()\n"
        "  assert(ui.save_binary('run.tap', 'run', nil, true, 'C:/Emu/emu.exe', '--fast'))\n"
        "end)\n"
        "menu.add('Binary open failure', function()\n"
        "  ui.save_binary('open-fail.tap', 'x', nil, true)\n"
        "end)\n");
    expect(run_ok(first, editor), "ui.prompt and ui.choose");
    expect(strcmp(s_lastTitle, "Ask") == 0, "dialogs are titled with the command label");
    expect(run_ok(first + 2, editor) && strcmp(s_binaryName, "output.tap") == 0 &&
           s_binaryLength == 3 && (unsigned char)s_binaryData[0] == 0 &&
           (unsigned char)s_binaryData[1] == 1 && (unsigned char)s_binaryData[2] == 255,
           "ui.save_binary preserves every byte and reports cancellation");
    s_binaryOpenAfterSave = 0;
    expect(run_ok(first + 3, editor) && strcmp(s_binaryName, "run.tap") == 0 &&
           s_binaryOpenAfterSave, "ui.save_binary can open the saved file by association");
    expect(run_ok(first + 4, editor) && strcmp(s_binaryApplication, "C:/Emu/emu.exe") == 0 &&
           strcmp(s_binaryArguments, "--fast") == 0,
           "ui.save_binary can run a configured application after saving");
    expect(run_fails(first + 5, editor, "was saved but could not be opened"),
           "ui.save_binary distinguishes an association launch failure");

    plugin_host_set_callbacks(NULL);
    expect(run_fails(first + 1, editor, "not available"), "missing callbacks raise errors");
    expect(run_fails(first + 2, editor, "not available"), "missing binary callback raises an error");
}

static void test_deferred(HWND editor) {
    static const PluginHostCallbacks callbacks = { .schedule_deferred = fake_schedule_deferred };
    plugin_host_set_callbacks(&callbacks);
    s_deferredToken = 0;
    int first = load_ok("defer.lua",
        "menu.add('Defer', function()\n"
        "  app.defer(10, function() editor.insert('later') end)\n"
        "end)\n");
    set_document(editor, "now ");
    SendMessageA(editor, SCI_GOTOPOS, 4, 0);
    expect(run_ok(first, editor) && s_deferredToken > 0, "app.defer schedules a callback");
    char *error = plugin_host_run_deferred(s_deferredToken, editor);
    expect(!error && document_is(editor, "now later"), "a deferred callback runs on the editor");
    free(error);
    s_deferredToken = 0;
}

static HWND s_reentryEditor;
static int s_reentryBlocked;

/* Stands in for a dialog whose message loop delivers an event and a menu
   command while a plugin is still running. */
static char *reentrant_prompt(const char *title, const char *label, const char *initial) {
    (void)title; (void)label; (void)initial;
    char *fired = plugin_host_fire("save", NULL, s_reentryEditor);
    char *run = plugin_host_run(0, s_reentryEditor);
    s_reentryBlocked = fired == NULL && run && strstr(run, "still running");
    free(fired);
    free(run);
    return NULL;
}

static void test_events(HWND editor) {
    plugin_host_shutdown();
    int first = load_ok("events.lua",
        "saves = {}\n"
        "events.on('save', function(path) saves[#saves + 1] = path; editor.insert('saved ', 1) end)\n"
        "events.on('save', function() editor.insert('lost'); error('second handler fails') end)\n"
        "events.on('open', function(path) assert(path == nil) end)\n"
        "menu.add('Saves', function() assert(#saves == 1 and saves[1] == 'C:\\\\a.md') end)\n"
        "menu.add('Reenter', function() ui.prompt('x') end)\n"
        "local removed = events.on('open', function() error('removed handler ran') end)\n"
        "assert(events.off(removed))\n"
        "local self, calls = nil, 0\n"
        "self = events.on('open', function() calls = calls + 1; events.off(self) end)\n"
        "events.on('open', function() calls = calls + 1 end)\n"
        "menu.add('Event removal', function() assert(calls == 3) end)\n");

    set_document(editor, "x");
    char *report = plugin_host_fire("save", "C:\\a.md", editor);
    expect(report && strstr(report, "second handler fails"), "a failing handler is reported");
    expect(document_is(editor, "saved x"), "handlers edit; a failing one's edits are undone");
    free(report);
    expect(run_ok(first, editor), "handlers receive the path");
    expect(plugin_host_fire("open", NULL, editor) == NULL, "nil path for an unsaved document");
    expect(plugin_host_fire("open", NULL, editor) == NULL, "self-removing handlers do not corrupt dispatch");
    expect(run_ok(first + 2, editor), "event removal preserves later handlers");
    expect(plugin_host_fire("tab_changed", NULL, editor) == NULL, "no handlers, no report");

    const char *unknown = "events.on('not_an_event', function() end)";
    char *error = plugin_host_load_chunk("unknown.lua", unknown, strlen(unknown));
    expect(error && strstr(error, "unknown event"), "unknown event names are rejected");
    free(error);
    const char *partial = "events.on('open', function() error('should not run') end)\nerror('late')\n";
    error = plugin_host_load_chunk("partial_events.lua", partial, strlen(partial));
    free(error);
    expect(plugin_host_fire("open", NULL, editor) == NULL, "a failed script's handlers are dropped");

    static const PluginHostCallbacks reentrant = {
        .prompt = reentrant_prompt, .config_get = fake_config_get, .config_set = fake_config_set
    };
    plugin_host_set_callbacks(&reentrant);
    s_reentryEditor = editor;
    expect(run_ok(first + 1, editor) && s_reentryBlocked, "no re-entry while a plugin runs");
    plugin_host_set_callbacks(NULL);
}

static void test_load_failures(void) {
    int before = plugin_host_command_count();
    char *error = plugin_host_load_chunk("syntax.lua", "menu.add(", 9);
    expect(error && strstr(error, "syntax.lua"), "syntax error names the script");
    free(error);

    const char *badShortcut = "menu.add('Bad', function() end, 'Shift+Q')";
    error = plugin_host_load_chunk("shortcut.lua", badShortcut, strlen(badShortcut));
    expect(error && strstr(error, "shortcut needs Ctrl or Alt"),
           "plugin shortcuts require a safe modifier and supported key");
    free(error);
    const char *partial = "menu.add('Lost', function() end)\nerror('late failure')\n";
    error = plugin_host_load_chunk("partial.lua", partial, strlen(partial));
    expect(error && strstr(error, "late failure"), "runtime error at load is reported");
    expect(plugin_host_command_count() == before, "a failed script adds no commands");
    free(error);
    const char *outside = "editor.get_text()";
    error = plugin_host_load_chunk("outside.lua", outside, strlen(outside));
    expect(error && strstr(error, "only available while"), "editor API needs a running command");
    free(error);

    const char *promptOutside = "ui.prompt('x')";
    error = plugin_host_load_chunk("prompt_outside.lua", promptOutside, strlen(promptOutside));
    expect(error && strstr(error, "only available while"), "ui.prompt needs a running command");
    free(error);
    const char *chooseOutside = "ui.choose({'x'})";
    error = plugin_host_load_chunk("choose_outside.lua", chooseOutside, strlen(chooseOutside));
    expect(error && strstr(error, "only available while"), "ui.choose needs a running command");
    free(error);

    const char *nulName =
        "commands.register('file.save' .. string.char(0) .. 'suffix', function() end)";
    error = plugin_host_load_chunk("nul_name.lua", nulName, strlen(nulName));
    expect(error && strstr(error, "must not contain NUL bytes"),
           "C-string API arguments reject embedded NUL bytes");
    expect(plugin_host_command_count() == before, "a rejected NUL name registers no command");
    free(error);

    const char *invalidUtf8 = "menu.add(string.char(255), function() end)";
    error = plugin_host_load_chunk("invalid_utf8.lua", invalidUtf8, strlen(invalidUtf8));
    expect(error && strstr(error, "must be valid UTF-8"),
           "labels constructed with invalid UTF-8 are rejected");
    expect(plugin_host_command_count() == before, "a rejected UTF-8 label registers no command");
    free(error);
}

static void test_environment(void) {
    load_ok("environment.lua",
        "assert(io == nil and os == nil and package == nil and debug == nil)\n"
        "assert(dofile == nil and loadfile == nil)\n"
        "assert(load('return 1')() == 1)\n"
        "assert(load('return string.upper(\"a\")')() == 'A', 'load keeps the globals without an env')\n"
        "assert(load(string.dump(function() end)) == nil, 'binary chunks refused')\n"
        "assert(not pcall(require, 'outside'), 'single-file plugins cannot require files')\n"
        "local ok, err = pcall(commands.execute, 'run.run_file')\n"
        "assert(not ok and err:find('can\\'t run'), 'plugins cannot run or debug files')\n");

    load_ok("clock.lua",
        "assert(app.date():match('^%d%d%d%d%-%d%d%-%d%d$'))\n"
        "assert(app.time():match('^%d%d:%d%d:%d%d$'))\n"
        "assert(app.date_time():match('^%d%d%d%d%-%d%d%-%d%d %d%d:%d%d:%d%d$'))\n");
    load_ok("metadata.lua",
        "assert(app.api_version() >= 9 and app.platform() == 'windows')\n"
        "assert(type(app.version()) == 'string' and app.memory_used() > 0)\n"
        "local c = app.capabilities(); assert(c.plugin_environments and c.lazy_require and c.save_and_open and c.configured_runner and c.menu_enabled and c.terminal_panel)\n"
        "assert(c.eol_conversion and c.save_encoding and c.tab_width)\n");
}

static void test_isolation(HWND editor) {
    plugin_host_shutdown();
    int first = load_ok("isolated-a.lua",
        "private_value = 'a'; string.plugin_change = true\n"
        "menu.add('Isolated A', function() assert(private_value == 'a' and string.plugin_change) end)");
    int second = load_ok("isolated-b.lua",
        "assert(private_value == nil and string.plugin_change == nil)\n"
        "private_value = 'b'\n"
        "menu.add('Isolated B', function() assert(private_value == 'b') end)");
    expect(run_ok(first, editor) && run_ok(second, editor),
           "plugins have isolated globals and library tables");

    load_ok("string-a.lua",
        "string.upper = function() return 'changed' end\n"
        "assert(getmetatable('') == false, 'the string metatable is hidden')\n"
        "assert(not pcall(function() getmetatable('').__index.upper = string.upper end))\n"
        "assert(('x'):upper() == 'X', 'string methods use the original library')\n");
    int third = load_ok("string-b.lua",
        "menu.add('String B', function()\n"
        "  assert(string.upper('x') == 'X' and ('x'):upper() == 'X')\n"
        "end)");
    expect(run_ok(third, editor), "a plugin can't change the string library for others");
}

static int s_limitTerminalEnds;
static void fake_limit_terminal_end(void) { s_limitTerminalEnds++; }

static void test_time_limit(void) {
    static const PluginHostCallbacks callbacks = { .terminal_end = fake_limit_terminal_end };
    plugin_host_set_callbacks(&callbacks);
    int endsBefore = s_limitTerminalEnds;
    const char *loop = "while true do end";
    char *error = plugin_host_load_chunk("loop.lua", loop, strlen(loop));
    expect(error && strstr(error, "stopped after") && s_limitTerminalEnds == endsBefore + 1,
           "runaway script is stopped and closes an in-process terminal session");
    free(error);
    /* Neither pcall, xpcall nor coroutine.resume can catch the limit's error. */
    const char *caught =
        "while true do\n"
        "  xpcall(function()\n"
        "    pcall(function()\n"
        "      while true do coroutine.resume(coroutine.create(function() while true do end end)) end\n"
        "    end)\n"
        "  end, function(e) return e end)\n"
        "end";
    error = plugin_host_load_chunk("caught.lua", caught, strlen(caught));
    expect(error && strstr(error, "stopped after") && s_limitTerminalEnds == endsBefore + 2,
           "a script can't catch the time limit");
    free(error);
    const char *catches = "assert(not pcall(error, 'x')) assert(not coroutine.resume(coroutine.create(error)))";
    error = plugin_host_load_chunk("catches.lua", catches, strlen(catches));
    expect(!error, "pcall and coroutine.resume still catch ordinary errors after a stop");
    free(error);
    const char *allocation = "return string.rep('x', 70 * 1024 * 1024)";
    error = plugin_host_load_chunk("memory.lua", allocation, strlen(allocation));
    expect(error && (strstr(error, "memory") || strstr(error, "allocate")),
           "plugin heap exhaustion is reported instead of exhausting the process");
    free(error);
    plugin_host_set_callbacks(NULL);
}

static char *s_relativeFileDocPath;
static char *fake_relative_file_doc_path(void) {
    if (!s_relativeFileDocPath) return NULL;
    size_t length = strlen(s_relativeFileDocPath) + 1;
    char *copy = (char *)malloc(length);
    if (copy) memcpy(copy, s_relativeFileDocPath, length);
    return copy;
}

/* editor.read_relative_file(), used by the bundled assemblers' INCLUDE: reads
   a real sibling file (and one reached through a subfolder or ".."), and
   rejects an absolute path or an unsaved document without touching the
   filesystem outside the document's own folder. */
static void test_read_relative_file(HWND editor) {
    WCHAR tempRoot[MAX_PATH], base[MAX_PATH], subdir[MAX_PATH];
    expect(GetTempPathW(MAX_PATH, tempRoot) > 0, "temp path available");
    expect(GetTempFileNameW(tempRoot, L"rrf", 0, base) != 0, "temp name reserved");
    DeleteFileW(base);
    expect(CreateDirectoryW(base, NULL), "temp directory created");
    swprintf(subdir, MAX_PATH, L"%ls\\sub", base);
    CreateDirectoryW(subdir, NULL);

    WCHAR siblingPath[MAX_PATH], nestedPath[MAX_PATH], mainPath[MAX_PATH];
    swprintf(siblingPath, MAX_PATH, L"%ls\\lib.asm", base);
    swprintf(nestedPath, MAX_PATH, L"%ls\\shared.asm", subdir);
    swprintf(mainPath, MAX_PATH, L"%ls\\main.asm", base);
    write_bytes_file(siblingPath, "sibling content", 15);
    write_bytes_file(nestedPath, "nested content", 14);

    char *mainPathUtf8 = to_utf8(mainPath);
    s_relativeFileDocPath = mainPathUtf8;
    PluginHostCallbacks callbacks = {0};
    callbacks.file_path = fake_relative_file_doc_path;
    plugin_host_set_callbacks(&callbacks);

    int index = load_ok("read-relative.lua",
        "menu.add('Sibling', function()\n"
        "  local text, err = editor.read_relative_file('lib.asm')\n"
        "  assert(text == 'sibling content' and err == nil)\n"
        "end)\n"
        "menu.add('Nested', function()\n"
        "  assert(editor.read_relative_file('sub/shared.asm') == 'nested content')\n"
        "end)\n"
        "menu.add('Dotdot', function()\n"
        "  assert(editor.read_relative_file('sub/../lib.asm') == 'sibling content')\n"
        "end)\n"
        "menu.add('Absolute', function()\n"
        "  local text, err = editor.read_relative_file('C:\\\\Windows\\\\win.ini')\n"
        "  assert(text == nil and err:find('absolute', 1, true))\n"
        "end)\n"
        "menu.add('Missing', function()\n"
        "  local text, err = editor.read_relative_file('nope.asm')\n"
        "  assert(text == nil and err ~= nil)\n"
        "end)\n");
    expect(run_ok(index, editor), "reads a sibling file");
    expect(run_ok(index + 1, editor), "reads a file through a subfolder");
    expect(run_ok(index + 2, editor), "'..' resolves back to a sibling");
    expect(run_ok(index + 3, editor), "an absolute path is rejected");
    expect(run_ok(index + 4, editor), "a missing file is reported, not a crash");

    s_relativeFileDocPath = NULL;
    callbacks.file_path = fake_relative_file_doc_path;
    plugin_host_set_callbacks(&callbacks);
    int unsavedIndex = load_ok("read-relative-unsaved.lua",
        "menu.add('Unsaved', function()\n"
        "  local text, err = editor.read_relative_file('lib.asm')\n"
        "  assert(text == nil and err:find('saved', 1, true))\n"
        "end)\n");
    expect(run_ok(unsavedIndex, editor), "an unsaved document has no folder to resolve against");

    plugin_host_set_callbacks(NULL);
    free(mainPathUtf8);
    DeleteFileW(siblingPath);
    DeleteFileW(nestedPath);
    RemoveDirectoryW(subdir);
    RemoveDirectoryW(base);
}

static void write_script(const WCHAR *directory, const WCHAR *name, const char *code) {
    WCHAR *path = path_join(directory, name);
    expect(path && write_bytes_file(path, code, strlen(code)), "write test script");
    free(path);
}

static void write_script_bytes(const WCHAR *directory, const WCHAR *name,
                               const char *code, size_t length) {
    WCHAR *path = path_join(directory, name);
    expect(path && write_bytes_file(path, code, length), "write binary test script");
    free(path);
}

static int test_plugin_enabled(const char *scope, const char *name) {
    return strcmp(scope, "test") != 0 || _stricmp(name, "b.lua") != 0;
}

static void test_directory(HWND editor) {
    WCHAR temp[MAX_PATH], directory[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    swprintf(directory, MAX_PATH, L"%lstinkeredit_plugins_%lu", temp, GetCurrentProcessId());
    CreateDirectoryW(directory, NULL);
    write_script(directory, L"b.lua", "menu.add('From B', function() end)");
    write_script(directory, L"a.lua", "menu.add('From A', function() end)");
    write_script(directory, L"broken.lua", "this is not lua");
    write_script(directory, L"ignored.luac", "menu.add('Ignored', function() end)");
    static const char nulCode[] =
        "menu.add('Before NUL', function() end)\0menu.add('After NUL', function() end)";
    write_script_bytes(directory, L"nul.lua", nulCode, sizeof(nulCode) - 1);
    WCHAR *folder = path_join(directory, L"folder");
    CreateDirectoryW(folder, NULL);
    WCHAR *nested = path_join(folder, L"nested");
    CreateDirectoryW(nested, NULL);
    write_script(folder, L"helper.lua", "return { label = 'From Folder' }");
    write_script(folder, L"late.lua", "return { value = 99 }");
    write_script(nested, L"value.lua", "return 42");
    write_script(folder, L"init.lua",
        "local helper = require('helper')\n"
        "assert(require('helper') == helper)\n"
        "assert(require('nested.value') == 42)\n"
        "menu.add(helper.label, function() end, 'Ctrl+Alt+M')\n"
        "menu.add('Lazy Module', function() assert(require('late').value == 99) end)");

    plugin_host_shutdown();
    char *report = plugin_host_load_directory(directory, "test");
    expect(report && strstr(report, "broken.lua") && strstr(report, "nul.lua"),
           "load errors are reported by file, including source after a NUL byte");
    expect(plugin_host_command_count() == 4, "only the good scripts and folder extensions load");
    expect(plugin_host_command_count() == 4 &&
           strcmp(plugin_host_command_label(0), "From A") == 0 &&
           strcmp(plugin_host_command_label(1), "From B") == 0 &&
           strcmp(plugin_host_command_label(2), "From Folder") == 0,
           "scripts and folder extensions load in name order");
    expect(run_ok(3, editor), "folder modules can be required lazily from a command");
    expect(strcmp(plugin_host_command_shortcut(2), "Ctrl+Alt+M") == 0,
           "plugin commands retain shortcuts");
    expect(plugin_host_extension_count() == 5 && plugin_host_error_path() != NULL,
           "discovered extensions and the first failing path are available to the UI");
    free(report);

    static const PluginHostCallbacks filtered = {
        .plugin_enabled = test_plugin_enabled
    };
    plugin_host_shutdown();
    plugin_host_set_callbacks(&filtered);
    report = plugin_host_load_directory(directory, "test");
    expect(plugin_host_command_count() == 3 &&
           strcmp(plugin_host_command_label(0), "From A") == 0 &&
           strcmp(plugin_host_command_label(1), "From Folder") == 0 &&
           plugin_host_extension_count() == 5,
           "disabled extensions are discovered but not executed");
    free(report);
    plugin_host_set_callbacks(NULL);

    const WCHAR *names[] = { L"a.lua", L"b.lua", L"broken.lua", L"ignored.luac", L"nul.lua" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        WCHAR *path = path_join(directory, names[i]);
        if (path) DeleteFileW(path);
        free(path);
    }
    WCHAR *helper = path_join(folder, L"helper.lua");
    WCHAR *late = path_join(folder, L"late.lua");
    WCHAR *init = path_join(folder, L"init.lua");
    WCHAR *nestedValue = path_join(nested, L"value.lua");
    DeleteFileW(helper); DeleteFileW(late); DeleteFileW(init); DeleteFileW(nestedValue);
    RemoveDirectoryW(nested); RemoveDirectoryW(folder);
    free(nestedValue); free(nested); free(helper); free(late); free(init); free(folder);
    RemoveDirectoryW(directory);

    plugin_host_shutdown();
    expect(plugin_host_load_directory(directory, "test") == NULL && plugin_host_command_count() == 0,
           "a missing folder loads nothing, silently");
}

static int find_command(const char *label) {
    for (int index = 0; index < plugin_host_command_count(); index++) {
        const char *candidate = plugin_host_command_label(index);
        if (candidate && strcmp(candidate, label) == 0) return index;
    }
    return -1;
}

static char s_outputText[2048];
static int s_outputShown;
static int s_terminalInputCancelled;
static int s_terminalInputInterrupted;
static int s_terminalInputsRemaining = -1;
static char s_problemSource[64], s_problemText[4096];

static void fake_output_write(const char *text) {
    size_t used = strlen(s_outputText);
    snprintf(s_outputText + used, sizeof(s_outputText) - used, "%s", text);
}
static int fake_terminal_begin(void) {
    s_outputText[0] = 0;
    s_outputShown = 1;
    return 1;
}
static void fake_terminal_write(const char *text) { fake_output_write(text); }
static char *fake_terminal_read(const char *prompt, int *interrupted) {
    if (interrupted) *interrupted = s_terminalInputInterrupted;
    if (prompt) fake_output_write(prompt);
    if (s_terminalInputInterrupted) return NULL;
    if (s_terminalInputCancelled || s_terminalInputsRemaining == 0) return NULL;
    if (s_terminalInputsRemaining > 0) s_terminalInputsRemaining--;
    fake_output_write("42\r\n");
    return _strdup("42");
}
static void fake_terminal_clear(void) { s_outputText[0] = 0; }
static void fake_terminal_end(void) {}
static void fake_output_clear(void) { s_outputText[0] = 0; }
static void fake_output_show(void) { s_outputShown = 1; }
static void fake_output_hide(void) { s_outputShown = 0; }
static int fake_output_visible(void) { return s_outputShown; }

static void fake_problems_report(const char *source, const char *text) {
    snprintf(s_problemSource, sizeof(s_problemSource), "%s", source ? source : "");
    snprintf(s_problemText, sizeof(s_problemText), "%s", text ? text : "");
}
static void fake_problems_clear_all(void) {
    s_problemSource[0] = 0;
    s_problemText[0] = 0;
}

static int s_autoReload;

static int fake_document_auto_reload(int index, int value) {
    (void)index;
    if (value >= 0) s_autoReload = value;
    return s_autoReload;
}

/* text repeated until the document holds at least bytes, between prefix
   and suffix, with separator between the copies. Caller frees. */
static char *repeated_text(const char *prefix, const char *text, const char *separator,
                           const char *suffix, size_t bytes) {
    size_t step = strlen(text) + strlen(separator);
    size_t count = bytes / step + 1;
    char *result = (char *)malloc(strlen(prefix) + count * step + strlen(suffix) + 1);
    if (!result) return NULL;
    char *end = result + sprintf(result, "%s", prefix);
    for (size_t i = 0; i < count; i++)
        end += sprintf(end, "%s%s", i ? separator : "", text);
    strcpy(end, suffix);
    return result;
}

/* The JSON and CSV commands must work on documents of several megabytes
   within the plugins' shared memory limit: formatting streams, and the
   parsed forms they keep are compact. */
static void test_large_documents(HWND editor, int jsonPretty, int jsonMinify, int jsonSort,
                                 int csvToTab, int csvFormat, int clefFormat) {
    const size_t size = 4u << 20;
    char *json = repeated_text("[", "{\"id\":12345,\"name\":\"item \\u00e9\",\"tags\":[\"a\",\"b\"],\"ok\":true}",
                               ",", "]", size);
    expect(json != NULL, "allocate a large JSON document");
    if (json) {
        set_document(editor, json);
        SendMessageA(editor, SCI_SETSEL, 0, 0);
        expect(run_ok(jsonPretty, editor) &&
               SendMessageA(editor, SCI_GETLENGTH, 0, 0) > (LRESULT)strlen(json),
               "Format JSON handles a 4 MB document");
        SendMessageA(editor, SCI_SETSEL, 0, 0);
        expect(run_ok(jsonMinify, editor) && document_is(editor, json),
               "Minify JSON restores a 4 MB formatted document");
        SendMessageA(editor, SCI_SETSEL, 0, 0);
        expect(run_ok(jsonSort, editor), "Sort Object Keys handles a 4 MB document");
        free(json);
    }
    char *csv = repeated_text("id,name,note\n", "12345,item,\"quoted, \"\"note\"\"\"", "\n", "\n", size);
    expect(csv != NULL, "allocate a large CSV document");
    if (csv) {
        set_document(editor, csv);
        SendMessageA(editor, SCI_SETSEL, 0, 0);
        expect(run_ok(csvToTab, editor), "CSV conversion handles a 4 MB document");
        set_document(editor, csv);
        SendMessageA(editor, SCI_SETSEL, 0, 0);
        expect(run_ok(csvFormat, editor), "Format Columns handles a 4 MB document");
        free(csv);
    }
    char *clef = repeated_text("",
        "{\"@t\":\"2026-01-01T00:00:00Z\",\"@mt\":\"Hello {Name}\",\"Name\":\"Ada\"}",
        "\n", "\n", size);
    expect(clef != NULL, "allocate a large CLEF document");
    if (clef) {
        set_document(editor, clef);
        SendMessageA(editor, SCI_SETSEL, 0, 0);
        expect(run_ok(clefFormat, editor) &&
               document_contains(editor, "2026-01-01T00:00:00Z [INF] Hello Ada"),
               "CLEF formatting handles a 4 MB document");
        free(clef);
    }
    set_document(editor, "");
}

static void test_bundled_plugins(HWND editor) {
    static const PluginHostCallbacks fakes = {
        .prompt = fake_prompt, .choose = fake_choose, .file_path = fake_file_path,
        .language = fake_language, .message = fake_message, .config_get = fake_config_get,
        .config_set = fake_config_set, .save_binary = fake_save_binary,
        .status = fake_status,
        .document_encoding = fake_document_encoding,
        .document_set_encoding = fake_document_set_encoding,
        .schedule_deferred = fake_schedule_deferred,
        .output_write = fake_output_write, .output_show = fake_output_show,
        .terminal_begin = fake_terminal_begin, .terminal_write = fake_terminal_write,
        .terminal_read = fake_terminal_read, .terminal_clear = fake_terminal_clear,
        .terminal_end = fake_terminal_end, .problems_report = fake_problems_report,
        .problems_clear_all = fake_problems_clear_all,
        .document_auto_reload = fake_document_auto_reload
    };
    plugin_host_set_callbacks(&fakes);
    plugin_host_shutdown();
    char *report = plugin_host_load_directory(L"plugins", "bundled");
    if (report) fprintf(stderr, "%s\n", report);
    expect(report == NULL, "bundled plugins load without errors");
    free(report);
    /* The limit is shared with the built-in formatting commands and the menu
       definition, which aren't loaded here, and with personal plugins. */
    expect(plugin_host_command_count() <= PLUGIN_MAX_COMMANDS / 2,
           "bundled plugins leave room under the command limit");

    int encode64 = find_command("Developer Tools/Base64 Encode");
    int decode64 = find_command("Developer Tools/Base64 Decode");
    int encodeUrl = find_command("Developer Tools/URL Encode");
    int decodeUrl = find_command("Developer Tools/URL Decode");
    int encodeHex = find_command("Developer Tools/Hex Encode");
    int decodeHex = find_command("Developer Tools/Hex Decode");
    int encodeHtml = find_command("Developer Tools/HTML Entity Encode");
    int decodeHtml = find_command("Developer Tools/HTML Entity Decode");
    int csvInspect = find_command("/Tools/CSV/Inspect and Validate");
    int csvFormat = find_command("/Tools/CSV/Format Columns");
    int csvToComma = find_command("/Tools/CSV/Convert/To Comma-Separated");
    int csvToTab = find_command("/Tools/CSV/Convert/To Tab-Separated");
    int csvToJson = find_command("/Tools/CSV/Convert/To JSON Objects");
    int csvDedupe = find_command("/Tools/CSV/Rows/Remove Duplicate Rows");
    int csvTranspose = find_command("/Tools/CSV/Columns/Transpose Rows and Columns");
    int jsonPretty = find_command("/Tools/JSON/Format JSON");
    int jsonMinify = find_command("/Tools/JSON/Minify JSON");
    int jsonSort = find_command("/Tools/JSON/Sort Object Keys");
    int jsonToYaml = find_command("/Tools/JSON/Convert JSON to YAML");
    int yamlToJson = find_command("/Tools/JSON/Convert YAML to JSON");
    int jsonEscape = find_command("/Tools/JSON/Escape Selection as JSON String");
    int jsonUnescape = find_command("/Tools/JSON/Unescape JSON String");
    int editUpper = find_command("/Edit/Convert/&UPPERCASE");
    int editLower = find_command("/Edit/Convert/&lowercase");
    int editTitle = find_command("/Edit/Convert/&Title Case");
    int editSort = find_command("/Edit/Convert/Sort Lines &Ascending");
    int editSortDescending = find_command("/Edit/Convert/Sort Lines Descend&ing");
    int editDedupe = find_command("/Edit/Convert/Remove &Duplicate Lines");
    int editTabs = find_command("/Edit/Convert/Tabs to &Spaces");
    int editSpaces = find_command("/Edit/Convert/Spaces to Ta&bs");
    int editCrlf = find_command("/Edit/Convert/Line &Endings/&CRLF (Windows)");
    int editLf = find_command("/Edit/Convert/Line &Endings/&LF (Unix/macOS)");
    int editCr = find_command("/Edit/Convert/Line &Endings/C&R (Classic Mac)");
    int editUtf8 = find_command("/Edit/Convert/En&coding/UTF-&8");
    int editUtf8Bom = find_command("/Edit/Convert/En&coding/UTF-8 with &BOM");
    int editUtf16Le = find_command("/Edit/Convert/En&coding/UTF-16 &LE");
    int editUtf16Be = find_command("/Edit/Convert/En&coding/UTF-16 B&E");
    int editAnsi = find_command("/Edit/Convert/En&coding/&ANSI (system code page)");
    int naturalSort = find_command("/Edit/Convert/Natural Sort Lines Ascending");
    int anagramWords = find_command("Anagrams (British English)/Words From Letters...");
    int numberHex = find_command("/Tools/Number Converter/To Hexadecimal");
    int numberDecimal = find_command("/Tools/Number Converter/To Decimal");
    int numberRoman = find_command("/Tools/Number Converter/To Roman Numerals");
    int sqlFormat = find_command("/Tools/SQL/Format SQL");
    int sqlMinify = find_command("/Tools/SQL/Minify SQL");
    int sqlUpper = find_command("/Tools/SQL/Uppercase Keywords");
    int sqlLower = find_command("/Tools/SQL/Lowercase Keywords");
    int removeContaining = find_command("Lines/Remove Lines Containing...");
    int jsonLinesToArray = find_command("/Tools/Data Converter/JSON Lines to JSON Array");
    int jsonArrayToLines = find_command("/Tools/Data Converter/JSON Array to JSON Lines");
    int queryToJson = find_command("/Tools/Data Converter/Query String to JSON");
    int clefFormat = find_command("/Tools/Logs/CLEF/Format Events");
    int clefWarnings = find_command("/Tools/Logs/CLEF/Keep Warnings and Above");
    int clefFormatProperties = find_command("/Tools/Logs/CLEF/Format Events with Properties");
    int clefInspect = find_command("/Tools/Logs/CLEF/Inspect");
    int clefLevel = find_command("/Tools/Logs/CLEF/Keep Level and Above...");
    int clefSearch = find_command("/Tools/Logs/CLEF/Keep Events Containing...");
    int clefSort = find_command("/Tools/Logs/CLEF/Sort by Timestamp");
    int clefExceptions = find_command("/Tools/Logs/CLEF/Extract Exceptions");
    int grammarCheck = find_command("/Tools/Grammar Checker/Check");
    int grammarHighlight = find_command("/Tools/Grammar Checker/Highlight Issues");
    int grammarFix = find_command("/Tools/Grammar Checker/Fix Safe Issues");
    int grammarLive = find_command("/Tools/Grammar Checker/Live Highlighting");
    int encodingDecode64 = find_command("Encode/Base64 Decode");
    int rot13 = find_command("Secret Message Lab/ROT13");
    int atbash = find_command("Secret Message Lab/Atbash");
    int encodeMorse = find_command("Secret Message Lab/Morse Code/Encode");
    int decodeMorse = find_command("Secret Message Lab/Morse Code/Decode");
    int encodeNato = find_command("Secret Message Lab/NATO Phonetics/Encode");
    int decodeNato = find_command("Secret Message Lab/NATO Phonetics/Decode");
    int encodeBinary = find_command("Secret Message Lab/Binary/Encode UTF-8");
    int decodeBinary = find_command("Secret Message Lab/Binary/Decode UTF-8");
    int pigLatin = find_command("Secret Message Lab/Pig Latin");
    int personality = find_command("Document Personality/Analyse");
    int clearPersonality = find_command("Document Personality/Clear Highlights");
    int checkConstraints = find_command("Constraint Writer/Check Constraints");
    int highlightConstraints = find_command("Constraint Writer/Highlight Issues");
    int clearConstraints = find_command("Constraint Writer/Clear Highlights");
    int toc = find_command("/Snippets/Markdown/Table of Contents...");
    int promoteHeading = find_command("/Tools/Markdown/Structure/Promote Heading");
    int renumberLists = find_command("/Tools/Markdown/Structure/Renumber Ordered Lists");
    int toggleTasks = find_command("/Tools/Markdown/Structure/Toggle Task State");
    int inlineToReferences = find_command("/Tools/Markdown/Links/Convert Inline Links to References");
    int referencesToInline = find_command("/Tools/Markdown/Links/Convert References to Inline Links");
    int lintMarkdown = find_command("/Tools/Markdown/Lint/Check Document");
    int statistics = find_command("/Tools/Statistics/Document or Selection Report");
    int calculator = find_command("Calculate Selection");
    if (calculator < 0) calculator = find_command("/Tools/Calculate Selection");
    int removeBlank = find_command("Line Tools/Clean/Remove Blank Lines");
    int sortLines = find_command("Line Tools/Order/Sort A to Z");
    int sortLength = find_command("Line Tools/Order/Sort by Length");
    int shuffle = find_command("Line Tools/Order/Shuffle");
    int buildTap = find_command("/Tools/Assemblers/Z80 Assembler/Compile");
    int buildAndRunTap = find_command("/Tools/Assemblers/Z80 Assembler/Compile and Run");
    int configureZ80Emulator = find_command("/Tools/Assemblers/Z80 Assembler/Configure Emulator...");
    expect(encode64 >= 0 && decode64 >= 0 && encodeUrl >= 0 && decodeUrl >= 0 &&
           encodeHex >= 0 && decodeHex >= 0 && encodeHtml >= 0 && decodeHtml >= 0 &&
           csvInspect >= 0 && csvFormat >= 0 && csvToComma >= 0 && csvToTab >= 0 && csvToJson >= 0 &&
           csvDedupe >= 0 && csvTranspose >= 0 &&
           jsonPretty >= 0 && jsonMinify >= 0 && jsonSort >= 0 &&
           jsonToYaml >= 0 && yamlToJson >= 0 && jsonEscape >= 0 && jsonUnescape >= 0 &&
           editUpper >= 0 && editLower >= 0 && editTitle >= 0 && editSort >= 0 &&
           editSortDescending >= 0 && editDedupe >= 0 && editTabs >= 0 && editSpaces >= 0 &&
           editCrlf >= 0 && editLf >= 0 && editCr >= 0 && editUtf8 >= 0 &&
           editUtf8Bom >= 0 && editUtf16Le >= 0 && editUtf16Be >= 0 && editAnsi >= 0 &&
           naturalSort >= 0 && anagramWords >= 0 &&
           numberHex >= 0 && numberDecimal >= 0 && numberRoman >= 0 &&
           sqlFormat >= 0 && sqlMinify >= 0 && sqlUpper >= 0 && sqlLower >= 0 &&
           removeContaining >= 0 && jsonLinesToArray >= 0 &&
           jsonArrayToLines >= 0 && queryToJson >= 0 && clefFormat >= 0 &&
           clefWarnings >= 0 && clefFormatProperties >= 0 && clefInspect >= 0 &&
           clefLevel >= 0 && clefSearch >= 0 && clefSort >= 0 && clefExceptions >= 0 &&
           grammarCheck >= 0 && grammarHighlight >= 0 && grammarFix >= 0 && grammarLive >= 0 &&
           encodingDecode64 >= 0 &&
           rot13 >= 0 && atbash >= 0 && encodeMorse >= 0 && decodeMorse >= 0 &&
           encodeNato >= 0 && decodeNato >= 0 && encodeBinary >= 0 &&
           decodeBinary >= 0 && pigLatin >= 0 &&
           personality >= 0 && clearPersonality >= 0 &&
           checkConstraints >= 0 && highlightConstraints >= 0 && clearConstraints >= 0 &&
           toc >= 0 && promoteHeading >= 0 && renumberLists >= 0 && toggleTasks >= 0 &&
           inlineToReferences >= 0 && referencesToInline >= 0 && lintMarkdown >= 0 &&
           statistics >= 0 && calculator >= 0 && removeBlank >= 0 &&
           sortLines >= 0 && sortLength >= 0 && shuffle >= 0 && buildTap >= 0 &&
           buildAndRunTap >= 0 && configureZ80Emulator >= 0,
           "bundled plugin commands are registered");

    set_document(editor, "one two three");
    SendMessageA(editor, SCI_SETSEL, 4, 7);
    expect(run_ok(editUpper, editor) && document_is(editor, "one TWO three") &&
           SendMessageA(editor, SCI_GETSELECTIONSTART, 0, 0) == 4 &&
           SendMessageA(editor, SCI_GETSELECTIONEND, 0, 0) == 7,
           "Edit Convert case command preserves its selection");
    expect(run_ok(editLower, editor) && document_is(editor, "one two three"),
           "Edit Convert lowercases a selection");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(editTitle, editor) && document_is(editor, "One Two Three"),
           "Edit Convert title-cases a selection");
    set_document(editor, "it's DON'T o'clock won\xE2\x80\x99t");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(editTitle, editor) && document_is(editor, "It's Don't O'clock Won\xE2\x80\x99t"),
           "Edit Convert title case keeps apostrophes inside words");

    SendMessageA(editor, SCI_SETEOLMODE, SC_EOL_CRLF, 0);
    set_document(editor, "b\r\na\r\nb\r\n");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(editSort, editor) && document_is(editor, "a\r\nb\r\nb\r\n"),
           "Edit Convert sorts selected lines");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(editSortDescending, editor) && document_is(editor, "b\r\nb\r\na\r\n"),
           "Edit Convert sorts selected lines descending");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(editDedupe, editor) && document_is(editor, "b\r\na\r\n"),
           "Edit Convert removes duplicate selected lines");
    set_document(editor, "b\r\nB\r\na\r\n");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(editSort, editor) && document_is(editor, "a\r\nb\r\nB\r\n"),
           "Edit Convert sorts ignoring case, keeping equal lines in order");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(editSort, editor) && document_is(editor, "a\r\nb\r\nB\r\n") &&
           strstr(s_lastMessage, "Select one or more lines"),
           "Edit Convert sort says so when nothing is selected");

    SendMessageA(editor, SCI_SETTABWIDTH, 4, 0);
    set_document(editor, "\tx\n  \ty\ninside\ttext");
    expect(run_ok(editTabs, editor) && document_is(editor, "    x\n    y\ninside\ttext"),
           "Edit Convert changes only leading indentation");
    expect(run_ok(editSpaces, editor) && document_is(editor, "\tx\n\ty\ninside\ttext"),
           "Edit Convert converts leading spaces back to tabs");
    expect(run_ok(editLf, editor) && document_is(editor, "\tx\n\ty\ninside\ttext") &&
           SendMessageA(editor, SCI_GETEOLMODE, 0, 0) == SC_EOL_LF,
           "Edit Convert changes existing and future line endings");
    expect(run_ok(editCr, editor) && document_is(editor, "\tx\r\ty\rinside\ttext") &&
           SendMessageA(editor, SCI_GETEOLMODE, 0, 0) == SC_EOL_CR,
           "Edit Convert changes line endings to CR");
    expect(run_ok(editCrlf, editor) && document_is(editor, "\tx\r\n\ty\r\ninside\ttext") &&
           SendMessageA(editor, SCI_GETEOLMODE, 0, 0) == SC_EOL_CRLF,
           "Edit Convert changes line endings to CRLF");
    expect(run_ok(editLf, editor) && SendMessageA(editor, SCI_GETEOLMODE, 0, 0) == SC_EOL_LF,
           "Edit Convert can return the document to LF endings");
    strcpy(s_documentEncoding, "ansi");
    expect(run_ok(editUtf8, editor) && strcmp(s_documentEncoding, "utf-8") == 0 &&
           run_ok(editUtf8Bom, editor) && strcmp(s_documentEncoding, "utf-8-bom") == 0 &&
           run_ok(editUtf16Le, editor) && strcmp(s_documentEncoding, "utf-16le") == 0 &&
           run_ok(editUtf16Be, editor) && strcmp(s_documentEncoding, "utf-16be") == 0 &&
           run_ok(editAnsi, editor) && strcmp(s_documentEncoding, "ansi") == 0,
           "Edit Convert exposes every save encoding through the plugin API");

    set_document(editor, "file10\nfile2\nfile01\nfile1");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(naturalSort, editor) && document_is(editor, "file1\nfile01\nfile2\nfile10"),
           "natural sort compares embedded digit runs numerically");

    set_document(editor, "tone");
    SendMessageA(editor, SCI_SETSEL, 0, 4);
    expect(run_ok(anagramWords, editor) && document_is(editor, "tone"),
           "anagram word indexing handles the bundled dictionary");

    set_document(editor, "255");
    SendMessageA(editor, SCI_SETSEL, 0, 3);
    expect(run_ok(numberHex, editor) && document_is(editor, "0xFF"),
           "number converter converts decimal to hexadecimal");
    SendMessageA(editor, SCI_SETSEL, 0, 4);
    expect(run_ok(numberDecimal, editor) && document_is(editor, "255"),
           "number converter reads prefixed hexadecimal");
    SendMessageA(editor, SCI_SETSEL, 0, 3);
    expect(run_ok(numberRoman, editor) && document_is(editor, "CCLV"),
           "number converter creates canonical Roman numerals");

    set_document(editor, "select a,b from users where id=1 and active=true;");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(sqlFormat, editor) &&
           document_is(editor, "SELECT a, b\nFROM users\nWHERE id = 1 AND active = TRUE;"),
           "SQL plugin formats clauses without changing identifiers");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(sqlMinify, editor) &&
           document_is(editor, "SELECT a, b FROM users WHERE id = 1 AND active = TRUE;"),
           "SQL plugin minifies formatted SQL");
    set_document(editor, "select count(*) from users;");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(sqlFormat, editor) && document_is(editor, "SELECT count(*)\nFROM users;"),
           "SQL plugin keeps function parentheses on the same line");
    set_document(editor, "select a from t where id=$1 and n=@p and s=N'x' and k=:key and v=a::int");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(sqlMinify, editor) &&
           document_is(editor, "SELECT a FROM t WHERE id = $1 AND n = @p AND s = N'x' AND k = :key AND v = a::int"),
           "SQL plugin keeps parameters, variables and prefixed strings whole");
    set_document(editor, "select caf\xc3\xa9 from t where doc @@ q");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(sqlMinify, editor) &&
           document_is(editor, "SELECT caf\xc3\xa9 FROM t WHERE doc @@ q"),
           "SQL plugin keeps non-ASCII names whole and reads a bare @@ as an operator");
    set_document(editor, "select a,\n  b\nfrom t -- note\nwhere x=1");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(sqlUpper, editor) && document_is(editor, "SELECT a,\n  b\nFROM t -- note\nWHERE x=1"),
           "SQL plugin changes keyword case without changing the layout");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(sqlLower, editor) && document_is(editor, "select a,\n  b\nfrom t -- note\nwhere x=1"),
           "SQL plugin lowercases keywords in place");

    char *longSql = repeated_text("select ", "column_name", ",", " from records", 256u << 10);
    expect(longSql != NULL, "allocate a long SQL statement");
    if (longSql) {
        set_document(editor, longSql);
        SendMessageA(editor, SCI_SETSEL, 0, 0);
        expect(run_ok(sqlFormat, editor) && document_contains(editor, "\nFROM records"),
               "SQL formatting handles a long expression in linear space");
        free(longSql);
    }

    set_document(editor, "{\"a\":1}\n{\"a\":2}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(jsonLinesToArray, editor) &&
           document_is(editor, "[\n  {\"a\":1},\n  {\"a\":2}\n]"),
           "data converter turns JSON Lines into an array");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(jsonArrayToLines, editor) && document_is(editor, "{\"a\":1}\n{\"a\":2}"),
           "data converter turns a JSON array back into JSON Lines");
    set_document(editor, "name=Ada+Lovelace&lang=Lua");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(queryToJson, editor) &&
           document_is(editor, "{\n  \"name\": \"Ada Lovelace\",\n  \"lang\": \"Lua\"\n}"),
           "data converter decodes URL query strings");

    set_document(editor,
        "{\"@t\":\"2026-01-01T00:00:00Z\",\"@mt\":\"Hello {Name}\","
        "\"@l\":\"Warning\",\"Name\":\"Ada\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefFormat, editor) &&
           document_is(editor, "2026-01-01T00:00:00Z [WRN] Hello Ada"),
           "log plugin renders CLEF message templates");
    set_document(editor,
        "{\"@t\":\"1\",\"@m\":\"ok\",\"@l\":\"Information\"}\n"
        "{\"@t\":\"2\",\"@m\":\"bad\",\"@l\":\"Error\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefWarnings, editor) &&
           document_is(editor, "{\"@t\":\"2\",\"@m\":\"bad\",\"@l\":\"Error\"}"),
           "log plugin filters CLEF by severity");
    set_document(editor,
        "{\"@t\":\"1\",\"@mt\":\"{{x}} {$Name,-5}| {Missing} [{Empty}] {Quote} {Face}\","
        "\"Name\":\"Ada\",\"Empty\":\"\",\"Quote\":\"\\\"q\\\"\",\"Face\":\"\\ud83d\\ude00\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefFormat, editor) &&
           document_is(editor, "1 [INF] {x} Ada  | {Missing} [] \"q\" \xF0\x9F\x98\x80"),
           "log plugin renders CLEF escapes, alignment, empty values and surrogate pairs");
    set_document(editor,
        "{\"@t\":\"1\",\"@mt\":\"Saw {raw}\",\"@@t\":\"user\",\"raw\":\"x\",\"@l\":\"Error\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefFormatProperties, editor) &&
           document_is(editor, "1 [ERR] Saw x\n    @t = \"user\"\n    raw = \"x\""),
           "log plugin keeps @@ properties apart from reserved CLEF fields");
    set_document(editor, "{\"@m\":\"a\"}\n\nnot json");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(clefFormat, editor) && strstr(s_lastMessage, "line 3."),
           "log plugin reports the real line number of an invalid CLEF event");
    set_document(editor,
        "{\"@t\":\"2026-01-01T01:00:00+02:00\",\"@m\":\"b\"}\n"
        "{\"@m\":\"none\"}\n"
        "{\"@t\":\"2026-01-01T00:00:00.5Z\",\"@m\":\"d\"}\n"
        "{\"@t\":\"2026-01-01T00:00:00Z\",\"@m\":\"c\"}\n"
        "{\"@t\":\"2025-12-31T23:00:00.25Z\",\"@m\":\"a\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefSort, editor) &&
           document_is(editor,
               "{\"@t\":\"2026-01-01T01:00:00+02:00\",\"@m\":\"b\"}\n"
               "{\"@t\":\"2025-12-31T23:00:00.25Z\",\"@m\":\"a\"}\n"
               "{\"@t\":\"2026-01-01T00:00:00Z\",\"@m\":\"c\"}\n"
               "{\"@t\":\"2026-01-01T00:00:00.5Z\",\"@m\":\"d\"}\n"
               "{\"@m\":\"none\"}"),
           "log plugin sorts CLEF events by UTC timestamp");
    s_outputText[0] = 0;
    expect(run_ok(clefInspect, editor) && strstr(s_outputText, "CLEF Inspection - note.md") &&
           strstr(s_outputText, "CLEF events: 5") &&
           strstr(s_outputText, "First: 2026-01-01T01:00:00+02:00") &&
           strstr(s_outputText, "Last: 2026-01-01T00:00:00.5Z"),
           "log plugin inspect reports the CLEF time range");
    set_document(editor,
        "{\"@m\":\"i\",\"@l\":\"Information\"}\n"
        "{\"@m\":\"e\",\"@l\":\"Error\"}\n"
        "{\"@m\":\"f\",\"@l\":\"Fatal\",\"User\":\"ada\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefLevel, editor) &&
           document_is(editor, "{\"@m\":\"e\",\"@l\":\"Error\"}\n"
                               "{\"@m\":\"f\",\"@l\":\"Fatal\",\"User\":\"ada\"}"),
           "log plugin filters CLEF by a chosen level");
    expect(run_ok(clefSearch, editor) &&
           document_is(editor, "{\"@m\":\"f\",\"@l\":\"Fatal\",\"User\":\"ada\"}"),
           "log plugin keeps CLEF events containing text");
    set_document(editor,
        "{\"@t\":\"1\",\"@m\":\"ok\"}\n"
        "{\"@t\":\"2\",\"@m\":\"boom\",\"@x\":\"System.Exception: boom\\n   at Main\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefExceptions, editor) &&
           document_is(editor, "2 boom\nSystem.Exception: boom\n   at Main"),
           "log plugin extracts CLEF exceptions with their message");
    set_document(editor,
        "{\"@t\":\"1\",\"@mt\":\"Placed {@Order} for {Tags} in {Elapsed:0.0} ms\","
        "\"Order\":{\"$type\":\"Order\",\"Id\":5,\"Lines\":[{\"Sku\":\"}A\\\"1\"},null]},"
        "\"Tags\":[\"x\",true],\"Elapsed\":12.345,\"@r\":[\"12.3\"]}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefFormat, editor) &&
           document_is(editor, "1 [INF] Placed Order { Id: 5, Lines: [{ Sku: \"}A\\\"1\" }, null] }"
                               " for [\"x\", true] in 12.3 ms"),
           "log plugin renders nested CLEF objects, arrays and @r renderings");
    set_document(editor,
        "{\"@t\":\"1\",\"@m\":\"m\",\"Order\":{\"Id\":5,\"Tags\":[]},"
        "\"Body\":\"{\\\"a\\\":[1,2]}\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefFormatProperties, editor) &&
           document_is(editor, "1 [INF] m\n"
                               "    Order = {\n      \"Id\": 5,\n      \"Tags\": []\n    }\n"
                               "    Body = {\n      \"a\": [\n        1,\n        2\n      ]\n    }"),
           "log plugin lists nested and embedded JSON properties as indented JSON");
    set_document(editor, "{\"@m\":\"x\",\"A\":{\"b\":}}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(clefFormat, editor) && strstr(s_lastMessage, "line 1."),
           "log plugin rejects invalid nested JSON in a CLEF event");

    set_document(editor, "this is is a error.");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_outputText[0] = 0;
    expect(run_ok(grammarCheck, editor) && strstr(s_outputText, "possible issues") &&
           strstr(s_outputText, "note.md:1:1: ") && strstr(s_outputText, "this is is a error.") &&
           run_ok(grammarHighlight, editor) && SendMessageA(editor, SCI_INDICATORVALUEAT, 12, 0),
           "grammar checker reports source lines and highlights heuristic issues");
    set_document(editor, "This is is badly ,spaced!");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(grammarFix, editor) && document_is(editor, "This is badly, spaced!"),
           "grammar checker fixes safe spacing and repeated-word issues");

    /* Live highlighting checks the whole document, not the selection,
       because each update clears highlights everywhere. */
    set_document(editor, "this is is a error.\nFine words here.");
    SendMessageA(editor, SCI_SETSEL, 20, 36);
    expect(run_ok(grammarLive, editor) && SendMessageA(editor, SCI_INDICATORVALUEAT, 12, 0) &&
           plugin_host_fire("text_changed", NULL, editor) == NULL &&
           SendMessageA(editor, SCI_INDICATORVALUEAT, 12, 0),
           "grammar checker live highlighting ignores the selection");
    size_t largeLength = 300 * 1024;
    char *large = (char *)malloc(largeLength + 1);
    if (large) {
        memset(large, 'x', largeLength);
        memcpy(large, "this is is a error. ", 20);
        large[largeLength] = 0;
        set_document(editor, large);
        free(large);
        expect(plugin_host_fire("text_changed", NULL, editor) == NULL &&
               !SendMessageA(editor, SCI_INDICATORVALUEAT, 12, 0),
               "grammar checker live highlighting skips very large documents");
    }
    expect(run_ok(grammarLive, editor), "grammar checker live highlighting turns off");

    set_document(editor, "It took an hour, e.g. at a university. An FBI agent made a one-off visit. "
                         "Plan A is fine.\n2. second item.\n- item (i) here.");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(grammarCheck, editor) && strstr(s_lastMessage, "No heuristic grammar issues found."),
           "grammar checker accepts articles said differently, abbreviations and list items");
    set_document(editor, "She had an X-ray after a U-turn.");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(grammarCheck, editor) && strstr(s_lastMessage, "No heuristic grammar issues found."),
           "grammar checker reads a single capital as its letter name");
    set_document(editor, "i dont know,, alot was on monday ( today ).. Im sure. IM me.");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(grammarFix, editor) && document_is(editor, "I don't know, a lot was on Monday (today). I'm sure. IM me."),
           "grammar checker fixes contractions, I, days, doubled punctuation and spaced parentheses");
    set_document(editor, "He don't care more then they was. Its a test.");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_outputText[0] = 0;
    expect(run_ok(grammarCheck, editor) && strstr(s_outputText, "Found 4 possible issues") &&
           strstr(s_outputText, "Use 'doesn't' after he.") &&
           strstr(s_outputText, "After a comparison, use 'than'.") &&
           strstr(s_outputText, "Use 'were' after they.") && strstr(s_outputText, "it's"),
           "grammar checker suggests agreement, than and its/it's corrections");
    set_document(editor, "Two  spaces here.");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    s_outputText[0] = 0;
    int doubleSpaces = find_command("/Tools/Grammar Checker/Rules/Double Spaces (Style)");
    expect(run_ok(grammarCheck, editor) && strstr(s_lastMessage, "No heuristic") &&
           doubleSpaces >= 0 && run_ok(doubleSpaces, editor) && run_ok(grammarCheck, editor) &&
           strstr(s_outputText, "More than one space between words."),
           "grammar checker style rules start off and can be switched on");
    expect(run_ok(doubleSpaces, editor), "grammar checker style rules can be switched off");

    set_document(editor, "ORG $8000\nstart: LD A,42\nLD ($9000),A\nJR start\n");
    s_binaryLength = 0;
    expect(buildTap >= 0 && run_ok(buildTap, editor) && !s_binaryOpenAfterSave &&
           strcmp(s_binaryName, "C:\\docs\\note.tap") == 0 && s_binaryLength == 93 &&
           (unsigned char)s_binaryData[0] == 19 && (unsigned char)s_binaryData[3] == 0 &&
           (unsigned char)s_binaryData[14] == 36 && (unsigned char)s_binaryData[16] == 10 &&
           (unsigned char)s_binaryData[18] == 36 && (unsigned char)s_binaryData[23] == 0xff &&
           (unsigned char)s_binaryData[24] == 0 && (unsigned char)s_binaryData[25] == 10 &&
           (unsigned char)s_binaryData[28] == 0xfd && memcmp(s_binaryData + 29, "32767", 5) == 0 &&
           (unsigned char)s_binaryData[34] == 0x0e && (unsigned char)s_binaryData[37] == 0xff &&
           (unsigned char)s_binaryData[38] == 0x7f && (unsigned char)s_binaryData[41] == 0xef &&
           (unsigned char)s_binaryData[44] == 0xaf && (unsigned char)s_binaryData[46] == 0xf9 &&
           (unsigned char)s_binaryData[47] == 0xc0 && memcmp(s_binaryData + 48, "32768", 5) == 0 &&
           (unsigned char)s_binaryData[64] == 3 && (unsigned char)s_binaryData[75] == 7 &&
           (unsigned char)s_binaryData[77] == 0 && (unsigned char)s_binaryData[78] == 0x80 &&
           (unsigned char)s_binaryData[84] == 0xff && (unsigned char)s_binaryData[85] == 0x3e &&
           (unsigned char)s_binaryData[86] == 42 && (unsigned char)s_binaryData[87] == 0x32 &&
           (unsigned char)s_binaryData[88] == 0 && (unsigned char)s_binaryData[89] == 0x90 &&
           (unsigned char)s_binaryData[90] == 0x18 && (unsigned char)s_binaryData[91] == 0xf9,
           "bundled Z80 assembler creates an auto-running Spectrum TAP");

    s_binaryOpenAfterSave = 0;
    s_z80EmulatorPath = "C:\\Emulators\\zx.exe";
    s_z80EmulatorArguments = "--fast";
    strcpy(s_lastMessage, "unchanged");
    expect(buildAndRunTap >= 0 && run_ok(buildAndRunTap, editor) &&
           strcmp(s_binaryName, "C:\\docs\\note.tap") == 0 && s_binaryLength == 93 &&
           s_binaryOpenAfterSave && strcmp(s_binaryApplication, s_z80EmulatorPath) == 0 &&
           strcmp(s_binaryArguments, s_z80EmulatorArguments) == 0 &&
           strcmp(s_lastMessage, "unchanged") == 0,
           "Z80 Compile and Run saves and launches its configured emulator silently");
    s_z80EmulatorPath = s_z80EmulatorArguments = NULL;

    set_document(editor,
        "ORG $8000\nCOUNT EQU 5\nLD BC,$1234\nLD IX,$9000\nLD (IX-2),A\n"
        "LD B,(IY+3)\nINC (IX+1)\nADD A,(IY-1)\nADC HL,SP\nBIT 7,(IX-8)\n"
        "JP NZ,done\nJR C,done\nCALL Z,done\nPUSH IX\nEX AF,AF'\nEX (SP),IY\nIN A,($FE)\n"
        "IN B,(C)\nOUT (C),D\nIM 1\nLDIR\ndone: RET\nDB \"OK\",0\nDW done\nDS COUNT,$FF\n");
    s_binaryLength = 0;
    expect(run_ok(buildTap, editor) && s_binaryLength > 40,
           "bundled Z80 assembler handles directives and indexed/extended instructions");

    set_document(editor, "org 3000\nld a,255\nloop: dec a\njp nz loop:\nret\n");
    s_binaryLength = 0;
    expect(run_ok(buildTap, editor) && s_binaryLength == 79 &&
           (unsigned char)s_binaryData[39] == 0x0e && (unsigned char)s_binaryData[42] == 0xb8 &&
           (unsigned char)s_binaryData[43] == 0x0b &&
           (unsigned char)s_binaryData[63] == 0xb8 && (unsigned char)s_binaryData[64] == 0x0b &&
           (unsigned char)s_binaryData[71] == 0x3e && (unsigned char)s_binaryData[72] == 0xff &&
           (unsigned char)s_binaryData[73] == 0x3d && (unsigned char)s_binaryData[74] == 0xc2 &&
           (unsigned char)s_binaryData[75] == 0xba && (unsigned char)s_binaryData[76] == 0x0b &&
           (unsigned char)s_binaryData[77] == 0xc9,
           "Z80 assembler accepts whitespace-separated conditions and colon references");

    set_document(editor, "        org 8000\n        ld z,255\nloop:   dec a\n"
                         "        jp nz, loop:\n        ret\n");
    s_lastMessage[0] = 0;
    expect(run_ok(buildTap, editor) && strstr(s_lastMessage, "Line 2:") &&
           strstr(s_lastMessage, "unsupported LD operands") &&
           SendMessageA(editor, SCI_GETSELECTIONSTART, 0, 0) == 17 &&
           SendMessageA(editor, SCI_GETSELECTIONEND, 0, 0) == 33,
           "bundled Z80 assembler highlights and reports the source line for an error");

    set_document(editor, s_calculatorExpression);
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(calculator >= 0 && run_ok(calculator, editor) && document_contains(editor, " = 33"),
           "bundled calculator evaluates bases, shifts and functions");

    s_calculatorExpression = "((0xF0 >> 4) xor 0b0011) + (10 % 4)";
    set_document(editor, s_calculatorExpression);
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(calculator, editor) && document_contains(editor, " = 14"),
           "bundled calculator evaluates bitwise xor and modulo");

    s_calculatorExpression =
        "deg(pi) + sqrt(16)";
    set_document(editor, s_calculatorExpression);
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(calculator, editor) && document_contains(editor, " = 184"),
           "bundled calculator evaluates trigonometry and angle conversions");

    s_calculatorExpression = "max(1, 2) * 3";
    set_document(editor, s_calculatorExpression);
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(calculator, editor) && document_contains(editor, " = 6"),
           "bundled calculator accepts commas between function arguments");

    s_calculatorExpression = "1,000 * 2";
    set_document(editor, s_calculatorExpression);
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    s_lastMessage[0] = 0;
    expect(run_ok(calculator, editor) && document_is(editor, "1,000 * 2") &&
           strstr(s_lastMessage, "Commas can only separate function arguments"),
           "bundled calculator rejects thousands separators instead of misreading them");

    set_document(editor,
        "fact(5)\n"
        "gcd(84, 30)\n"
        "round(avg(1, 2, 8), 2)\n"
        "percent(250, 12)\n"
        "ans + 5");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(calculator, editor) &&
           document_is(editor,
               "fact(5) = 120\n"
               "gcd(84, 30) = 6\n"
               "round(avg(1, 2, 8), 2) = 3.67\n"
               "percent(250, 12) = 30\n"
               "ans + 5 = 35"),
           "bundled calculator provides common functions and carries ans between lines");

    set_document(editor, "2 + 3 = 999");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(calculator, editor) && document_is(editor, "2 + 3 = 5"),
           "bundled calculator refreshes an existing result");

    s_calculatorExpression = "lcm(12, 18) + percent(200, 5)";
    set_document(editor, "");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(calculator, editor) &&
           document_is(editor, "lcm(12, 18) + percent(200, 5) = 46"),
           "bundled calculator prompts for an expression when there is no selection");

    set_document(editor, "hello world");
    SendMessageA(editor, SCI_SETSEL, 0, 5);
    expect(encode64 >= 0 && run_ok(encode64, editor) && document_is(editor, "aGVsbG8= world"),
           "bundled Base64 encoder");
    SendMessageA(editor, SCI_SETSEL, 0, 8);
    expect(decode64 >= 0 && run_ok(decode64, editor) && document_is(editor, "hello world"),
           "bundled Base64 decoder");

    set_document(editor, "AB==");
    SendMessageA(editor, SCI_SETSEL, 0, 4);
    s_lastMessage[0] = 0;
    expect(run_ok(decode64, editor) && document_is(editor, "AB==") &&
           strstr(s_lastMessage, "not valid Base64"),
           "bundled Base64 decoder rejects non-canonical padding bits");

    set_document(editor, "AAEC/w==");
    SendMessageA(editor, SCI_SETSEL, 0, 8);
    s_lastMessage[0] = 0;
    expect(run_ok(decode64, editor) && document_is(editor, "AAEC/w==") &&
           strstr(s_lastMessage, "binary data"),
           "bundled Base64 decoder refuses to insert binary data");
    set_document(editor, "AB==");
    SendMessageA(editor, SCI_SETSEL, 0, 4);
    s_lastMessage[0] = 0;
    expect(run_ok(encodingDecode64, editor) && document_is(editor, "AB==") &&
           strstr(s_lastMessage, "isn't valid Base64"),
           "Encode > Base64 Decode rejects non-canonical padding bits");
    set_document(editor, "aGk=");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(encodingDecode64, editor) && document_is(editor, "hi"),
           "Encode > Base64 Decode still decodes canonical padded text");
    set_document(editor, "h\xc3\xa9llo");
    SendMessageA(editor, SCI_SETSEL, 0, 6);
    s_lastMessage[0] = 0;
    expect(run_ok(encodingDecode64, editor) && strstr(s_lastMessage, "\"\xc3\xa9\" isn't a Base64 character"),
           "Encode > Base64 Decode names a non-ASCII character it can't decode");
    set_document(editor, "ff fe");
    SendMessageA(editor, SCI_SETSEL, 0, 5);
    s_lastMessage[0] = 0;
    expect(run_ok(decodeHex, editor) && document_is(editor, "ff fe") &&
           strstr(s_lastMessage, "binary data"),
           "bundled hexadecimal decoder refuses to insert binary data");
    set_document(editor, "%FF");
    SendMessageA(editor, SCI_SETSEL, 0, 3);
    s_lastMessage[0] = 0;
    expect(run_ok(decodeUrl, editor) && document_is(editor, "%FF") &&
           strstr(s_lastMessage, "binary data"),
           "bundled URL decoder refuses to insert binary data");

    set_document(editor, "keep\ndrop 1\ndrop 2\nkeep too");
    SendMessageA(editor, SCI_SETSEL, 5, 18);
    expect(removeContaining >= 0 && run_ok(removeContaining, editor) &&
           document_is(editor, "keep\nkeep too"),
           "removing every selected line leaves no blank line behind");
    set_document(editor, "keep\ndrop 1\ndrop 2");
    SendMessageA(editor, SCI_SETSEL, 5, 18);
    expect(run_ok(removeContaining, editor) && document_is(editor, "keep"),
           "removing the last lines also removes the line ending before them");

    set_document(editor, "a b+c");
    SendMessageA(editor, SCI_SETSEL, 0, 5);
    expect(encodeUrl >= 0 && run_ok(encodeUrl, editor) && document_is(editor, "a%20b%2Bc"),
           "bundled URL encoder");
    SendMessageA(editor, SCI_SETSEL, 0, 9);
    expect(decodeUrl >= 0 && run_ok(decodeUrl, editor) && document_is(editor, "a b+c"),
           "bundled URL decoder");

    set_document(editor, "Hi!");
    SendMessageA(editor, SCI_SETSEL, 0, 3);
    expect(encodeHex >= 0 && run_ok(encodeHex, editor) && document_is(editor, "486921"),
           "bundled hexadecimal encoder");
    SendMessageA(editor, SCI_SETSEL, 0, 6);
    expect(decodeHex >= 0 && run_ok(decodeHex, editor) && document_is(editor, "Hi!"),
           "bundled hexadecimal decoder");

    set_document(editor, "<a&b>");
    SendMessageA(editor, SCI_SETSEL, 0, 5);
    expect(encodeHtml >= 0 && run_ok(encodeHtml, editor) &&
           document_is(editor, "&lt;a&amp;b&gt;"), "bundled HTML entity encoder");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(decodeHtml >= 0 && run_ok(decodeHtml, editor) && document_is(editor, "<a&b>"),
           "bundled HTML entity decoder");

    set_document(editor, "name;note\nAda;\"hello; world\"\n");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvToTab, editor) && document_is(editor, "name\tnote\nAda\thello; world\n"),
           "bundled CSV plugin parses quoted semicolon data and converts it to TSV");

    set_document(editor, "name\tage\nAda\t36\nBob\t40");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvToComma, editor) && document_is(editor, "name,age\nAda,36\nBob,40"),
           "bundled CSV plugin converts TSV to CSV");
    set_document(editor, "name\tage\n\"Ada\"\t36\nBob\t");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvToComma, editor) && document_is(editor, "name,age\nAda,36\nBob,"),
           "bundled CSV plugin reads quoted fields before a tab separator");

    set_document(editor, "before\nname;age\nAda;36\nafter");
    SendMessageA(editor, SCI_SETSEL, 7, 22);
    expect(run_ok(csvToComma, editor) &&
           document_is(editor, "before\nname,age\nAda,36\nafter") &&
           SendMessageA(editor, SCI_GETSELECTIONSTART, 0, 0) == 7 &&
           SendMessageA(editor, SCI_GETSELECTIONEND, 0, 0) == 22,
           "bundled CSV plugin transforms only the selected data range");

    set_document(editor, "name age\nAda 36\nBob 40");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvToComma, editor) && document_is(editor, "name age\nAda 36\nBob 40"),
           "bundled CSV plugin never auto-detects whitespace as the separator");

    test_large_documents(editor, jsonPretty, jsonMinify, jsonSort, csvToTab, csvFormat, clefFormat);

    set_document(editor, "name;age\nAda;36\nBob;40");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvToJson, editor) &&
           document_is(editor, "[\n  {\n    \"name\": \"Ada\",\n    \"age\": \"36\"\n  },\n  {\n    \"name\": \"Bob\",\n    \"age\": \"40\"\n  }\n]"),
           "bundled CSV plugin converts delimited rows to JSON objects");

    set_document(editor, "name,age\nAda,36\nAda,36\nBob,40");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvDedupe, editor) && document_is(editor, "name,age\nAda,36\nBob,40"),
           "bundled CSV plugin removes duplicate data rows");

    set_document(editor, "name,age\nAda,36\nBob,40");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvTranspose, editor) && document_is(editor, "name,Ada,Bob\nage,36,40"),
           "bundled CSV plugin transposes rows and columns");

    set_document(editor, "name,age\nAda,36\nBob,40");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_outputText[0] = 0;
    expect(run_ok(csvInspect, editor) && strstr(s_outputText, "Separator: comma") &&
           strstr(s_outputText, "Rows: 3") && strstr(s_outputText, "Columns: 2"),
           "bundled CSV plugin inspects delimited data");

    set_document(editor, "name,note\nAda,un\"quoted");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(csvInspect, editor) && document_is(editor, "name,note\nAda,un\"quoted") &&
           strstr(s_lastMessage, "line 2, column 7") &&
           strstr(s_lastMessage, "quoted field must begin"),
           "bundled CSV plugin rejects quotes inside unquoted fields");
    set_document(editor, "x\ry\rname,note\rAda,un\"quoted");
    SendMessageA(editor, SCI_SETSEL, 4, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    s_lastMessage[0] = 0;
    expect(run_ok(csvInspect, editor) && strstr(s_lastMessage, "line 4, column 7"),
           "CSV errors in a selection are reported at their document line, with CR endings");

    set_document(editor, "name,age\nAlexandra,9\nBo,12");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvFormat, editor) &&
           document_is(editor, "name     ,age\nAlexandra,9\nBo       ,12"),
           "bundled CSV plugin aligns columns without adding quotes");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvFormat, editor) &&
           document_is(editor, "name     ,age\nAlexandra,9\nBo       ,12"),
           "formatting aligned CSV again leaves it unchanged");
    set_document(editor, "a,b\n\"x,y\",\"say \"\"hi\"\"\"");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvFormat, editor) &&
           document_is(editor, "a    ,b\n\"x,y\",\"say \"\"hi\"\"\""),
           "CSV formatting keeps fields that were quoted as they were");
    set_document(editor, "id, name, city\n1, Alexandra, Oslo\n22, Bo, \"Rome, IT\"");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvFormat, editor) &&
           document_is(editor, "id,name     ,city\n1 ,Alexandra,Oslo\n22,Bo       ,\"Rome, IT\""),
           "CSV formatting aligns spaced-out separators without adding quotes");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvFormat, editor) &&
           document_is(editor, "id,name     ,city\n1 ,Alexandra,Oslo\n22,Bo       ,\"Rome, IT\""),
           "formatting spaced-out CSV twice leaves it unchanged");
    set_document(editor, "  a ,  b  \n ccc,d   ");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvFormat, editor) && document_is(editor, "a  ,b\nccc,d"),
           "CSV formatting trims blanks around fields and at line ends");

    set_document(editor, "{\"z\":0,\"a\":[1,true,null]}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(jsonPretty, editor) &&
           document_is(editor, "{\n  \"z\": 0,\n  \"a\": [\n    1,\n    true,\n    null\n  ]\n}"),
           "bundled JSON plugin pretty-prints the whole document");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(jsonMinify, editor) && document_is(editor, "{\"z\":0,\"a\":[1,true,null]}"),
           "bundled JSON plugin minifies a selection");

    set_document(editor, "{\"z\":0,\"a\":{\"z\":1,\"a\":2}}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(jsonSort, editor) &&
           document_is(editor, "{\n  \"a\": {\n    \"a\": 2,\n    \"z\": 1\n  },\n  \"z\": 0\n}"),
           "bundled JSON plugin recursively sorts object keys");

    set_document(editor, "{\"name\":\"Tinker Editor\",\"enabled\":true,\"items\":[1,{\"value\":null}]}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(jsonToYaml, editor) &&
           document_is(editor, "\"name\": \"Tinker Editor\"\n\"enabled\": true\n\"items\":\n  - 1\n  -\n    \"value\": null"),
           "bundled JSON plugin converts JSON to YAML");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(yamlToJson, editor) &&
           document_is(editor, "{\n  \"name\": \"Tinker Editor\",\n  \"enabled\": true,\n  \"items\": [\n    1,\n    {\n      \"value\": null\n    }\n  ]\n}"),
           "bundled JSON plugin converts its YAML output back to JSON");

    set_document(editor, "name: Example # comment\nnote: 'it''s # data'\nports:\n  - 80\n  - 443\nowner:\n  first: Ada\n  active: TRUE");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(yamlToJson, editor) &&
           document_is(editor, "{\n  \"name\": \"Example\",\n  \"note\": \"it's # data\",\n  \"ports\": [\n    80,\n    443\n  ],\n  \"owner\": {\n    \"first\": \"Ada\",\n    \"active\": true\n  }\n}"),
           "bundled JSON plugin reads common YAML mappings, sequences, comments and scalars");
    set_document(editor, "-   a: 1\n    b: 2");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(yamlToJson, editor) &&
           document_is(editor, "[\n  {\n    \"a\": 1,\n    \"b\": 2\n  }\n]"),
           "bundled JSON plugin reads a sequence item's mapping at its own indent");
    set_document(editor, "{\"a\":\"\\\xc3\xa9\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(jsonPretty, editor) && strstr(s_lastMessage, "\\\xc3\xa9"),
           "bundled JSON plugin names a non-ASCII bad escape in its error");

    set_document(editor, "[1,]");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(jsonPretty, editor) && document_is(editor, "[1,]") &&
           strstr(s_lastMessage, "line 1, column 4") && strstr(s_lastMessage, "Trailing commas"),
           "bundled JSON plugin reports the precise error without changing text");
    expect(SendMessageA(editor, SCI_INDICATORVALUEAT, 17, 3) != 0,
           "bundled JSON plugin highlights the invalid token");

    const char *invalidJson[] = { "[1 2]", "{\"a\" 1}", "{\"a\":}", "[truefalse]",
        "[01]", "[1.]", "[1e+]", "[+1]", "[NaN]", "[undefined]", "[,1]",
        "{1:2}", "{\"a\":1 \"b\":2}", "[\"\\q\"]", "[\"\\u12xz\"]",
        "[\"a\nb\"]", "{}{}", "[1}", "true false", "[\"unterminated]" };
    int invalidJsonCount = (int)(sizeof(invalidJson) / sizeof(invalidJson[0]));
    int invalidJsonRejected = 1;
    for (int i = 0; i < invalidJsonCount; i++) {
        set_document(editor, invalidJson[i]);
        SendMessageA(editor, SCI_SETSEL, 0, 0);
        s_lastMessage[0] = 0;
        invalidJsonRejected = invalidJsonRejected && run_ok(jsonPretty, editor) &&
            strstr(s_lastMessage, "Invalid JSON") != NULL && document_is(editor, invalidJson[i]);
    }
    expect(invalidJsonRejected, "bundled JSON plugin rejects malformed JSON forms");

    /* Errors are reported at their document line, not the line within the
       selection or code block, whatever the line endings. */
    set_document(editor, "a\rb\r[1,]");
    SendMessageA(editor, SCI_SETSEL, 4, 8);
    s_lastMessage[0] = 0;
    expect(run_ok(jsonPretty, editor) && strstr(s_lastMessage, "line 3, column 4"),
           "JSON errors in a selection are reported at their document line, with CR endings");
    set_document(editor, "before\n```json\n[1,]\n```");
    SendMessageA(editor, SCI_SETSEL, 16, 16);
    s_lastMessage[0] = 0;
    expect(run_ok(jsonPretty, editor) && strstr(s_lastMessage, "line 3, column 4"),
           "JSON errors in a fenced block are reported at their document line");

    set_document(editor, "before\n```json\n{\"a\":1}\n```\nafter");
    SendMessageA(editor, SCI_SETSEL, 18, 18);
    expect(run_ok(jsonPretty, editor) &&
           document_is(editor, "before\n```json\n{\n  \"a\": 1\n}\n```\nafter"),
           "bundled JSON plugin formats the fenced block at the caret");

    set_document(editor, "line 1\n\"line 2\"");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(jsonEscape, editor) && document_is(editor, "\"line 1\\n\\\"line 2\\\"\""),
           "bundled JSON plugin escapes a selection");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(jsonUnescape, editor) && document_is(editor, "line 1\n\"line 2\""),
           "bundled JSON plugin unescapes a JSON string");

    set_document(editor, "Hello, World!");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(rot13, editor) && document_is(editor, "Uryyb, Jbeyq!"),
           "bundled ROT13 transform");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(rot13, editor) && document_is(editor, "Hello, World!"),
           "bundled ROT13 transform is reversible");

    set_document(editor, "Abc-Z");
    SendMessageA(editor, SCI_SETSEL, 0, 5);
    expect(run_ok(atbash, editor) && document_is(editor, "Zyx-A"),
           "bundled Atbash transform preserves case and punctuation");

    set_document(editor, "SOS 2");
    SendMessageA(editor, SCI_SETSEL, 0, 5);
    expect(run_ok(encodeMorse, editor) && document_is(editor, "... --- ... / ..---"),
           "bundled Morse encoder");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(decodeMorse, editor) && document_is(editor, "SOS 2"),
           "bundled Morse decoder");

    set_document(editor, "Hi 5");
    SendMessageA(editor, SCI_SETSEL, 0, 4);
    expect(run_ok(encodeNato, editor) && document_is(editor, "Hotel India / Five"),
           "bundled NATO phonetics encoder");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(decodeNato, editor) && document_is(editor, "HI 5"),
           "bundled NATO phonetics decoder");

    set_document(editor, "Hi!");
    SendMessageA(editor, SCI_SETSEL, 0, 3);
    expect(run_ok(encodeBinary, editor) &&
           document_is(editor, "01001000 01101001 00100001"), "bundled binary encoder");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(decodeBinary, editor) && document_is(editor, "Hi!"),
           "bundled binary decoder");

    set_document(editor, "Hello, apple and rhythm!");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(pigLatin, editor) && document_is(editor, "Ellohay, appleway andway rhythmay!"),
           "bundled Pig Latin transform");

    set_document(editor, "Bright sparks dance! Bright ideas sing! Bright colours shine!");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_outputText[0] = 0;
    s_outputShown = 0;
    expect(run_ok(personality, editor) && strstr(s_outputText, "Document mood: Enthusiastic") &&
           strstr(s_outputText, "Favourite word: bright (3 times)") &&
           strstr(s_outputText, "Document Personality - note.md") && s_outputShown,
           "bundled document personality reports mood and favourite word in the output panel");
    int reportsInOutput = find_command("Document Personality/Reports in Output Panel");
    s_lastMessage[0] = 0;
    expect(reportsInOutput >= 0 && run_ok(reportsInOutput, editor) && run_ok(personality, editor) &&
           strstr(s_lastMessage, "Document mood: Enthusiastic") && run_ok(reportsInOutput, editor),
           "bundled document personality can report in a message box instead");
    expect(run_ok(clearPersonality, editor), "bundled document personality clears highlights");

    set_document(editor, "Short sentences work. Clear prose helps.");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_outputText[0] = 0;
    expect(run_ok(checkConstraints, editor) &&
           strstr(s_outputText, "All active constraints are satisfied."),
           "bundled constraint writer checks its default sentence limit");
    set_document(editor,
        "one two three four five six seven eight nine ten eleven twelve thirteen fourteen "
        "fifteen sixteen seventeen eighteen nineteen twenty twentyone.");
    s_outputText[0] = 0;
    expect(run_ok(checkConstraints, editor) &&
           strstr(s_outputText, "Sentence limit (20 words): 1 too long"),
           "bundled constraint writer reports a long sentence");
    expect(run_ok(highlightConstraints, editor) && run_ok(clearConstraints, editor),
           "bundled constraint writer manages issue highlights");

    SendMessageA(editor, SCI_SETEOLMODE, SC_EOL_LF, 0);
    set_document(editor, "# Intro\n\n## Details\n\n```md\n# Hidden\n```");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(toc >= 0 && run_ok(toc, editor) &&
           document_contains(editor, "- [Intro](#intro)\n  - [Details](#details)") &&
           !document_contains(editor, "[Hidden](#hidden)"),
           "bundled TOC generator ignores fenced headings");

    set_document(editor, "## Two\n### Three");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(promoteHeading, editor) && document_is(editor, "# Two\n## Three"),
           "Markdown structure tools promote selected headings");

    set_document(editor, "7. outer\n  9. nested\n3. outer again");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(renumberLists, editor) &&
           document_is(editor, "1. outer\n  1. nested\n2. outer again"),
           "Markdown structure tools renumber nested ordered lists");

    set_document(editor, "- [ ] one\n- [x] two");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(toggleTasks, editor) && document_is(editor, "- [x] one\n- [x] two"),
           "Markdown structure tools toggle task states as a group");

    set_document(editor, "Read [the guide](https://example.test/guide).");
    expect(run_ok(inlineToReferences, editor) &&
           document_contains(editor, "Read [the guide][1].\n\n[1]: https://example.test/guide"),
           "Markdown link tools convert inline links to references");
    expect(run_ok(referencesToInline, editor) &&
           document_is(editor, "Read [the guide](https://example.test/guide).\n"),
           "Markdown link tools convert references back to inline links");

    set_document(editor, "Existing [old][1] and [new](new).\n\n[1]: old");
    expect(run_ok(inlineToReferences, editor) &&
           document_contains(editor, "[new][2]") && document_contains(editor, "[2]: new"),
           "inline-link conversion does not collide with existing reference ids");
    set_document(editor, "Use [old][1].\n\n[1]: old\n[unused]: keep");
    expect(run_ok(referencesToInline, editor) &&
           document_contains(editor, "Use [old](old).") &&
           document_contains(editor, "[unused]: keep"),
           "reference conversion preserves definitions it did not use");

    set_document(editor, "# First  \n\n### Third\n\n[Missing](#nowhere)");
    s_problemSource[0] = s_problemText[0] = 0;
    expect(run_ok(lintMarkdown, editor) && strcmp(s_problemSource, "Markdown Linter") == 0 &&
           strstr(s_problemText, "Trailing whitespace") &&
           strstr(s_problemText, "Heading level jumps") &&
           strstr(s_problemText, "does not resolve"),
           "Markdown linter reports structure, whitespace and link problems");

    set_document(editor, "One two. Three!");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_outputText[0] = 0;
    expect(run_ok(statistics, editor) && strstr(s_outputText, "Words: 3") &&
           strstr(s_outputText, "Characters: 15") && strstr(s_outputText, "Sentences: 2"),
           "selection statistics reports document counts and reading time");

    set_document(editor, "alpha\n\n  \nbeta\n");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(removeBlank >= 0 && run_ok(removeBlank, editor) && document_is(editor, "alpha\nbeta\n"),
           "bundled line tool removes blank selected lines");

    set_document(editor, "zebra\nApple\nbanana\n");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(sortLines >= 0 && run_ok(sortLines, editor) &&
           document_is(editor, "Apple\nbanana\nzebra\n"),
           "bundled line tool sorts selected lines case-insensitively");

    set_document(editor, "longest\nx\nmedium\n");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(sortLength >= 0 && run_ok(sortLength, editor) &&
           document_is(editor, "x\nmedium\nlongest\n"),
           "bundled line tool sorts selected lines by length");

    set_document(editor, "one\ntwo\nthree\nfour\n");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(shuffle >= 0 && run_ok(shuffle, editor) &&
           !document_is(editor, "one\ntwo\nthree\nfour\n") &&
           document_contains(editor, "one") && document_contains(editor, "two") &&
           document_contains(editor, "three") && document_contains(editor, "four"),
           "bundled line tool shuffles selected lines");

    int align = find_command("Line Tools/Align on Delimiter...");
    set_document(editor, "a = 1\nlong_name = 2\nno delimiter");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    s_alignDelimiter = "=";
    expect(align >= 0 && run_ok(align, editor) &&
           document_is(editor, "a         = 1\nlong_name = 2\nno delimiter"),
           "Align on Delimiter lines up = and leaves other lines alone");
    set_document(editor, "|a|bb|\n|ccc|d|");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    s_alignDelimiter = "|";
    expect(run_ok(align, editor) && document_is(editor, "| a   | bb |\n| ccc | d  |"),
           "Align on Delimiter lines up every column of a table");
    set_document(editor, "a: 1\nlong: 2");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    s_alignDelimiter = ":";
    expect(run_ok(align, editor) && document_is(editor, "a:    1\nlong: 2"),
           "Align on Delimiter pads after a colon");

    static const struct { const char *from, *to; int toIso; } dates[] = {
        { "0", "1970-01-01T00:00:00Z", 1 },
        { "1700000000", "2023-11-14T22:13:20Z", 1 },
        { "1700000000123", "2023-11-14T22:13:20.123Z", 1 },
        { "-1", "1969-12-31T23:59:59Z", 1 },
        { "2000-01-01", "946684800", 0 },
        { "2024-02-29T12:00:00+01:00", "1709204400", 0 },
        { "2023-11-14 22:13:20.123Z", "1700000000123", 0 },
    };
    int toIso = find_command("/Tools/Number Converter/Unix Timestamp to ISO Date (UTC)");
    int toUnix = find_command("/Tools/Number Converter/ISO Date to Unix Timestamp");
    for (size_t i = 0; i < sizeof(dates) / sizeof(dates[0]); i++) {
        set_document(editor, dates[i].from);
        SendMessageA(editor, SCI_SETSEL, 0, (LPARAM)strlen(dates[i].from));
        int ok = run_ok(dates[i].toIso ? toIso : toUnix, editor) && document_is(editor, dates[i].to);
        if (!ok) fprintf(stderr, "%s should convert to %s\n", dates[i].from, dates[i].to);
        expect(ok, "Number Converter converts Unix timestamps and ISO dates");
    }
    set_document(editor, "2023-02-29");
    SendMessageA(editor, SCI_SETSEL, 0, 10);
    s_lastMessage[0] = 0;
    expect(run_ok(toUnix, editor) && document_is(editor, "2023-02-29") &&
           strstr(s_lastMessage, "Not a supported ISO 8601 date"),
           "ISO Date to Unix Timestamp rejects a day the month doesn't have");

    int readability = find_command("Document Personality/Readability");
    set_document(editor, "# Pets\n\nThe cat sat on the mat. It was happy.\n\n```\nnot prose at all\n```");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_outputText[0] = 0;
    expect(readability >= 0 && run_ok(readability, editor) &&
           strstr(s_outputText, "Flesch reading ease: 110 (very easy)") &&
           strstr(s_outputText, "Words: 10  |  Sentences: 3  |  Syllables: 11"),
           "Readability scores prose, with headings as sentences and code left out");

    int luaRun = find_command("/Tools/Interpreters/Lua/Run Selection");
    int luaEvaluate = find_command("/Tools/Interpreters/Lua/Evaluate Selection");
    set_document(editor, "print('a', 1 + 1)\nio.write('b')\nreturn 5");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    s_outputText[0] = 0;
    expect(luaRun >= 0 && luaEvaluate >= 0 && run_ok(luaRun, editor) &&
           strstr(s_outputText, "a\t2\r\nb=> 5\r\n"),
           "Lua runner prints, writes and shows returned values");
    set_document(editor, "-- first\nlocal x\nx()");
    SendMessageA(editor, SCI_SETSEL, 9, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    s_outputText[0] = 0;
    expect(run_ok(luaRun, editor) && strstr(s_outputText, "note.md:3: attempt to call"),
           "Lua runner reports errors at their document line");
    set_document(editor, "2^10");
    SendMessageA(editor, SCI_SETSEL, 0, 4);
    s_outputText[0] = 0;
    expect(run_ok(luaEvaluate, editor) && strstr(s_outputText, "=> 1024.0"),
           "Lua runner evaluates an expression");
    set_document(editor, "leaked = 1");
    SendMessageA(editor, SCI_SETSEL, 0, 10);
    run_ok(luaRun, editor);
    set_document(editor, "print(leaked, io.open, os, menu)");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    s_outputText[0] = 0;
    expect(run_ok(luaRun, editor) && strstr(s_outputText, "nil\tnil\tnil\tnil\r\n"),
           "Lua runner code has no file access and keeps no globals between runs");
    set_document(editor, "io.write('Name? ')\nassert(io.flush())\nlocal n = io.read()\nprint('hi ' .. n)\nprint(io.read('n'))");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    s_outputText[0] = 0;
    s_terminalInputCancelled = 0;
    expect(run_ok(luaRun, editor) &&
           strstr(s_outputText, "Name? 42\r\nhi 42\r\nInput for the Lua script: 42\r\n42"),
           "Lua runner flushes prompts, then reads and echoes terminal input");
    set_document(editor,
        "assert(io.type(io.stdin) == 'file')\n"
        "assert(io.type(io.stdout) == 'file')\n"
        "assert(io.stdout:write('stream'):flush())\n"
        "io.stderr:write(' error')\n"
        "assert(io.read(2, 'l') == '42')");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    s_outputText[0] = 0;
    expect(run_ok(luaRun, editor) && strstr(s_outputText, "stream error42\r\n"),
           "Lua runner exposes stream proxies and shares buffered input formats");
    set_document(editor, "assert(io.read('*a') == '42\\n42\\n')\nprint('all input')");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    s_outputText[0] = 0;
    s_terminalInputsRemaining = 2;
    expect(run_ok(luaRun, editor) && strstr(s_outputText, "all input\r\n"),
           "Lua runner reads all terminal input until end-of-input");
    s_terminalInputsRemaining = -1;
    set_document(editor, "io.write('cancel')\nprint(io.read())");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    s_outputText[0] = 0;
    s_terminalInputCancelled = 1;
    expect(run_ok(luaRun, editor) && strstr(s_outputText, "cancelnil\r\n"),
           "Lua runner input ends when terminal input is cancelled");
    s_terminalInputCancelled = 0;
    set_document(editor, "local ok = pcall(io.read)\nprint(ok)");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    s_terminalInputInterrupted = 1;
    expect(run_fails(luaRun, editor, "interrupted"),
           "Lua terminal Ctrl+C cannot be swallowed by a script pcall");
    s_terminalInputInterrupted = 0;

    int luaFormatDocument = find_command("/Tools/Interpreters/Lua/Format Document");
    int luaFormatSelection = find_command("/Tools/Interpreters/Lua/Format Selection");
    set_document(editor, "if a then\nx = 1\nend\nif b then\ny = 2\nend");
    SendMessageA(editor, SCI_SETSEL, 22, 32);
    expect(luaFormatSelection >= 0 && run_ok(luaFormatSelection, editor) &&
           document_is(editor, "if a then\nx = 1\nend\nif b then\n  y = 2\nend") &&
           SendMessageA(editor, SCI_GETSELECTIONSTART, 0, 0) == 20 &&
           SendMessageA(editor, SCI_GETSELECTIONEND, 0, 0) == 37,
           "Lua > Format Selection reindents only the selected lines and keeps them selected");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(luaFormatSelection, editor) && strstr(s_lastMessage, "Select the lines") &&
           document_is(editor, "if a then\nx = 1\nend\nif b then\n  y = 2\nend"),
           "Lua > Format Selection asks for a selection and changes nothing without one");
    SendMessageA(editor, SCI_SETSEL, 22, 32);
    expect(luaFormatDocument >= 0 && run_ok(luaFormatDocument, editor) &&
           document_is(editor, "if a then\n  x = 1\nend\nif b then\n  y = 2\nend"),
           "Lua > Format Document reindents the whole file even with a selection");

    int luaCheckSyntax = find_command("/Tools/Interpreters/Lua/Check Syntax");
    set_document(editor, "#!/usr/bin/env lua\nif a then\n  x = 1\n");
    s_outputText[0] = 0;
    expect(luaCheckSyntax >= 0 && run_ok(luaCheckSyntax, editor) &&
           strstr(s_outputText, "C:\\docs\\note.md:4: 'end' expected (to close 'if' at line 2) near <eof>"),
           "Lua > Check Syntax reports the first error at its document line, skipping a #! line");
    set_document(editor, "#!/usr/bin/env lua\nif a then\n  x = 1\nend\n");
    s_outputText[0] = 0;
    expect(run_ok(luaCheckSyntax, editor) && strstr(s_outputText, "No syntax errors.") &&
           document_is(editor, "#!/usr/bin/env lua\nif a then\n  x = 1\nend\n"),
           "Lua > Check Syntax accepts valid code without running or changing it");
    int luaCheckSelection = find_command("/Tools/Interpreters/Lua/Check Selection Syntax");
    set_document(editor, "# Notes\n\n```lua\nif x then\n  y = 1\n```\n");
    SendMessageA(editor, SCI_SETSEL, 16, 33);
    s_outputText[0] = 0;
    expect(luaCheckSelection >= 0 && run_ok(luaCheckSelection, editor) &&
           strstr(s_outputText, "C:\\docs\\note.md:5: 'end' expected (to close 'if' at line 4) near <eof>"),
           "Lua > Check Selection Syntax checks only the selection and reports document lines");
    set_document(editor, "# Notes\n\n```lua\nlocal x = 1\nprint(x)\n```\n");
    SendMessageA(editor, SCI_SETSEL, 16, 37);
    s_outputText[0] = 0;
    expect(run_ok(luaCheckSelection, editor) && strstr(s_outputText, "Lines 4-5: No syntax errors."),
           "Lua > Check Selection Syntax names the lines it found valid");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(luaCheckSelection, editor) && strstr(s_lastMessage, "Select the Lua code"),
           "Lua > Check Selection Syntax asks for a selection when there is none");

    int follow = find_command("/View/Follow EOF");
    set_document(editor, "first\nsecond\nthird");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_autoReload = 0;
    expect(follow >= 0 && run_ok(follow, editor) && s_autoReload &&
           SendMessageA(editor, SCI_GETCURRENTPOS, 0, 0) == 18,
           "Follow EOF reloads the file by itself and goes to its end");
    expect(plugin_host_command_is_checkable(follow) && plugin_host_command_is_checked(follow),
           "Follow EOF is checked while the active tab is followed");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    char *followError = plugin_host_fire("reload", "C:\\docs\\note.md", editor);
    expect(!followError && SendMessageA(editor, SCI_GETCURRENTPOS, 0, 0) == 18,
           "a followed file goes to its end each time it reloads");
    free(followError);
    s_autoReload = 0;
    followError = plugin_host_fire("tab_changed", "C:\\docs\\note.md", editor);
    expect(!followError && !plugin_host_command_is_checked(follow),
           "Follow EOF is unchecked after switching to a tab that isn't followed");
    free(followError);
    s_autoReload = 1;
    followError = plugin_host_fire("tab_changed", "C:\\docs\\note.md", editor);
    expect(!followError && plugin_host_command_is_checked(follow),
           "Follow EOF is checked again after switching back to the followed tab");
    free(followError);
    expect(run_ok(follow, editor) && !s_autoReload, "Follow EOF switches off again");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    followError = plugin_host_fire("reload", "C:\\docs\\note.md", editor);
    expect(!followError && SendMessageA(editor, SCI_GETCURRENTPOS, 0, 0) == 0,
           "a file no longer followed keeps its place when it reloads");
    free(followError);
    expect(run_ok(load_ok("auto-reload.lua",
               "menu.add('Auto reload', function()\n"
               "  assert(documents.auto_reload(true) == true and documents.auto_reload() == true)\n"
               "  assert(documents.auto_reload(1, false) == false and documents.auto_reload(1) == false)\n"
               "  assert(not pcall(documents.auto_reload, 1, 'yes'), 'needs a boolean')\n"
               "end)\n"), editor),
           "documents.auto_reload reads and sets the setting, for the active or a given document");
}

static char *s_jsonYamlDbFolder;

static char *fake_pick_db_folder(const char *title, int folder) {
    (void)title; (void)folder;
    if (!s_jsonYamlDbFolder) return NULL;
    size_t length = strlen(s_jsonYamlDbFolder) + 1;
    char *copy = (char *)malloc(length);
    if (copy) memcpy(copy, s_jsonYamlDbFolder, length);
    return copy;
}

/* json-store.lua and yaml-store.lua each carry a hand-written codec with no
   other test coverage, since they're ordinary bundled plugins rather than C
   code. Exercise their actual commands end to end - decode a selection,
   save it to a real sqlite file, read it back, re-encode it - rather than
   trusting that plugin_host_load_directory() merely loading them without
   error (test_bundled_plugins, above) says anything about whether they
   work. */
static void test_json_yaml_store(HWND editor) {
    WCHAR tempRoot[MAX_PATH];
    expect(GetTempPathW(MAX_PATH, tempRoot) > 0, "temp path available");
    size_t rootLength = wcslen(tempRoot);
    if (rootLength && tempRoot[rootLength - 1] == L'\\') tempRoot[rootLength - 1] = 0;
    char *folderUtf8 = to_utf8(tempRoot);
    s_jsonYamlDbFolder = folderUtf8;

    static const PluginHostCallbacks fakes = {
        .prompt = fake_prompt, .choose = fake_choose, .pick_path = fake_pick_db_folder,
        .config_get = fake_config_get, .config_set = fake_config_set,
        .output_write = fake_output_write, .output_show = fake_output_show,
        .message = fake_message
    };
    plugin_host_set_callbacks(&fakes);
    s_configValue[0] = 0; /* force each plugin's first call to pick a fresh folder */

    int jsonLength = 0, yamlLength = 0;
    char *jsonSource = read_bytes_file(L"plugins\\json-store.lua", &jsonLength);
    expect(jsonSource != NULL, "plugins/json-store.lua is readable");
    char *yamlSource = read_bytes_file(L"plugins\\yaml-store.lua", &yamlLength);
    expect(yamlSource != NULL, "plugins/yaml-store.lua is readable");

    if (jsonSource && yamlSource) {
        int json = load_ok("json-store.lua", jsonSource);
        int yaml = load_ok("yaml-store.lua", yamlSource);
        /* Registration order in each file: Save, Insert, View, List, Delete, Choose. */
        enum { SAVE = 0, INSERT = 1, VIEW = 2 };

        set_document(editor, "{\"name\": \"Ada\", \"tags\": [\"x\", \"y\"], \"active\": true, \"count\": 3}");
        SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
        expect(run_ok(json + SAVE, editor), "JSON Store saves a decoded, validated selection");
        s_outputText[0] = 0;
        expect(run_ok(json + VIEW, editor) && strstr(s_outputText, "\"active\": true") &&
               strstr(s_outputText, "\"count\": 3") && strstr(s_outputText, "\"name\": \"Ada\"") &&
               strstr(s_outputText, "\"x\"") && strstr(s_outputText, "\"y\""),
               "JSON Store round-trips an object through sqlite");
        set_document(editor, "");
        expect(run_ok(json + INSERT, editor), "JSON Store inserts the stored value back as pretty JSON");
        char *insertedJson = document_text(editor);
        expect(insertedJson && strstr(insertedJson, "\"name\": \"Ada\"") && strstr(insertedJson, "true"),
               "the inserted JSON matches what was saved");
        free(insertedJson);

        s_lastMessage[0] = 0;
        set_document(editor, "not json at all {");
        SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
        expect(run_ok(json + SAVE, editor) && strstr(s_lastMessage, "isn't valid JSON"),
               "JSON Store reports a syntax error instead of saving garbage");

        set_document(editor, "name: Ada\ntags:\n  - x\n  - y\nactive: true\ncount: 3\n");
        SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
        expect(run_ok(yaml + SAVE, editor), "YAML Store saves a decoded, validated selection");
        s_outputText[0] = 0;
        expect(run_ok(yaml + VIEW, editor) && strstr(s_outputText, "\"active\": true") &&
               strstr(s_outputText, "\"count\": 3") && strstr(s_outputText, "\"name\": \"Ada\"") &&
               strstr(s_outputText, "- \"x\"") && strstr(s_outputText, "- \"y\""),
               "YAML Store round-trips a mapping through sqlite");
        set_document(editor, "");
        expect(run_ok(yaml + INSERT, editor), "YAML Store inserts the stored value back as YAML");
        char *insertedYaml = document_text(editor);
        expect(insertedYaml && strstr(insertedYaml, "\"name\": \"Ada\"") && strstr(insertedYaml, "- \"x\""),
               "the inserted YAML matches what was saved");
        free(insertedYaml);

        s_lastMessage[0] = 0;
        set_document(editor, "key: [unterminated");
        SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
        expect(run_ok(yaml + SAVE, editor) && strstr(s_lastMessage, "isn't valid YAML"),
               "YAML Store reports a syntax error instead of saving garbage");
    }
    free(jsonSource);
    free(yamlSource);

    WCHAR dbFile[MAX_PATH];
    swprintf(dbFile, MAX_PATH, L"%ls\\tinkeredit-json-store.db", tempRoot);
    DeleteFileW(dbFile);
    swprintf(dbFile, MAX_PATH, L"%ls\\tinkeredit-yaml-store.db", tempRoot);
    DeleteFileW(dbFile);
    free(folderUtf8);
    s_jsonYamlDbFolder = NULL;
    plugin_host_shutdown();
}

static char *s_browserDbFolder, *s_browserDbPath;

/* ui.pick_folder (folder = 1) and ui.pick_file (folder = 0) both reach this,
   since sqlite-browser.lua's "Create New Database..." uses the former and
   "Open Existing Database..." the latter. */
static char *fake_pick_browser_path(const char *title, int folder) {
    (void)title;
    const char *source = folder ? s_browserDbFolder : s_browserDbPath;
    if (!source) return NULL;
    size_t length = strlen(source) + 1;
    char *copy = (char *)malloc(length);
    if (copy) memcpy(copy, source, length);
    return copy;
}

/* sqlite-browser.lua carries its own Markdown-table renderer, SQL statement
   splitter, and Markdown-table parser, none of which are covered anywhere
   else - exercise its actual commands: create a database, run a multi-
   statement script, read the result back as a Markdown table, describe the
   table, and import a Markdown table into a new one. */
static void test_sqlite_browser(HWND editor) {
    WCHAR tempRoot[MAX_PATH], dbPath[MAX_PATH];
    expect(GetTempPathW(MAX_PATH, tempRoot) > 0, "temp path available");
    size_t rootLength = wcslen(tempRoot);
    if (rootLength && tempRoot[rootLength - 1] == L'\\') tempRoot[rootLength - 1] = 0;
    swprintf(dbPath, MAX_PATH, L"%ls\\test-browser.db", tempRoot);
    DeleteFileW(dbPath);
    char *folderUtf8 = to_utf8(tempRoot);
    char *dbPathUtf8 = to_utf8(dbPath);
    s_browserDbFolder = folderUtf8;
    s_browserDbPath = dbPathUtf8;

    static const PluginHostCallbacks fakes = {
        .prompt = fake_prompt, .choose = fake_choose, .pick_path = fake_pick_browser_path,
        .config_get = fake_config_get, .config_set = fake_config_set,
        .output_write = fake_output_write, .output_show = fake_output_show,
        .message = fake_message
    };
    plugin_host_set_callbacks(&fakes);
    s_configValue[0] = 0; /* force a fresh "no database open yet" state */

    int sourceLength = 0;
    char *source = read_bytes_file(L"plugins\\sqlite-browser.lua", &sourceLength);
    expect(source != NULL, "plugins/sqlite-browser.lua is readable");
    if (source) {
        int first = load_ok("sqlite-browser.lua", source);
        /* Registration order in the file: Open Existing, Create New, Current
           Database, Run SQL, Run SQL (Enter Manually), Show Tables, Describe
           Table, Import Markdown. */
        enum { BROWSER_OPEN_EXISTING = 0, BROWSER_CREATE_NEW = 1, BROWSER_CURRENT = 2, BROWSER_RUN_SQL = 3,
               BROWSER_RUN_SQL_MANUAL = 4, BROWSER_SHOW_TABLES = 5, BROWSER_DESCRIBE = 6, BROWSER_IMPORT = 7 };

        set_document(editor, "");
        expect(run_ok(first + BROWSER_RUN_SQL, editor) && strstr(s_lastMessage, "Open or create a database"),
               "Run SQL asks for a database before one is open");

        expect(run_ok(first + BROWSER_CREATE_NEW, editor) && strstr(s_lastMessage, "Created"),
               "Create New Database creates and selects a database file");

        s_configValue[0] = 0; /* forget the database Create New just selected */
        expect(run_ok(first + BROWSER_OPEN_EXISTING, editor) && strstr(s_lastMessage, "Opened"),
               "Open Existing Database reopens the same file by path");

        set_document(editor,
            "create table staff (id integer primary key, name text, age integer);\n"
            "insert into staff (name, age) values ('Ada', 36);\n"
            "insert into staff (name, age) values ('Grace', 85);\n"
            "select name, age from staff order by name;\n");
        SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
        s_outputText[0] = 0;
        expect(run_ok(first + BROWSER_RUN_SQL, editor) &&
               strstr(s_outputText, "| name | age |") &&
               strstr(s_outputText, "| Ada | 36 |") &&
               strstr(s_outputText, "| Grace | 85 |") &&
               strstr(s_outputText, "2 row(s)."),
               "Run SQL executes a multi-statement script and renders the final query as a table");

        set_document(editor, "update staff set age = age + 1;");
        SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
        s_outputText[0] = 0;
        expect(run_ok(first + BROWSER_RUN_SQL, editor) && strstr(s_outputText, "2 row(s) affected."),
               "Run SQL reports rows affected for a non-query statement");

        set_document(editor, "");
        s_manualSql = "select name, age from staff order by name;";
        s_outputText[0] = 0;
        expect(run_ok(first + BROWSER_RUN_SQL_MANUAL, editor) &&
               strstr(s_outputText, "| Ada | 37 |") && strstr(s_outputText, "| Grace | 86 |"),
               "Run SQL (Enter Manually) runs SQL typed into the prompt, ignoring the document");
        s_manualSql = NULL;

        s_outputText[0] = 0;
        expect(run_ok(first + BROWSER_SHOW_TABLES, editor) && strstr(s_outputText, "staff"),
               "Show Tables lists the created table");

        s_outputText[0] = 0;
        expect(run_ok(first + BROWSER_DESCRIBE, editor) &&
               strstr(s_outputText, "| name |") && strstr(s_outputText, "| age |"),
               "Describe Table reports the table's columns");

        expect(run_ok(first + BROWSER_CURRENT, editor) && strstr(s_lastMessage, "Current database:"),
               "Current Database reports the open database's path");

        set_document(editor,
            "| city | population |\n"
            "| --- | --- |\n"
            "| Paris | 2148000 |\n"
            "| Oslo | 697000 |\n");
        SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
        expect(run_ok(first + BROWSER_IMPORT, editor) && strstr(s_lastMessage, "2 row(s) imported"),
               "Import Markdown Table creates a table and inserts its rows");

        /* The imported table is named "people" - fake_prompt's fixed reply
           to "Table name:" - distinct from the script-created "staff". */
        set_document(editor, "select city, population from people order by population;");
        SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
        s_outputText[0] = 0;
        expect(run_ok(first + BROWSER_RUN_SQL, editor) &&
               strstr(s_outputText, "| Oslo | 697000 |") && strstr(s_outputText, "| Paris | 2148000 |"),
               "the imported rows are queryable, with population imported as a number");
    }
    free(source);
    free(folderUtf8);
    free(dbPathUtf8);
    DeleteFileW(dbPath);
    s_browserDbFolder = NULL;
    s_browserDbPath = NULL;
    plugin_host_shutdown();
}

/* ---- a stand-in Windows dictionary, for spell and the anagram plugin ---- */
static const char *const s_dictionary[] = {
    "listen", "silent", "enlist", "tinsel", "inlets", "dirty", "room", "dormitory", "the"
};
static char s_chooseLabel[256], s_choosePick[64];
static int s_chooseCount;

static int in_dictionary(const char *word, size_t length) {
    for (size_t i = 0; i < sizeof(s_dictionary) / sizeof(s_dictionary[0]); i++)
        if (strlen(s_dictionary[i]) == length && strncmp(s_dictionary[i], word, length) == 0)
            return 1;
    return 0;
}

static const char *s_spellLanguage = "en-GB";

static char *fake_spell_language(void) {
    char *copy = (char *)malloc(strlen(s_spellLanguage) + 1);
    if (copy) strcpy(copy, s_spellLanguage);
    return copy;
}

/* Like Windows, text is accepted when every space-separated word in it is. */
static int fake_spell_check(const char *const *words, int count, unsigned char *misspelled) {
    for (int i = 0; i < count; i++) {
        misspelled[i] = 0;
        for (const char *word = words[i]; *word; ) {
            size_t length = strcspn(word, " ");
            if (length && !in_dictionary(word, length)) misspelled[i] = 1;
            word += length + (word[length] == ' ');
        }
    }
    return 1;
}

/* Picks the item equal to s_choosePick, or the first one. */
static int fake_spell_choose(const char *title, const char *label, const char *const *items,
                             int count) {
    (void)title;
    snprintf(s_chooseLabel, sizeof(s_chooseLabel), "%s", label);
    s_chooseCount = count;
    for (int i = 0; i < count; i++) if (strcmp(items[i], s_choosePick) == 0) return i;
    return 0;
}

static char *fake_cancel_prompt(const char *title, const char *label, const char *initial) {
    (void)title; (void)label; (void)initial;
    return NULL;
}

static void test_spelling(HWND editor) {
    static const PluginHostCallbacks fakes = {
        .spell_language = fake_spell_language, .spell_check = fake_spell_check
    };
    plugin_host_set_callbacks(&fakes);
    int first = load_ok("spell.lua",
        "menu.add('Check', function()\n"
        "  assert(spell.language() == 'en-GB')\n"
        "  assert(spell.check('listen') == true and spell.check('lsiten') == false)\n"
        "  assert(spell.check('dirty room') and not spell.check('dirty rooom'))\n"
        "  local results = spell.check({ 'silent', 'xx', 'the' })\n"
        "  assert(#results == 3 and results[1] and not results[2] and results[3])\n"
        "  assert(#spell.check({}) == 0)\n"
        "  assert(#spell.check(setmetatable({}, { __len = function() return -8198552921648689607 end })) == 0)\n"
        "  assert(not pcall(spell.check, { 'a', 5 }), 'non-string word')\n"
        "  assert(not pcall(spell.check, 5), 'not a word or table')\n"
        "  assert(app.capabilities().spelling)\n"
        "end)\n"
        "menu.add('Language', function() assert(spell.language() == nil) end)\n"
        "menu.add('Check one', function() spell.check('word') end)\n");
    expect(run_ok(first, editor), "spell.check checks words and lists of words");

    plugin_host_set_callbacks(NULL);
    expect(run_ok(first + 1, editor), "spell.language is nil without a dictionary");
    expect(run_fails(first + 2, editor, "not available"), "spell.check needs a dictionary");
}

static int run_anagram(int index, HWND editor, const char *text, const char *pick) {
    set_document(editor, text);
    SendMessageA(editor, SCI_SETSEL, 0, (LPARAM)strlen(text));
    snprintf(s_choosePick, sizeof(s_choosePick), "%s", pick);
    s_chooseLabel[0] = 0;
    s_chooseCount = 0;
    return run_ok(index, editor);
}

/* An unsaved document. */
static char *fake_untitled_path(void) {
    return NULL;
}

/* Loads one bundled plugin on its own, with the stand-in dictionary. */
static void load_spelling_plugin(const WCHAR *path, const char *name) {
    static const PluginHostCallbacks fakes = {
        .prompt = fake_cancel_prompt, .choose = fake_spell_choose, .message = fake_message,
        .status = fake_status, .file_path = fake_untitled_path,
        .spell_language = fake_spell_language, .spell_check = fake_spell_check,
        .output_write = fake_output_write, .output_show = fake_output_show,
        .problems_report = fake_problems_report
    };
    plugin_host_set_callbacks(&fakes);
    plugin_host_shutdown();
    size_t length = 0;
    char *code = read_utf8_text_file_length(path, 1024 * 1024, &length);
    expect(code != NULL, name);
    if (!code) return;
    char *error = plugin_host_load_chunk(name, code, length);
    free(code);
    if (error) fprintf(stderr, "%s\n", error);
    expect(error == NULL, name);
    free(error);
}

static void test_spelling_report(HWND editor) {
    load_spelling_plugin(L"plugins\\spelling-report.lua", "spelling-report.lua");
    int report = find_command("/Tools/Spelling Report...");
    expect(report >= 0, "Spelling Report is registered");
    const char *text = "the lsiten, `codeword` [the](http://x.y/zz) www.foo.com xx9 \xE2\x80\x9Clsiten\xE2\x80\x9D";
    set_document(editor, text);
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    snprintf(s_choosePick, sizeof(s_choosePick), "lsiten  (2)");
    s_chooseLabel[0] = 0;
    expect(run_ok(report, editor) && s_chooseCount == 1 &&
           strstr(s_chooseLabel, "1 misspelled of 2 different words"),
           "Spelling Report lists the words the dictionary rejects, skipping code and links");
    expect(SendMessageA(editor, SCI_GETSELECTIONS, 0, 0) == 2 &&
           SendMessageA(editor, SCI_GETSELECTIONNSTART, 0, 0) == 4 &&
           SendMessageA(editor, SCI_GETSELECTIONNEND, 0, 0) == 10,
           "Choosing a misspelling selects every occurrence");

    set_document(editor, "the silent room.");
    s_lastMessage[0] = 0;
    expect(run_ok(report, editor) && strstr(s_lastMessage, "No spelling mistakes in the document's 3 words"),
           "Spelling Report says when there are no mistakes");
    plugin_host_shutdown();
}

static void test_grammar_languages(HWND editor) {
    load_spelling_plugin(L"plugins\\grammar-checker.lua", "grammar-checker.lua");
    int check = find_command("/Tools/Grammar Checker/Check");
    set_document(editor, "He dont care,, they was here.");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_outputText[0] = 0;
    expect(check >= 0 && run_ok(check, editor) && strstr(s_outputText, "'don't'") &&
           strstr(s_outputText, "Line 1, column 4: ") && strstr(s_outputText, "Use 'were'"),
           "grammar checker reports a missing apostrophe the dictionary rejects");
    s_spellLanguage = "de-DE";
    s_outputText[0] = 0;
    expect(run_ok(check, editor) && strstr(s_outputText, "Found 1 possible issue:") &&
           strstr(s_outputText, "Repeated ','"),
           "grammar checker skips English rules for another dictionary language");
    s_spellLanguage = "en-GB";
    plugin_host_shutdown();
}


static void test_output_api(HWND editor) {
    static const PluginHostCallbacks fakes = {
        .output_write = fake_output_write, .output_clear = fake_output_clear,
        .output_show = fake_output_show, .output_hide = fake_output_hide,
        .output_visible = fake_output_visible,
        .problems_report = fake_problems_report,
        .problems_clear_all = fake_problems_clear_all
    };
    plugin_host_set_callbacks(&fakes);
    int first = load_ok("output.lua",
        "menu.add('Write', function()\n"
        "  output.write('stale')\n"
        "  output.clear()\n"
        "  output.write('a', 'b\\n')\n"
        "  assert(not output.visible())\n"
        "  output.show()\n"
        "  assert(output.visible())\n"
        "  output.hide()\n"
        "  assert(not output.visible())\n"
        "  output.show()\n"
        "  assert(not pcall(output.write, {}), 'output.write needs strings')\n"
        "  problems.report('test', 'file:1: issue')\n"
        "  problems.clear_all()\n"
        "  assert(app.capabilities().output_panel)\n"
        "end)\n");
    s_outputText[0] = 0;
    s_outputShown = 0;
    snprintf(s_problemSource, sizeof(s_problemSource), "old");
    snprintf(s_problemText, sizeof(s_problemText), "old problem");
    expect(run_ok(first, editor) && strcmp(s_outputText, "ab\n") == 0 && s_outputShown &&
           !s_problemSource[0] && !s_problemText[0],
           "clear APIs reach the Output and Problems panels");
    plugin_host_set_callbacks(NULL);
    expect(run_fails(first, editor, "not available"), "the output API needs the panel");
}

static int only_anagrams(const char *scope, const char *name) {
    (void)scope;
    return strcmp(name, "anagrams") == 0;
}

/* The anagram plugin is a folder extension (its word list is a module
   beside init.lua), so it loads from the bundled folder on its own. */
static void load_anagram_plugin(void) {
    static const PluginHostCallbacks fakes = {
        .prompt = fake_cancel_prompt, .choose = fake_spell_choose, .message = fake_message,
        .status = fake_status, .file_path = fake_untitled_path, .plugin_enabled = only_anagrams,
        .spell_language = fake_spell_language, .spell_check = fake_spell_check,
    };
    plugin_host_set_callbacks(&fakes);
    plugin_host_shutdown();
    char *error = plugin_host_load_directory(L"plugins", "bundled");
    if (error) fprintf(stderr, "%s\n", error);
    expect(error == NULL, "anagrams loads as a folder extension");
    free(error);
}

static void test_anagram_plugin(HWND editor) {
    load_anagram_plugin();
    int solve = find_command("Anagrams (British English)/Solve Anagram...");
    int words = find_command("Anagrams (British English)/Words From Letters...");
    int phrases = find_command("Anagrams (British English)/Create Anagram Phrases...");
    int scramble = find_command("Anagrams (British English)/Scramble Words");
    expect(solve >= 0 && words >= 0 && phrases >= 0 && scramble >= 0,
           "anagram commands are registered");

    /* English uses the bundled British word list. */
    expect(run_anagram(solve, editor, "Listen", "silent") && s_chooseCount == 4 &&
           strstr(s_chooseLabel, "4 anagrams of \"listen\"") && document_is(editor, "Silent"),
           "Solve Anagram offers every other word and keeps the capital");
    s_lastMessage[0] = 0;
    expect(run_anagram(solve, editor, "zyx", "") && strstr(s_lastMessage, "No single-word anagrams"),
           "Solve Anagram reports when there are none");
    expect(run_anagram(solve, editor, "certification", "rectification") && s_chooseCount == 1 &&
           document_is(editor, "rectification"),
           "Solve Anagram solves 13 letters at once from the word list");

    expect(run_anagram(words, editor, "listen", "") && s_chooseCount == 62 &&
           document_contains(editor, "6 letters: enlist, inlets, listen, silent, tinsel") &&
           document_contains(editor, " est,") == 0 && document_contains(editor, " isl,") == 0 &&
           document_contains(editor, " inst,") == 0 && document_contains(editor, " int,") == 0,
           "Words From Letters inserts every listed word, grouped by length, without abbreviations");
    expect(run_anagram(words, editor, "olucor", "colour") && document_is(editor, "colour"),
           "the word list uses British spellings");

    expect(run_anagram(phrases, editor, "Dormitory", "dirty room") &&
           document_is(editor, "Dirty room"),
           "Create Anagram Phrases finds phrases of several words");

    /* Other languages check arrangements against the Windows dictionary. */
    s_spellLanguage = "de-DE";
    expect(run_anagram(solve, editor, "Listen", "silent") && s_chooseCount == 4 &&
           document_is(editor, "Silent"),
           "Solve Anagram checks arrangements with the dictionary for other languages");
    s_lastMessage[0] = 0;
    expect(run_anagram(solve, editor, "certification", "") &&
           strstr(s_lastMessage, "too many arrangements"),
           "Solve Anagram still limits arrangements for other languages");
    expect(run_anagram(words, editor, "listen", "") && s_chooseCount == 6 &&
           document_is(editor, "6 letters: enlist, inlets, listen, silent, tinsel"),
           "Words From Letters checks arrangements with the dictionary for other languages");
    s_spellLanguage = "en-GB";

    expect(run_anagram(scramble, editor, "listen, to it", ""), "Scramble Words runs");
    char text[64];
    SendMessageA(editor, SCI_GETTEXT, sizeof(text), (LPARAM)text);
    expect(strlen(text) == 13 && !in_dictionary(text, 6) && strcmp(text + 6, ", ot ti") == 0,
           "Scramble Words avoids jumbles that are words");
    /* Sorting the jumble's letters gives listen's. */
    for (int i = 0; i < 6; i++)
        for (int j = i + 1; j < 6; j++)
            if (text[j] < text[i]) { char swap = text[i]; text[i] = text[j]; text[j] = swap; }
    expect(strncmp(text, "eilnst", 6) == 0, "Scramble Words keeps a word's letters");
    plugin_host_shutdown();
}

int main(void) {
    /* Resolve the DLL relative to this executable, including Premake builds. */
    WCHAR path[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return 1;
    WCHAR *slash = wcsrchr(path, L'\\');
    if (!slash || (size_t)(slash - path) + 19 >= MAX_PATH) return 1;
    wcscpy(slash + 1, L"..\\Scintilla.dll");
    HMODULE scintilla = LoadLibraryW(path);
    if (!scintilla) { fprintf(stderr, "Cannot load Scintilla.dll\n"); return 1; }
    HWND editor = CreateWindowExA(0, "Scintilla", "", WS_POPUP, 0, 0, 100, 100,
                                  NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!editor) { FreeLibrary(scintilla); return 1; }

    static const PluginHostCallbacks fakes = {
        .prompt = fake_prompt, .choose = fake_choose, .file_path = fake_file_path,
        .language = fake_language, .message = fake_message, .config_get = fake_config_get,
        .config_set = fake_config_set, .save_binary = fake_save_binary,
        .schedule_deferred = fake_schedule_deferred
    };
    plugin_host_set_callbacks(&fakes);
    int ziptoolLength = 0;
    char *ziptoolSource = read_bytes_file(L"src\\lua\\ziptool.lua", &ziptoolLength);
    if (!ziptoolSource) { fprintf(stderr, "Cannot load src/lua/ziptool.lua\n"); FreeLibrary(scintilla); return 1; }
    plugin_host_set_ziptool_source(ziptoolSource, (size_t)ziptoolLength);
    test_commands(editor);
    test_menu_enabled(editor);
    test_separators(editor);
    test_context_menu(editor);
    test_editor_api(editor);
    test_checkboxes_and_config(editor);
    test_ui(editor);
    test_zip_api(editor);
    test_sqlite_api(editor);
    test_deferred(editor);
    test_events(editor);
    test_load_failures();
    test_environment();
    test_isolation(editor);
    test_time_limit();
    test_read_relative_file(editor);
    test_directory(editor);
    test_bundled_plugins(editor);
    test_json_yaml_store(editor);
    test_sqlite_browser(editor);
    test_spelling(editor);
    test_output_api(editor);
    test_anagram_plugin(editor);
    test_spelling_report(editor);
    test_grammar_languages(editor);
    plugin_host_shutdown();

    DestroyWindow(editor);
    FreeLibrary(scintilla);
    if (s_ok) puts("All plugin host tests passed.");
    return s_ok ? 0 : 1;
}
