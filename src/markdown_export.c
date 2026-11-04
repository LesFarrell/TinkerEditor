/* Renders parsed Markdown to plain prose text and to RTF, for File > Export.
   Both walk the same cmark-gfm tree markdown_to_html() parses, just with a
   different leaf renderer, so new Markdown constructs picked up by cmark-gfm
   degrade to "render the children" here instead of vanishing silently.

   Known, accepted limitations (not worth the extra code for an export
   convenience feature): GFM tables fall back to their flattened cell text
   since the table extension's cells are custom node types; list numbering
   in RTF/DOCX is literal "1. " text rather than a real list, since a real
   numbered list needs a separate list-definition table most readers don't
   require; images become a "[image: alt (url)]" placeholder rather than
   embedded data; DOCX links render the same way plain text does (link text
   plus the URL in parentheses) rather than as a clickable hyperlink, since a
   real one needs a part-level relationship file this export skips - RTF's
   own hyperlink field doesn't need that, so it gets a real one (see
   rtf_render_inline's CMARK_NODE_LINK case below). */
#include "markdown_export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmark-gfm.h"
#include "cmark-gfm-core-extensions.h"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} strbuf;

static void sb_append_n(strbuf *sb, const char *text, size_t n) {
    if (!text || !n) return;
    if (sb->len + n + 1 > sb->cap) {
        size_t newCap = sb->cap ? sb->cap * 2 : 256;
        while (newCap < sb->len + n + 1) newCap *= 2;
        char *data = (char *)realloc(sb->data, newCap);
        if (!data) return; /* leaves the buffer as it was; export just truncates */
        sb->data = data;
        sb->cap = newCap;
    }
    memcpy(sb->data + sb->len, text, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_append(strbuf *sb, const char *text) {
    if (text) sb_append_n(sb, text, strlen(text));
}

static void sb_append_char(strbuf *sb, char c) { sb_append_n(sb, &c, 1); }

static void sb_append_repeat(strbuf *sb, char c, int count) {
    for (int i = 0; i < count; i++) sb_append_char(sb, c);
}

/* Re-emits `text` with `prefix` in front of every line, for blockquotes and
   indented code blocks. A document with no trailing newline still gets its
   last line prefixed. */
static void sb_append_prefixed(strbuf *out, const char *text, size_t len, const char *prefix) {
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && text[i] != '\n') i++;
        sb_append(out, prefix);
        sb_append_n(out, text + start, i - start);
        sb_append_char(out, '\n');
        if (i < len) i++;
    }
}

static cmark_node *parse_markdown(const char *text, size_t length, cmark_llist **outExtensions) {
    static const char *const extension_names[] = {
        "table", "strikethrough", "autolink", "tagfilter", "tasklist"
    };
    const int options = CMARK_OPT_DEFAULT | CMARK_OPT_VALIDATE_UTF8 |
                        CMARK_OPT_FOOTNOTES | CMARK_OPT_STRIKETHROUGH_DOUBLE_TILDE;
    cmark_mem *mem = cmark_get_default_mem_allocator();
    cmark_llist *renderExtensions = NULL;
    *outExtensions = NULL;

    cmark_gfm_core_extensions_ensure_registered();
    cmark_parser *parser = cmark_parser_new(options);
    if (!parser) return NULL;
    for (size_t i = 0; i < sizeof(extension_names) / sizeof(extension_names[0]); i++) {
        cmark_syntax_extension *extension = cmark_find_syntax_extension(extension_names[i]);
        if (!extension || !cmark_parser_attach_syntax_extension(parser, extension)) {
            cmark_llist_free(mem, renderExtensions);
            cmark_parser_free(parser);
            return NULL;
        }
        renderExtensions = cmark_llist_append(mem, renderExtensions, extension);
    }
    cmark_parser_feed(parser, text ? text : "", length);
    cmark_node *document = cmark_parser_finish(parser);
    cmark_parser_free(parser);
    if (!document) {
        cmark_llist_free(mem, renderExtensions);
        return NULL;
    }
    *outExtensions = renderExtensions;
    return document;
}

/* ---- plain text ---------------------------------------------------- */

static void plain_render_inline_children(cmark_node *node, strbuf *out);

