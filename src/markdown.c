#include "markdown.h"

#include <stddef.h>
#include <string.h>

#include "cmark-gfm.h"
#include "cmark-gfm-core-extensions.h"

static int attach_extension(cmark_parser *parser, cmark_llist **render_extensions,
                            cmark_mem *mem, const char *name) {
    cmark_syntax_extension *extension = cmark_find_syntax_extension(name);
    if (!extension || !cmark_parser_attach_syntax_extension(parser, extension)) return 0;
    *render_extensions = cmark_llist_append(mem, *render_extensions, extension);
    return 1;
}

static const char *find_line_end(const char *start, const char *end) {
    while (start < end && *start != '\r' && *start != '\n') start++;
    return start < end ? start : NULL;
}

static const char *next_line(const char *line_end, const char *end) {
    if (*line_end == '\r' && line_end + 1 < end && line_end[1] == '\n') return line_end + 2;
    if (*line_end == '\r' || *line_end == '\n') return line_end + 1;
    return line_end;
}

/* Returns the Markdown body and its length, hiding a leading YAML front
   matter block. The length lets callers skip a second full scan of the
   document on every preview render. */
const char *skip_yaml_front_matter(const char *text, size_t text_length,
                                          size_t *body_length) {
    const char *start = text;
    const char *end = text + text_length;
    const char *line_end;

    if (text_length >= 3 &&
        (unsigned char)start[0] == 0xEF &&
        (unsigned char)start[1] == 0xBB &&
        (unsigned char)start[2] == 0xBF) {
        start += 3;
    }

    line_end = find_line_end(start, end);
    if (!line_end || line_end - start != 3 || memcmp(start, "---", 3) != 0) {
        *body_length = text_length;
        return text;
    }

    start = next_line(line_end, end);
    while (start < end) {
        size_t length;
        line_end = find_line_end(start, end);
        length = line_end ? (size_t)(line_end - start) : (size_t)(end - start);
        size_t trimmed = length;
        while (trimmed > 0 && (start[trimmed - 1] == ' ' || start[trimmed - 1] == '\t'))
            trimmed--;
        if (trimmed == 3 &&
            (memcmp(start, "---", 3) == 0 || memcmp(start, "...", 3) == 0)) {
            const char *body = line_end ? next_line(line_end, end) : start + length;
            *body_length = text_length - (size_t)(body - text);
            return body;
        }
        if (!line_end) break;
        start = next_line(line_end, end);
    }

    /* An unterminated opening marker is ordinary Markdown, not metadata. */
    *body_length = text_length;
    return text;
}

char *markdown_to_html_n(const char *text, size_t text_length) {
    static const char *const extension_names[] = {
        "table",
        "strikethrough",
        "autolink",
        "tagfilter",
        "tasklist"
    };
    const int options = CMARK_OPT_DEFAULT | CMARK_OPT_VALIDATE_UTF8 |
                        CMARK_OPT_FOOTNOTES | CMARK_OPT_STRIKETHROUGH_DOUBLE_TILDE |
                        CMARK_OPT_SOURCEPOS;
    cmark_parser *parser;
    cmark_node *document;
    cmark_mem *mem = cmark_get_default_mem_allocator();
    cmark_llist *render_extensions = NULL;
    char *html;

    if (!text) { text = ""; text_length = 0; }

    cmark_gfm_core_extensions_ensure_registered();
    parser = cmark_parser_new(options);
    if (!parser) return NULL;

    for (size_t i = 0; i < sizeof(extension_names) / sizeof(extension_names[0]); i++) {
        if (!attach_extension(parser, &render_extensions, mem, extension_names[i])) {
            cmark_llist_free(mem, render_extensions);
            cmark_parser_free(parser);
            return NULL;
        }
    }

    size_t body_length = 0;
    const char *body = skip_yaml_front_matter(text, text_length, &body_length);
    /* Keep source positions aligned with the editor after hiding metadata. */
    for (const char *cursor = text; cursor < body; cursor++) {
        if (*cursor == '\r') {
            cmark_parser_feed(parser, "\n", 1);
            if (cursor + 1 < body && cursor[1] == '\n') cursor++;
        } else if (*cursor == '\n') {
            cmark_parser_feed(parser, "\n", 1);
        }
    }
    cmark_parser_feed(parser, body, body_length);
    document = cmark_parser_finish(parser);
    if (!document) {
        cmark_llist_free(mem, render_extensions);
        cmark_parser_free(parser);
        return NULL;
    }

    /* CMARK_OPT_UNSAFE is deliberately absent: raw HTML and unsafe URLs stay inert. */
    html = cmark_render_html(document, options, render_extensions);
    cmark_node_free(document);
    cmark_llist_free(mem, render_extensions);
    cmark_parser_free(parser);
    return html;
}

char *markdown_to_html(const char *text) {
    return markdown_to_html_n(text, text ? strlen(text) : 0);
}
