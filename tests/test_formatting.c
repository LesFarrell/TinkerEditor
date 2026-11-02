/* Built-in Markdown formatting and the bundled HTML snippets plugin. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/menu_commands.h"
#include "../src/plugin_host.h"
#include "../src/file_io.h"
#include "../third_party/scintilla/include/Scintilla.h"

static int s_ok = 1;
static char s_lastMessage[256];
static const char *s_language = "markdown";

static const char *fake_language(void) {
    return s_language;
}

static char *read_formatting_source(void) {
    static const WCHAR *const paths[] = {
        L"src\\lua\\formatting_base.lua", L"src\\lua\\formatting_tables.lua",
        L"src\\lua\\formatting_text.lua",
    };
    char *joined = NULL;
    size_t used = 0;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        char *part = read_utf8_text_file(paths[i], 1024 * 1024);
        if (!part) { free(joined); return NULL; }
        size_t length = strlen(part);
        char *grown = (char *)realloc(joined, used + length + 2);
        if (!grown) { free(part); free(joined); return NULL; }
        joined = grown;
        memcpy(joined + used, part, length);
        used += length;
        joined[used++] = '\n';
        joined[used] = 0;
        free(part);
    }
    return joined;
}

static void expect(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", what);
        s_ok = 0;
    }
}

/* Answers for ui.prompt, in order; NULL cancels. */
static const char *s_answers[4];
static int s_answerCount, s_nextAnswer;

static char *fake_prompt(const char *title, const char *label, const char *initial) {
    (void)title;
    (void)label;
    (void)initial;
    if (s_nextAnswer >= s_answerCount || !s_answers[s_nextAnswer]) return NULL;
    const char *answer = s_answers[s_nextAnswer++];
    char *copy = (char *)malloc(strlen(answer) + 1);
    if (copy) strcpy(copy, answer);
    return copy;
}

static void answer_with(const char *rows, const char *columns) {
    s_answers[0] = rows;
    s_answers[1] = columns;
    s_answerCount = 2;
    s_nextAnswer = 0;
}

static void record_message(const char *title, const char *text, const char *kind) {
    (void)title;
    (void)kind;
    snprintf(s_lastMessage, sizeof(s_lastMessage), "%s", text);
}

/* Runs a built-in command by name on the whole of text (selected), and
   returns the document afterwards. */
static const char *run_with_selection(HWND editor, const char *name, const char *text,
                                      size_t length, Sci_Position start,
                                      Sci_Position finish, int eolMode) {
    static char result[8192];
    s_lastMessage[0] = 0;
    SendMessageA(editor, SCI_SETEOLMODE, (WPARAM)eolMode, 0);
    SendMessageA(editor, SCI_CLEARALL, 0, 0);
    SendMessageA(editor, SCI_ADDTEXT, (WPARAM)length, (LPARAM)text);
    SendMessageA(editor, SCI_SETSEL, (WPARAM)start, (LPARAM)finish);
    int index = plugin_host_find_override(name);
    char *error = index >= 0 ? plugin_host_run(index, editor) : NULL;
    if (error) fprintf(stderr, "%s: %s\n", name, error);
    expect(index >= 0 && !error, name);
    free(error);
    SendMessageA(editor, SCI_GETTEXT, sizeof(result), (LPARAM)result);
    return result;
}

static const char *run(HWND editor, const char *name, const char *text, size_t length, int eolMode) {
    return run_with_selection(editor, name, text, length, 0, (Sci_Position)length, eolMode);
}

static const char *run_at(HWND editor, const char *name, const char *text,
                          Sci_Position caret, int eolMode) {
    return run_with_selection(editor, name, text, strlen(text), caret, caret, eolMode);
}

static int find_plugin_command(const char *label) {
    for (int index = 0; index < plugin_host_command_count(); index++) {
        const char *candidate = plugin_host_command_label(index);
        if (candidate && strcmp(candidate, label) == 0) return index;
    }
    return -1;
}