static void plain_render_inline(cmark_node *node, strbuf *out) {
    switch (cmark_node_get_type(node)) {
    case CMARK_NODE_TEXT:
    case CMARK_NODE_CODE: {
        const char *literal = cmark_node_get_literal(node);
        if (literal) sb_append(out, literal);
        break;
    }
    case CMARK_NODE_SOFTBREAK:
        sb_append_char(out, ' ');
        break;
    case CMARK_NODE_LINEBREAK:
        sb_append_char(out, '\n');
        break;
    case CMARK_NODE_LINK: {
        strbuf label = {0};
        plain_render_inline_children(node, &label);
        sb_append_n(out, label.data, label.len);
        const char *url = cmark_node_get_url(node);
        if (url && *url && strcmp(label.len ? label.data : "", url) != 0) {
            sb_append(out, " (");
            sb_append(out, url);
            sb_append_char(out, ')');
        }
        free(label.data);
        break;
    }
    case CMARK_NODE_IMAGE: {
        strbuf alt = {0};
        plain_render_inline_children(node, &alt);
        sb_append(out, "[image: ");
        sb_append_n(out, alt.data, alt.len);
        const char *url = cmark_node_get_url(node);
        if (url && *url) {
            sb_append(out, alt.len ? " (" : "(");
            sb_append(out, url);
            sb_append_char(out, ')');
        }
        sb_append_char(out, ']');
        free(alt.data);
        break;
    }
    case CMARK_NODE_HTML_INLINE:
    case CMARK_NODE_CUSTOM_INLINE:
        break;
    case CMARK_NODE_FOOTNOTE_REFERENCE:
        sb_append(out, "[note]");
        break;
    default:
        plain_render_inline_children(node, out);
        break;
    }
}

static void plain_render_inline_children(cmark_node *node, strbuf *out) {
    for (cmark_node *child = cmark_node_first_child(node); child; child = cmark_node_next(child))
        plain_render_inline(child, out);
}

static void plain_render_block_children(cmark_node *node, strbuf *out);

static void plain_render_block(cmark_node *node, strbuf *out) {
    switch (cmark_node_get_type(node)) {
    case CMARK_NODE_DOCUMENT:
    case CMARK_NODE_FOOTNOTE_DEFINITION:
        plain_render_block_children(node, out);
        break;
    case CMARK_NODE_PARAGRAPH:
        plain_render_inline_children(node, out);
        sb_append(out, "\n\n");
        break;
    case CMARK_NODE_HEADING: {
        strbuf heading = {0};
        plain_render_inline_children(node, &heading);
        sb_append_n(out, heading.data, heading.len);
        sb_append_char(out, '\n');
        if (cmark_node_get_heading_level(node) <= 2)
            sb_append_repeat(out, cmark_node_get_heading_level(node) == 1 ? '=' : '-',
                             heading.len > 0 ? (int)heading.len : 1);
        sb_append(out, "\n\n");
        free(heading.data);
        break;
    }
    case CMARK_NODE_BLOCK_QUOTE: {
        strbuf inner = {0};
        plain_render_block_children(node, &inner);
        while (inner.len > 0 && inner.data[inner.len - 1] == '\n') inner.len--;
        sb_append_prefixed(out, inner.data, inner.len, "> ");
        sb_append_char(out, '\n');
        free(inner.data);
        break;
    }
    case CMARK_NODE_LIST: {
        cmark_list_type listType = cmark_node_get_list_type(node);
        int number = cmark_node_get_list_start(node);
        if (number <= 0) number = 1;
        for (cmark_node *item = cmark_node_first_child(node); item; item = cmark_node_next(item)) {
            char marker[16];
            if (listType == CMARK_ORDERED_LIST) snprintf(marker, sizeof(marker), "%d. ", number++);
            else snprintf(marker, sizeof(marker), "- ");
            size_t indent = strlen(marker);
            char pad[16];
            memset(pad, ' ', indent);
            pad[indent] = '\0';

            strbuf inner = {0};
            for (cmark_node *c = cmark_node_first_child(item); c; c = cmark_node_next(c))
                plain_render_block(c, &inner);
            while (inner.len > 0 && inner.data[inner.len - 1] == '\n') inner.len--;

            if (inner.len == 0) {
                sb_append(out, marker);
                sb_append_char(out, '\n');
            } else {
                size_t startPos = out->len;
                sb_append_prefixed(out, inner.data, inner.len, pad);
                memcpy(out->data + startPos, marker, indent);
            }
            free(inner.data);
        }
        sb_append_char(out, '\n');
        break;
    }
    case CMARK_NODE_CODE_BLOCK: {
        const char *literal = cmark_node_get_literal(node);
        size_t len = literal ? strlen(literal) : 0;
        while (len > 0 && literal[len - 1] == '\n') len--;
        sb_append_prefixed(out, literal ? literal : "", len, "    ");
        sb_append_char(out, '\n');
        break;
    }
    case CMARK_NODE_THEMATIC_BREAK:
        sb_append(out, "----------------------------------------\n\n");
        break;
    case CMARK_NODE_HTML_BLOCK:
    case CMARK_NODE_CUSTOM_BLOCK:
        break;
    default:
        plain_render_block_children(node, out);
        break;
    }
}

