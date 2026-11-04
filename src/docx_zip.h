#ifndef TINKEREDIT_DOCX_ZIP_H
#define TINKEREDIT_DOCX_ZIP_H

#include <stddef.h>

typedef struct { const char *name; const char *data; size_t length; } DocxZipPart;

/* Packages `parts` (word/document.xml, "[Content_Types].xml", "_rels/.rels",
   etc. - the caller supplies every part a valid .docx needs) into a ZIP byte
   string using src/lua/ziptool.lua, in a dedicated, fully-privileged Lua
   state separate from the sandboxed one plugins share: ziptool.lua needs
   real string/table/os library access plugins are deliberately denied (see
   plugin_host_internal.h). It never touches the real filesystem - a fake
   Lua file handle captures the bytes ziptool would otherwise write to disk,
   so this sidesteps both plugin sandboxing and the narrow-string/ANSI-
   codepage problems Lua's own io.open would hit on a Unicode Windows path;
   the caller writes *outBytes with its own Unicode-aware file I/O instead
   (write_bytes_file()).

   On success returns NULL and sets *outBytes/*outLength (caller frees
   *outBytes with free()); on failure returns a malloc'd error message
   (caller frees it) and leaves *outBytes NULL. */
char *docx_zip_build(const char *ziptoolSource, size_t ziptoolLength,
                     const DocxZipPart *parts, size_t partCount,
                     char **outBytes, size_t *outLength);

#endif
