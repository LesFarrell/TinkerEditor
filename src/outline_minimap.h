#ifndef TINKEREDIT_OUTLINE_MINIMAP_H
#define TINKEREDIT_OUTLINE_MINIMAP_H

#include <windows.h>

#include "../third_party/scintilla/include/Scintilla.h" /* Sci_Position */

/* Upper bound on the number of ATX headings the outline pane will list; also
   sizes tinkeredit.c's own g_outlineLines table, which the module writes. */
#define OUTLINE_MAX_HEADINGS 1024

/* The outline/minimap module is a self-contained view over the active
   Scintilla document: the outline reads it directly, while the minimap keeps
   a throttled snapshot. Both windows are created by tinkeredit.c and their
   state is owned by the shell.
   Rather than reaching into tinkeredit.c's translation-unit statics by name,
   the shell hands in this table once at startup. Members that point into
   live shell state are followed on each use, so a later change to the
   underlying global is observed without re-initialising. */
typedef struct OutlineMinimapHost {
    /* Window handles the module drives. Recorded by address so they reflect
       the shell's current values (all are NULL until create_editor_views()
       / create_navigation_controls() run). */
    HWND *edit;        /* main editor view */
    HWND *outline;     /* outline listbox (NULL while the pane has never been created) */
    HWND *minimap;     /* minimap view */
    HWND *splitEdit;   /* secondary editor view */
    HWND *lastEditView; /* which editor view most recently had focus */

    /* Mutable shell state the module updates in place. */
    int *outlineCount;
    Sci_Position *outlineLines; /* OUTLINE_MAX_HEADINGS entries */
    int *activeLanguage;        /* Language of the active document; 0 is LANG_MARKDOWN */
    int *minimapDragging;
    int *minimapViewTop;    /* client-relative y (minimap coords) of the visible-region box */
    int *minimapViewHeight;
    int *splitEditorPos;    /* pixel position along the split axis, local to the editor region */

    /* Shell values that change over time, so read through a accessor. */
    int (*largeFile)(void);

    /* Shell helpers static to tinkeredit.c, passed in rather than duplicated. */
    HWND (*trackedEditView)(void);
    void (*syncSplitDocument)(void);
    void (*relayoutAndSave)(HWND hwnd);
    void (*layout)(HWND hwnd);
} OutlineMinimapHost;

/* Records the host table. Call once, before the first outline/minimap use
   (i.e. before create_editor_views() creates the minimap). */
void outline_minimap_init(const OutlineMinimapHost *host);

/* The level (1-6) of the ATX heading on `line`, or 0 if it isn't one. On
   success, *textStart and *textLength give the title without its markers. */
int outline_parse_atx_heading(const char *line, int length, int *textStart, int *textLength);

/* Tracks fenced code blocks (``` or ~~~) while a document is fed to
   outline_scan_line() one line at a time, so a "# comment" inside one isn't
   taken for a heading. Start from {0}. */
typedef struct OutlineScan {
    char fence;       /* the open fence's character, 0 outside a fence */
    int fenceLength;  /* its run length; a closing fence needs at least this */
} OutlineScan;

/* The heading level of the next line (given without its line ending), or 0
   when it isn't a heading or sits inside a fenced code block. */
int outline_scan_line(OutlineScan *scan, const char *line, int length, int *textStart, int *textLength);

/* Rebuilds the outline listbox from the active document's headings. */
void update_outline(void);

/* Flips the outline pane's visibility and relayouts. */
void toggle_outline(HWND hwnd);
/* View > Document Outline on Left: flips which edge the panel docks at. */
void toggle_outline_side(HWND hwnd);

/* One-time, document-independent setup for the minimap's Scintilla view. */
void configure_minimap(HWND hMinimap);

/* Recomputes the minimap's scroll position and its visible-region box. */
void update_minimap_viewport(void);

/* Coalesces snapshot updates while the active document is being edited. */
void defer_minimap_paint(void);
void refresh_minimap_after_edit(void);

/* Flips the minimap pane's visibility and relayouts. */
void toggle_minimap(HWND hwnd);

/* Shows/hides the secondary editor, splitting vertically when requested. */
void toggle_split_editor(HWND hwnd, int vertical);

/* Restores the split-editor divider to an even 50/50. */
void reset_split_editor(HWND hwnd);

/* Subclass procedure installed on the minimap view; see tinkeredit.c. */
LRESULT CALLBACK MinimapSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR id, DWORD_PTR data);

#endif