static void plain_render_block_children(cmark_node *node, strbuf *out) {
    for (cmark_node *child = cmark_node_first_child(node); child; child = cmark_node_next(child))
        plain_render_block(child, out);
}

char *markdown_to_plaintext(const char *text) {
    cmark_llist *extensions = NULL;
    cmark_node *document = parse_markdown(text, text ? strlen(text) : 0, &extensions);
    if (!document) return _strdup("");

    strbuf out = {0};
    plain_render_block(document, &out);
    while (out.len > 0 && out.data[out.len - 1] == '\n') out.len--;
    sb_append_char(&out, '\n');

    cmark_node_free(document);
    cmark_llist_free(cmark_get_default_mem_allocator(), extensions);
    return out.data ? out.data : _strdup("");
}

/* ---- RTF ------------------------------------------------------------- */

/* Appends `text` RTF-escaped: backslash/braces get a backslash, bytes under
   0x80 pass through, and everything else is decoded from UTF-8 and emitted
   as \uN? (a signed UTF-16 code unit plus a one-character ANSI fallback),
   RTF's own Unicode escape - \uc1 in the document header matches the single
   '?' fallback byte used here. */
static void rtf_append_text(strbuf *out, const char *text, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\\' || c == '{' || c == '}') {
            sb_append_char(out, '\\');
            sb_append_char(out, (char)c);
            i++;
        } else if (c == '\n') {
            sb_append(out, "\\line ");
            i++;
        } else if (c < 0x80) {
            sb_append_char(out, (char)c);
            i++;
        } else {
            unsigned int codepoint = 0;
            int extra = 0;
            if ((c & 0xE0) == 0xC0) { codepoint = c & 0x1F; extra = 1; }
            else if ((c & 0xF0) == 0xE0) { codepoint = c & 0x0F; extra = 2; }
            else if ((c & 0xF8) == 0xF0) { codepoint = c & 0x07; extra = 3; }
            else { i++; continue; } /* stray continuation byte: drop it */
            if (i + (size_t)extra >= len) break;
            int ok = 1;
            for (int k = 1; k <= extra; k++) {
                unsigned char cont = (unsigned char)text[i + k];
                if ((cont & 0xC0) != 0x80) { ok = 0; break; }
                codepoint = (codepoint << 6) | (cont & 0x3F);
            }
            if (!ok) { i++; continue; }
            i += (size_t)extra + 1;
            char escape[24];
            if (codepoint <= 0xFFFF) {
                snprintf(escape, sizeof(escape), "\\u%d?", (short)codepoint);
                sb_append(out, escape);
            } else {
                unsigned int v = codepoint - 0x10000;
                short high = (short)(0xD800 + (v >> 10));
                short low = (short)(0xDC00 + (v & 0x3FF));
                snprintf(escape, sizeof(escape), "\\u%d?", high);
                sb_append(out, escape);
                snprintf(escape, sizeof(escape), "\\u%d?", low);
                sb_append(out, escape);
            }
        }
    }
}

static void rtf_append(strbuf *out, const char *text) {
    if (text) rtf_append_text(out, text, strlen(text));
}

