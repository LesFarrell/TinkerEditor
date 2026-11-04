#ifndef TINKEREDIT_MARKDOWN_H
#define TINKEREDIT_MARKDOWN_H

#include <stddef.h>

/* Convert UTF-8 Markdown to a newly allocated UTF-8 HTML fragment. */
char *markdown_to_html(const char *text);
/* Length-aware form for callers that already know the buffer size. */
char *markdown_to_html_n(const char *text, size_t length);

/* Returns the Markdown body and its length, hiding a leading YAML front
   matter block ("---" ... "---" or "..."). Shared with markdown_export.c so
   Export as Text/RTF/.docx hide front matter the same way the preview and
   Export Rendered HTML/PDF already do. */
const char *skip_yaml_front_matter(const char *text, size_t text_length, size_t *body_length);

#endif
