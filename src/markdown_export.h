#ifndef MARKDOWN_EXPORT_H
#define MARKDOWN_EXPORT_H

/* Renders Markdown source to plain prose text (formatting markers stripped)
   or to RTF (Rich Text Format, readable by Word/WordPad). Both return a
   malloc'd, NUL-terminated buffer the caller frees with free(), or NULL on
   allocation failure - the same convention markdown_to_html() uses. */
char *markdown_to_plaintext(const char *text);
char *markdown_to_rtf(const char *text);

/* Renders Markdown source to a DOCX package's word/document.xml - the whole
   file, ready to be one entry of the .docx ZIP. Pairing it with the fixed
   parts docx_export_fixed_part() returns builds a complete, minimal .docx
   (see src/lua/ziptool.lua, used to do the actual ZIP packaging). */
char *markdown_to_docx_document_xml(const char *text);

typedef struct { const char *name, *content; } DocxFixedPart;
/* [Content_Types].xml and _rels/.rels - the two parts every .docx needs
   besides word/document.xml itself, and the same for every document since
   this export has no images or real hyperlink relationships to list. Not
   malloc'd - string literals, nothing to free. */
const DocxFixedPart *docx_export_fixed_parts(size_t *count);

#endif