static void rtf_render_inline_children(cmark_node *node, strbuf *out);

static void rtf_render_inline(cmark_node *node, strbuf *out) {
    switch (cmark_node_get_type(node)) {
    case CMARK_NODE_TEXT:
        rtf_append(out, cmark_node_get_literal(node));
        break;
    case CMARK_NODE_CODE:
        sb_append(out, "{\\f1 ");
        rtf_append(out, cmark_node_get_literal(node));
        sb_append_char(out, '}');
        break;
    case CMARK_NODE_SOFTBREAK:
        sb_append_char(out, ' ');
        break;
    case CMARK_NODE_LINEBREAK:
        sb_append(out, "\\line ");
        break;
    case CMARK_NODE_EMPH:
        sb_append(out, "{\\i1 ");
        rtf_render_inline_children(node, out);
        sb_append_char(out, '}');
        break;
    case CMARK_NODE_STRONG:
        sb_append(out, "{\\b1 ");
        rtf_render_inline_children(node, out);
        sb_append_char(out, '}');
        break;
    case CMARK_NODE_LINK: {
        const char *url = cmark_node_get_url(node);
        if (url && *url) {
            sb_append(out, "{\\field{\\*\\fldinst HYPERLINK \"");
            rtf_append(out, url);
            sb_append(out, "\"}{\\fldrslt {\\ul\\cf1 ");
            rtf_render_inline_children(node, out);
            sb_append(out, "}}}");
        } else {
            rtf_render_inline_children(node, out);
        }
        break;
    }
    case CMARK_NODE_IMAGE: {
        const char *url = cmark_node_get_url(node);
        sb_append(out, "[image: ");
        rtf_render_inline_children(node, out);
        if (url && *url) {
            sb_append(out, " (");
            rtf_append(out, url);
            sb_append_char(out, ')');
        }
        sb_append_char(out, ']');
        break;
    }
    case CMARK_NODE_HTML_INLINE:
    case CMARK_NODE_CUSTOM_INLINE:
        break;
    case CMARK_NODE_FOOTNOTE_REFERENCE:
        sb_append(out, "[note]");
        break;
    default:
        rtf_render_inline_children(node, out);
        break;
    }
}

static void rtf_render_inline_children(cmark_node *node, strbuf *out) {
    for (cmark_node *child = cmark_node_first_child(node); child; child = cmark_node_next(child))
        rtf_render_inline(child, out);
}

static void rtf_render_block_children(cmark_node *node, strbuf *out, int indentTwips);

/* `indentTwips` carries blockquote nesting into every \pard this emits, since
   RTF paragraph properties (like \li) don't inherit from an enclosing group. */