static const char *run_plugin(HWND editor, const char *label, const char *text,
                              size_t length, int eolMode) {
    static char result[8192];
    s_lastMessage[0] = 0;
    SendMessageA(editor, SCI_SETEOLMODE, (WPARAM)eolMode, 0);
    SendMessageA(editor, SCI_CLEARALL, 0, 0);
    SendMessageA(editor, SCI_ADDTEXT, (WPARAM)length, (LPARAM)text);
    SendMessageA(editor, SCI_SETSEL, 0, (LPARAM)length);
    int index = find_plugin_command(label);
    char *error = index >= 0 ? plugin_host_run(index, editor) : NULL;
    if (error) fprintf(stderr, "%s: %s\n", label, error);
    expect(index >= 0 && !error, label);
    free(error);
    SendMessageA(editor, SCI_GETTEXT, sizeof(result), (LPARAM)result);
    return result;
}

static void test_registration(void) {
    static const char *const textCommands[] = { "edit.trim_whitespace", "edit.toggle_comment" };
    for (int i = 0; menu_command_at(i); i++) {
        const char *name = menu_command_at(i)->name;
        int inLua = strncmp(name, "markdown.", 9) == 0;
        for (size_t t = 0; t < sizeof(textCommands) / sizeof(textCommands[0]); t++)
            if (strcmp(name, textCommands[t]) == 0) inLua = 1;
        if (inLua && plugin_host_find_override(name) < 0) fprintf(stderr, "Not registered: %s\n", name);
        if (inLua) expect(plugin_host_find_override(name) >= 0, "every formatting and text command is in Lua");
    }
}

static void test_html_commands(HWND editor) {
    static const char *const labels[] = {
        "/Snippets/HTML/&HTML", "/Snippets/HTML/H&ead", "/Snippets/HTML/&Body",
        "/Snippets/HTML/&Paragraph", "/Snippets/HTML/B&old", "/Snippets/HTML/&Italic",
        "/Snippets/HTML/&Underline", "/Snippets/HTML/&Div", "/Snippets/HTML/&Span",
        "/Snippets/HTML/&Link", "/Snippets/HTML/I&mage", "/Snippets/HTML/&Table",
        "/Snippets/HTML/B&ullet List", "/Snippets/HTML/&Numbered List",
        "/Snippets/HTML/Line &Break", "/Snippets/HTML/HTML5 Document Boilerplate",
        "/Snippets/HTML/Details and Summary", "/Snippets/HTML/Figure...",
        "/Snippets/HTML/Description List", "/Snippets/HTML/Wrap with Tag...",
        "/Snippets/HTML/Convert Selected Markdown Links",
        "/Snippets/HTML/Semantic/Section", "/Snippets/HTML/Semantic/Article",
        "/Snippets/HTML/Semantic/Navigation", "/Snippets/HTML/Semantic/Aside",
        "/Snippets/HTML/Semantic/Header", "/Snippets/HTML/Semantic/Footer",
        "/Snippets/HTML/Semantic/Main", "/Snippets/HTML/Semantic/Code",
        "/Snippets/HTML/Semantic/Preformatted"
    };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
        expect(find_plugin_command(labels[i]) >= 0, "every HTML snippet command is registered");
    expect(strcmp(run_plugin(editor, "/Snippets/HTML/B&old", "word", 4, SC_EOL_LF),
                  "<b>word</b>") == 0, "HTML tag command wraps text");
    expect(strcmp(run_plugin(editor, "/Snippets/HTML/B&old", "<b>word</b>", 11, SC_EOL_LF),
                  "word") == 0, "HTML tag command unwraps text");
    expect(strcmp(run_plugin(editor, "/Snippets/HTML/B&ullet List", "one\ntwo", 7, SC_EOL_LF),
                  "<ul>\n  <li>one</li>\n  <li>two</li>\n</ul>") == 0,
           "HTML list command wraps selected lines");
    expect(strcmp(run_plugin(editor, "/Snippets/HTML/Semantic/Section", "content", 7, SC_EOL_LF),
                  "<section>content</section>") == 0,
           "HTML semantic command wraps selected text");
    expect(strcmp(run_plugin(editor, "/Snippets/HTML/Convert Selected Markdown Links",
                             "[site](https://example.test)", 28, SC_EOL_LF),
                  "<a href=\"https://example.test\">site</a>") == 0,
           "HTML tools convert selected Markdown links");
}

