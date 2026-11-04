#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/markdown_export.h"

static int failures = 0;

static void expect_text_contains(const char *name, const char *markdown, const char *expected) {
    char *text = markdown_to_plaintext(markdown);
    if (!text || !strstr(text, expected)) {
        fprintf(stderr, "FAIL %s\nexpected substring: %s\nactual: %s\n", name, expected, text ? text : "(null)");
        failures++;
    }
    free(text);
}

static void expect_text_not_contains(const char *name, const char *markdown, const char *unexpected) {
    char *text = markdown_to_plaintext(markdown);
    if (!text || strstr(text, unexpected)) {
        fprintf(stderr, "FAIL %s\nunexpected substring: %s\nactual: %s\n", name, unexpected, text ? text : "(null)");
        failures++;
    }
    free(text);
}

static void expect_rtf_contains(const char *name, const char *markdown, const char *expected) {
    char *rtf = markdown_to_rtf(markdown);
    if (!rtf || !strstr(rtf, expected)) {
        fprintf(stderr, "FAIL %s\nexpected substring: %s\nactual: %s\n", name, expected, rtf ? rtf : "(null)");
        failures++;
    }
    free(rtf);
}

static void expect_docx_contains(const char *name, const char *markdown, const char *expected) {
    char *xml = markdown_to_docx_document_xml(markdown);
    if (!xml || !strstr(xml, expected)) {
        fprintf(stderr, "FAIL %s\nexpected substring: %s\nactual: %s\n", name, expected, xml ? xml : "(null)");
        failures++;
    }
    free(xml);
}

int main(void) {
    /* plain text: formatting markers are stripped, structure stays readable */
    expect_text_contains("bold stripped", "This is **bold** text.\n", "This is bold text.");
    expect_text_contains("italic stripped", "This is *italic* text.\n", "This is italic text.");
    expect_text_not_contains("no asterisks remain", "**bold** and *italic*\n", "*");
    expect_text_contains("heading text kept", "# Title\n\nBody.\n", "Title");
    expect_text_not_contains("front matter hidden", "---\ntitle: Secret\n---\n\nBody.\n", "Secret");
    expect_text_contains("bullet list marker", "- one\n- two\n", "- one");
    expect_text_contains("ordered list marker", "1. first\n2. second\n", "1. first");
    expect_text_contains("link shows url", "[site](https://example.com)\n", "site (https://example.com)");
    expect_text_contains("blockquote prefixed", "> quoted line\n", "> quoted line");
    expect_text_contains("code block indented", "```\ncode here\n```\n", "    code here");

    /* RTF: a valid-looking envelope plus the expected control words */
    expect_rtf_contains("rtf envelope starts", "Hello\n", "{\\rtf1");
    expect_rtf_contains("rtf envelope ends", "Hello\n", "}");
    expect_rtf_contains("rtf bold group", "**bold**\n", "{\\b1 ");
    expect_rtf_contains("rtf italic group", "*italic*\n", "{\\i1 ");
    expect_rtf_contains("rtf heading size", "# Title\n", "\\fs36");
    expect_rtf_contains("rtf hyperlink field", "[site](https://example.com)\n", "HYPERLINK \"https://example.com\"");
    expect_rtf_contains("rtf escapes braces", "literal { brace }\n", "\\{");
    expect_rtf_contains("rtf unicode escape", "caf\xc3\xa9\n", "\\u233?");

    /* DOCX: valid-looking word/document.xml with the expected run markup */
    expect_docx_contains("docx envelope", "Hello\n", "<w:document ");
    expect_docx_contains("docx body closes", "Hello\n", "</w:body></w:document>");
    expect_docx_contains("docx bold run", "**bold**\n", "<w:b/>");
    expect_docx_contains("docx italic run", "*italic*\n", "<w:i/>");
    expect_docx_contains("docx heading size", "# Title\n", "w:val=\"36\"");
    expect_docx_contains("docx escapes ampersand", "A & B\n", "A &amp; B");
    expect_docx_contains("docx link shows url", "[site](https://example.com)\n", "(https://example.com)");
    expect_docx_contains("docx bullet marker", "- one\n", "\xE2\x80\xA2 ");

    size_t fixedPartCount = 0;
    const DocxFixedPart *fixedParts = docx_export_fixed_parts(&fixedPartCount);
    if (fixedPartCount != 2 || !fixedParts) {
        fprintf(stderr, "FAIL docx fixed parts count\nactual: %zu\n", fixedPartCount);
        failures++;
    }

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("All markdown export tests passed.\n");
    return 0;
}