static void rtf_render_block(cmark_node *node, strbuf *out, int indentTwips) {
    char pard[48];
    switch (cmark_node_get_type(node)) {
    case CMARK_NODE_DOCUMENT:
    case CMARK_NODE_FOOTNOTE_DEFINITION:
        rtf_render_block_children(node, out, indentTwips);
        break;
    case CMARK_NODE_PARAGRAPH:
        snprintf(pard, sizeof(pard), "\\pard\\li%d\\sa160 ", indentTwips);
        sb_append(out, pard);
        rtf_render_inline_children(node, out);
        sb_append(out, "\\par\n");
        break;
    case CMARK_NODE_HEADING: {
        static const int sizes[] = {36, 32, 28, 26, 24, 22};
        int level = cmark_node_get_heading_level(node);
        int size = sizes[level >= 1 && level <= 6 ? level - 1 : 5];
        snprintf(pard, sizeof(pard), "\\pard\\li%d\\sb240\\sa120\\b1\\fs%d ", indentTwips, size);
        sb_append(out, pard);
        rtf_render_inline_children(node, out);
        sb_append(out, "\\par\n");
        break;
    }
    case CMARK_NODE_BLOCK_QUOTE:
        rtf_render_block_children(node, out, indentTwips + 720);
        break;
    case CMARK_NODE_LIST: {
        cmark_list_type listType = cmark_node_get_list_type(node);
        int number = cmark_node_get_list_start(node);
        if (number <= 0) number = 1;
        for (cmark_node *item = cmark_node_first_child(node); item; item = cmark_node_next(item)) {
            snprintf(pard, sizeof(pard), "\\pard\\li%d\\fi-240\\sa80 ", indentTwips + 360);
            sb_append(out, pard);
            if (listType == CMARK_ORDERED_LIST) {
                char marker[16];
                snprintf(marker, sizeof(marker), "%d.\\tab ", number++);
                sb_append(out, marker);
            } else {
                sb_append(out, "\\bullet\\tab ");
            }
            for (cmark_node *c = cmark_node_first_child(item); c; c = cmark_node_next(c)) {
                if (cmark_node_get_type(c) == CMARK_NODE_PARAGRAPH) rtf_render_inline_children(c, out);
                else rtf_render_block(c, out, indentTwips + 360);
            }
            sb_append(out, "\\par\n");
        }
        break;
    }
    case CMARK_NODE_CODE_BLOCK:
        snprintf(pard, sizeof(pard), "\\pard\\li%d\\sa160\\f1 ", indentTwips + 360);
        sb_append(out, pard);
        rtf_append(out, cmark_node_get_literal(node));
        sb_append(out, "\\f0\\par\n");
        break;
    case CMARK_NODE_THEMATIC_BREAK:
        sb_append(out, "\\pard\\brdrb\\brdrs\\brdrw10\\brsp20\\sa160\\par\n");
        break;
    case CMARK_NODE_HTML_BLOCK:
    case CMARK_NODE_CUSTOM_BLOCK:
        break;
    default:
        rtf_render_block_children(node, out, indentTwips);
        break;
    }
}

static void rtf_render_block_children(cmark_node *node, strbuf *out, int indentTwips) {
    for (cmark_node *child = cmark_node_first_child(node); child; child = cmark_node_next(child))
        rtf_render_block(child, out, indentTwips);
}

char *markdown_to_rtf(const char *text) {
    cmark_llist *extensions = NULL;
    cmark_node *document = parse_markdown(text, text ? strlen(text) : 0, &extensions);

    strbuf out = {0};
    sb_append(&out,
        "{\\rtf1\\ansi\\ansicpg1252\\deff0\\deflang1033\\uc1\n"
        "{\\fonttbl{\\f0\\fswiss Calibri;}{\\f1\\fmodern Consolas;}}\n"
        "{\\colortbl;\\red0\\green0\\blue255;}\n"
        "\\viewkind4\\f0\\fs22\n");
    if (document) rtf_render_block(document, &out, 0);
    sb_append(&out, "}");

    if (document) cmark_node_free(document);
    cmark_llist_free(cmark_get_default_mem_allocator(), extensions);
    return out.data ? out.data : _strdup("{\\rtf1}");
}

/* ---- DOCX (word/document.xml) ----------------------------------------- */

static void xml_append_escaped(strbuf *out, const char *text, size_t len) {
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        const char *entity = text[i] == '&' ? "&amp;" : text[i] == '<' ? "&lt;" :
                             text[i] == '>' ? "&gt;" : NULL;
        if (!entity) continue;
        sb_append_n(out, text + start, i - start);
        sb_append(out, entity);
        start = i + 1;
    }
    sb_append_n(out, text + start, len - start);
}

/* `size` is a heading's font size in half-points (0 for the document's own
   default), threaded through unchanged by every recursive call below so a
   heading's bold/italic spans keep its size too. */
static void docx_emit_run(strbuf *out, const char *text, size_t len, int bold, int italic,
                          int code, int size) {
    if (!len) return;
    sb_append(out, "<w:r>");
    if (bold || italic || code || size) {
        sb_append(out, "<w:rPr>");
        if (bold) sb_append(out, "<w:b/>");
        if (italic) sb_append(out, "<w:i/>");
        if (code) sb_append(out, "<w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\"/>");
        if (size) {
            char sz[48];
            snprintf(sz, sizeof(sz), "<w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/>", size, size);
            sb_append(out, sz);
        }
        sb_append(out, "</w:rPr>");
    }
    sb_append(out, "<w:t xml:space=\"preserve\">");
    xml_append_escaped(out, text, len);
    sb_append(out, "</w:t></w:r>");
}