static void test_table_commands(HWND editor) {
    const char *prose = "a | b\nc | d";
    expect(strcmp(run_at(editor, "markdown.table_column_delete", prose, 0, SC_EOL_LF), prose) == 0 &&
           strstr(s_lastMessage, "inside a Markdown table"),
           "pipe-containing prose is not treated as a table");

    const char *unequal = "a | b | c\n--- | ---\n1 | 2";
    expect(strcmp(run_at(editor, "markdown.table_column_delete", unequal, 0, SC_EOL_LF), unequal) == 0 &&
           strstr(s_lastMessage, "inside a Markdown table"),
           "a delimiter with the wrong number of columns does not form a table");

    const char *table = "| a | b |\n| --- | --- |\n| 1 | 2 |";
    expect(strcmp(run_at(editor, "markdown.table_column_delete", table,
                         (Sci_Position)(strstr(table, "2 |") - table), SC_EOL_LF),
                  "| a |\n| --- |\n| 1 |") == 0,
           "a table with a delimiter row is recognised");

    const char *escaped = "| a \\| b | c |\n| --- | --- |\n| x | y |";
    expect(strcmp(run_at(editor, "markdown.table_column_delete", escaped,
                         (Sci_Position)(strstr(escaped, "c |") - escaped), SC_EOL_LF),
                  "| a \\| b |\n| --- |\n| x |") == 0,
           "escaped pipes remain cell text in a Markdown table");

    run_at(editor, "markdown.table_row_below", table,
           (Sci_Position)(strstr(table, "1 |") - table), SC_EOL_LF);
    expect(SendMessageA(editor, SCI_GETCURRENTPOS, 0, 0) == (LRESULT)strlen(table) + 1,
           "the caret enters an appended table row after its inserted line ending");
}

/* Runs name on the whole of input, checks the result, then that one Undo
   restores input. */
static void check_text_command(HWND editor, const char *name, const char *input, int eolMode,
                               const char *expected) {
    const char *after = run(editor, name, input, strlen(input), eolMode);
    if (strcmp(after, expected) != 0) fprintf(stderr, "%s on \"%s\" gave \"%s\"\n", name, input, after);
    expect(strcmp(after, expected) == 0, name);
    SendMessageA(editor, SCI_UNDO, 0, 0);
    char undone[512];
    SendMessageA(editor, SCI_GETTEXT, sizeof(undone), (LPARAM)undone);
    expect(strcmp(undone, input) == 0, "a text command is one undo step");
}

/* Edit commands that remain part of built-in Markdown-aware formatting.
   General Convert commands are tested with the bundled plugins. */
static void test_text_commands(HWND editor) {
    check_text_command(editor, "edit.trim_whitespace", "a  \nb\t\n  \nc", SC_EOL_LF, "a\nb\n\nc");
    s_language = "markdown";
    check_text_command(editor, "edit.toggle_comment", "a\nb", SC_EOL_LF, "<!--\na\nb\n-->");
    check_text_command(editor, "edit.toggle_comment", "<!--\na\nb\n-->", SC_EOL_LF, "a\nb");
    check_text_command(editor, "edit.toggle_comment", " <!-- \n\t-->", SC_EOL_CRLF, "");

    s_language = "css";
    check_text_command(editor, "edit.toggle_comment", "a\nb", SC_EOL_LF, "/*\na\nb\n*/");
    check_text_command(editor, "edit.toggle_comment", "/*\na\nb\n*/", SC_EOL_LF, "a\nb");

    s_language = "cpp";
    check_text_command(editor, "edit.toggle_comment", "int a;\nint b;", SC_EOL_LF,
                       "// int a;\n// int b;");
    check_text_command(editor, "edit.toggle_comment", "// int a;\n// int b;", SC_EOL_LF,
                       "int a;\nint b;");
    check_text_command(editor, "edit.toggle_comment", "  int a;\n\n  int b;", SC_EOL_LF,
                       "  // int a;\n\n  // int b;");

    s_language = "python";
    check_text_command(editor, "edit.toggle_comment", "a = 1", SC_EOL_LF, "# a = 1");

    s_language = "lua";
    check_text_command(editor, "edit.toggle_comment", "local a = 1", SC_EOL_LF, "-- local a = 1");
    check_text_command(editor, "edit.toggle_comment", "-- local a = 1", SC_EOL_LF, "local a = 1");

    s_language = "markdown";
}

