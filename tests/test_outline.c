#include <stdio.h>
#include <string.h>
#include "../src/outline_minimap.h"

/* outline_minimap.c reads these settings and saves the layout; the heading
   parser under test uses none of them. */
int g_editorDarkMode, g_outlineVisible, g_outlineOnLeft, g_minimapVisible;
int g_splitEditorVisible, g_splitEditorRatio, g_splitEditorVertical;
void save_layout_settings(void) {}

static int failures;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); failures++; } } while (0)

/* Parses `line` and checks the level and the extracted title. */
static void check_heading(const char *line, int expectedLevel, const char *expectedTitle) {
    int start = -1, length = -1;
    int level = outline_parse_atx_heading(line, (int)strlen(line), &start, &length);
    CHECK(level == expectedLevel);
    if (!expectedLevel) return;
    CHECK(length == (int)strlen(expectedTitle));
    CHECK(length >= 0 && strncmp(line + start, expectedTitle, (size_t)length) == 0);
}

static void test_atx_headings(void) {
    check_heading("# Title\n", 1, "Title");
    check_heading("###### Deep\r\n", 6, "Deep");
    check_heading("   ## Indented", 2, "Indented");
    check_heading("## Closed ##  \n", 2, "Closed");
    check_heading("## C# and F#\n", 2, "C# and F#");
    check_heading("##\tTabbed\n", 2, "Tabbed");
    check_heading("#NoSpace\n", 0, NULL);
    check_heading("    # Code block\n", 0, NULL);
    check_heading("####### Seven\n", 0, NULL);
    check_heading("#", 0, NULL);
    check_heading("Plain text\n", 0, NULL);
}

/* Feeds `lines` through one scan and records each line's heading level. */
static void scan_levels(const char *const *lines, int count, int *levels) {
    OutlineScan scan = {0};
    for (int i = 0; i < count; i++) {
        int start = 0, length = 0;
        levels[i] = outline_scan_line(&scan, lines[i], (int)strlen(lines[i]), &start, &length);
    }
}

static void test_code_fences(void) {
    static const char *const lines[] = {
        "# Title",          /* 1 */
        "```lua",           /* opens a backtick fence */
        "# not a heading",
        "~~~",              /* a tilde run doesn't close a backtick fence */
        "## still code",
        "```",              /* closes it */
        "## Section",       /* 2 */
        "~~~~",             /* opens a 4-tilde fence */
        "# code",
        "~~~ text",         /* trailing text: not a closing fence */
        "~~~",              /* too short to close a 4-tilde fence */
        "# code",
        "~~~~~",            /* closes it: at least as long */
        "### After",        /* 3 */
        "``not a fence",
        "# Real",           /* 1 */
    };
    enum { COUNT = sizeof(lines) / sizeof(lines[0]) };
    static const int expected[COUNT] = { 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 3, 0, 1 };
    int levels[COUNT];
    scan_levels(lines, COUNT, levels);
    for (int i = 0; i < COUNT; i++) {
        if (levels[i] != expected[i]) fprintf(stderr, "line %d: level %d, expected %d\n", i, levels[i], expected[i]);
        CHECK(levels[i] == expected[i]);
    }
}

int main(void) {
    test_atx_headings();
    test_code_fences();
    if (failures) return 1;
    puts("All outline tests passed.");
    return 0;
}