static void docx_render_inline(cmark_node *node, strbuf *out, int bold, int italic, int size);

static void docx_render_inline_children(cmark_node *node, strbuf *out, int bold, int italic, int size) {
    for (cmark_node *child = cmark_node_first_child(node); child; child = cmark_node_next(child))
        docx_render_inline(child, out, bold, italic, size);
}

static void docx_render_inline(cmark_node *node, strbuf *out, int bold, int italic, int size) {
    switch (cmark_node_get_type(node)) {
    case CMARK_NODE_TEXT: {
        const char *literal = cmark_node_get_literal(node);
        docx_emit_run(out, literal, literal ? strlen(literal) : 0, bold, italic, 0, size);
        break;
    }
    case CMARK_NODE_CODE: {
        const char *literal = cmark_node_get_literal(node);
        docx_emit_run(out, literal, literal ? strlen(literal) : 0, bold, italic, 1, size);
        break;
    }
    case CMARK_NODE_SOFTBREAK:
        docx_emit_run(out, " ", 1, bold, italic, 0, size);
        break;
    case CMARK_NODE_LINEBREAK:
        sb_append(out, "<w:r><w:br/></w:r>");
        break;
    case CMARK_NODE_EMPH:
        docx_render_inline_children(node, out, bold, 1, size);
        break;
    case CMARK_NODE_STRONG:
        docx_render_inline_children(node, out, 1, italic, size);
        break;
    case CMARK_NODE_LINK: {
        docx_render_inline_children(node, out, bold, italic, size);
        const char *url = cmark_node_get_url(node);
        if (url && *url) {
            strbuf suffix = {0};
            sb_append(&suffix, " (");
            sb_append(&suffix, url);
            sb_append_char(&suffix, ')');
            docx_emit_run(out, suffix.data, suffix.len, bold, italic, 0, size);
            free(suffix.data);
        }
        break;
    }
    case CMARK_NODE_IMAGE: {
        docx_emit_run(out, "[image: ", 8, bold, italic, 0, size);
        docx_render_inline_children(node, out, bold, italic, size);
        const char *url = cmark_node_get_url(node);
        if (url && *url) {
            strbuf suffix = {0};
            sb_append(&suffix, " (");
            sb_append(&suffix, url);
            sb_append_char(&suffix, ')');
            docx_emit_run(out, suffix.data, suffix.len, bold, italic, 0, size);
            free(suffix.data);
        }
        docx_emit_run(out, "]", 1, bold, italic, 0, size);
        break;
    }
    case CMARK_NODE_HTML_INLINE:
    case CMARK_NODE_CUSTOM_INLINE:
        break;
    case CMARK_NODE_FOOTNOTE_REFERENCE:
        docx_emit_run(out, "[note]", 6, bold, italic, 0, size);
        break;
    default:
        docx_render_inline_children(node, out, bold, italic, size);
        break;
    }
}

/* A paragraph with the given left indent (twips, 1/1440 inch - 0 for none),
   its "<w:p>" through "</w:p>" entirely emitted here so callers just supply
   the inline content callback's worth of work inline. */
static void docx_open_paragraph(strbuf *out, int indentTwips) {
    sb_append(out, "<w:p>");
    if (indentTwips) {
        char ind[48];
        snprintf(ind, sizeof(ind), "<w:pPr><w:ind w:left=\"%d\"/></w:pPr>", indentTwips);
        sb_append(out, ind);
    }
}

static void docx_emit_code_block(strbuf *out, const char *literal, size_t len, int indentTwips) {
    docx_open_paragraph(out, indentTwips + 360);
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && literal[i] != '\n') i++;
        docx_emit_run(out, literal + start, i - start, 0, 0, 1, 0);
        if (i < len) { sb_append(out, "<w:r><w:br/></w:r>"); i++; }
    }
    sb_append(out, "</w:p>");
}

static void docx_render_block(cmark_node *node, strbuf *out, int indentTwips);

static void docx_render_block_children(cmark_node *node, strbuf *out, int indentTwips) {
    for (cmark_node *child = cmark_node_first_child(node); child; child = cmark_node_next(child))
        docx_render_block(child, out, indentTwips);
}