/* The built-in Markdown and bundled HTML table commands both ask for a size. */
static void test_insert_tables(HWND editor) {
    answer_with("2", "2");
    expect(strcmp(run(editor, "markdown.table", "", 0, SC_EOL_LF),
                  "| Column 1 | Column 2 |\n| --- | --- |\n| Cell | Cell |\n| Cell | Cell |\n") == 0,
           "Markdown table of the size asked for");
    answer_with("1", "3");
    expect(strcmp(run(editor, "markdown.table", "", 0, SC_EOL_CRLF),
                  "| Column 1 | Column 2 | Column 3 |\r\n| --- | --- | --- |\r\n| Cell | Cell | Cell |\r\n") == 0,
           "the default Markdown table is the one it always was");
    answer_with("1", "1");
    expect(strcmp(run_plugin(editor, "/Snippets/HTML/&Table", "", 0, SC_EOL_LF),
                  "<table>\n  <tr>\n    <th>Column 1</th>\n  </tr>\n  <tr>\n    <td>Cell</td>\n  </tr>\n</table>\n") == 0,
           "HTML table of the size asked for");
    answer_with("1", "3");
    expect(strcmp(run_plugin(editor, "/Snippets/HTML/&Table", "", 0, SC_EOL_LF),
                  "<table>\n  <tr>\n    <th>Column 1</th>\n    <th>Column 2</th>\n    <th>Column 3</th>\n  </tr>\n"
                  "  <tr>\n    <td>Cell</td>\n    <td>Cell</td>\n    <td>Cell</td>\n  </tr>\n</table>\n") == 0,
           "the default HTML table is the one it always was");
    answer_with("2", NULL);
    expect(strcmp(run(editor, "markdown.table", "", 0, SC_EOL_LF), "") == 0, "cancelling inserts nothing");
    answer_with("0", "3");
    expect(strcmp(run_plugin(editor, "/Snippets/HTML/&Table", "", 0, SC_EOL_LF), "") == 0 &&
           strstr(s_lastMessage, "1–50"),
           "an out-of-range size is reported");
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

    static const PluginHostCallbacks callbacks = {
        .prompt = fake_prompt, .message = record_message, .language = fake_language
    };
    plugin_host_set_callbacks(&callbacks);
    char *code = read_formatting_source();
    char *error = code ? plugin_host_load_builtin("formatting.lua", code, strlen(code)) : NULL;
    if (error) fprintf(stderr, "%s\n", error);
    expect(code && !error, "built-in formatting Lua files load together");
    free(error);
    free(code);

    code = read_utf8_text_file(L"plugins\\html-tools.lua", 1024 * 1024);
    error = code ? plugin_host_load_chunk("html-tools.lua", code, strlen(code)) : NULL;
    if (error) fprintf(stderr, "%s\n", error);
    expect(code && !error, "bundled HTML snippets plugin loads");
    free(error);
    free(code);

    test_registration();
    test_html_commands(editor);
    test_insert_tables(editor);
    test_table_commands(editor);
    test_text_commands(editor);
    plugin_host_shutdown();

    DestroyWindow(editor);
    FreeLibrary(scintilla);
    if (s_ok) puts("All formatting tests passed.");
    return s_ok ? 0 : 1;
}