static void docx_render_block(cmark_node *node, strbuf *out, int indentTwips) {
    switch (cmark_node_get_type(node)) {
    case CMARK_NODE_DOCUMENT:
    case CMARK_NODE_FOOTNOTE_DEFINITION:
        docx_render_block_children(node, out, indentTwips);
        break;
    case CMARK_NODE_PARAGRAPH:
        docx_open_paragraph(out, indentTwips);
        docx_render_inline_children(node, out, 0, 0, 0);
        sb_append(out, "</w:p>");
        break;
    case CMARK_NODE_HEADING: {
        static const int sizes[] = { 36, 32, 28, 26, 24, 22 };
        int level = cmark_node_get_heading_level(node);
        int size = sizes[level >= 1 && level <= 6 ? level - 1 : 5];
        docx_open_paragraph(out, indentTwips);
        docx_render_inline_children(node, out, 1, 0, size);
        sb_append(out, "</w:p>");
        break;
    }
    case CMARK_NODE_BLOCK_QUOTE:
        docx_render_block_children(node, out, indentTwips + 720);
        break;
    case CMARK_NODE_LIST: {
        cmark_list_type listType = cmark_node_get_list_type(node);
        int number = cmark_node_get_list_start(node);
        if (number <= 0) number = 1;
        for (cmark_node *item = cmark_node_first_child(node); item; item = cmark_node_next(item)) {
            docx_open_paragraph(out, indentTwips + 360);
            char marker[16];
            if (listType == CMARK_ORDERED_LIST) snprintf(marker, sizeof(marker), "%d. ", number++);
            else snprintf(marker, sizeof(marker), "\xE2\x80\xA2 "); /* "• ", a bullet */
            docx_emit_run(out, marker, strlen(marker), 0, 0, 0, 0);
            cmark_node *first = cmark_node_first_child(item);
            if (first && cmark_node_get_type(first) == CMARK_NODE_PARAGRAPH)
                docx_render_inline_children(first, out, 0, 0, 0);
            sb_append(out, "</w:p>");
            for (cmark_node *c = first ? cmark_node_next(first) : NULL; c; c = cmark_node_next(c))
                docx_render_block(c, out, indentTwips + 360);
        }
        break;
    }
    case CMARK_NODE_CODE_BLOCK: {
        const char *literal = cmark_node_get_literal(node);
        size_t len = literal ? strlen(literal) : 0;
        while (len > 0 && literal[len - 1] == '\n') len--;
        docx_emit_code_block(out, literal ? literal : "", len, indentTwips);
        break;
    }
    case CMARK_NODE_THEMATIC_BREAK:
        sb_append(out, "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\" w:sz=\"6\" w:space=\"1\" "
                      "w:color=\"auto\"/></w:pBdr></w:pPr></w:p>");
        break;
    case CMARK_NODE_HTML_BLOCK:
    case CMARK_NODE_CUSTOM_BLOCK:
        break;
    default:
        docx_render_block_children(node, out, indentTwips);
        break;
    }
}

char *markdown_to_docx_document_xml(const char *text) {
    cmark_llist *extensions = NULL;
    cmark_node *document = parse_markdown(text, text ? strlen(text) : 0, &extensions);

    strbuf out = {0};
    sb_append(&out,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>");
    if (document) docx_render_block(document, &out, 0);
    sb_append(&out, "<w:sectPr/></w:body></w:document>");

    if (document) cmark_node_free(document);
    cmark_llist_free(cmark_get_default_mem_allocator(), extensions);
    return out.data ? out.data : _strdup("");
}

static const DocxFixedPart DOCX_FIXED_PARTS[] = {
    { "[Content_Types].xml",
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
      "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-"
      "officedocument.wordprocessingml.document.main+xml\"/>"
      "</Types>" },
    { "_rels/.rels",
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
      "<Relationship Id=\"rId1\" "
      "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
      "Target=\"word/document.xml\"/>"
      "</Relationships>" },
};

const DocxFixedPart *docx_export_fixed_parts(size_t *count) {
    *count = sizeof(DOCX_FIXED_PARTS) / sizeof(DOCX_FIXED_PARTS[0]);
    return DOCX_FIXED_PARTS;
}
