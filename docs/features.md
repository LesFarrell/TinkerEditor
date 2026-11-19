# Tinker Editor features

This is the detailed feature catalogue. For building, architecture and
security information, return to the [main README](../README.md).

- CommonMark plus GFM tables, task lists, strikethrough, bare-URL autolinks, and footnotes, along with code-fence language classes used by syntax-aware preview features.
- GitHub-style note, tip, important, warning, and caution alerts.
- Emoji shortcodes (`:tada:`, `:rocket:`, and similar) render as their Unicode emoji in the preview; unrecognised shortcodes are left as plain text.
- Wiki-style `[[Page Name]]` and `[[Page Name|label]]` links prefer the current document's folder, then resolve unique page names and workspace-relative paths across the active workspace. Typing after `[[` offers page-name completion; duplicate names are disambiguated when opened. A missing local page can still be created beside the current document. File → Go Back (Alt+Left) returns to wherever the jump was made from.
- Tools → Markdown → Check Workspace Links reports missing linked files, images, wiki pages, and heading anchors, plus ambiguous wiki-page names, in the clickable Problems panel. Find Backlinks lists every workspace document that links to the active Markdown file. Both commands include unsaved content from open tabs and skip fenced/inline code examples.
- Local syntax highlighting for named code fences in the preview, and for Markdown source in the editor, with matching light and dark themes.
- Editor syntax highlighting follows the file extension: Markdown, JSON, XML, HTML, YAML, C/C++/JavaScript/TypeScript, Python, Bash, PowerShell, Lua, CSS, SQL, INI-style configuration (`.ini`, `.cfg`, `.conf`, `.properties`, `.editorconfig`), TOML, Windows batch files (`.bat`, `.cmd`), makefiles (`Makefile`, `GNUmakefile`, `.mk`, `.mak`), and Inno Setup scripts (`.iss`). Unrecognised extensions fall back to Markdown.
- Python files can be run with Ctrl+F5 or debugged with F5 through the standard `pdb` debugger. At every stop, the output panel's Debug tab shows local variable names and values alongside Continue, Step Over, Step Into, and Step Out controls. The current execution line has a full-line highlight and gutter arrow. The same values are still printed in the console, where `vars` and `globals` remain available. Tinker Editor prefers an activated virtual environment, then a project-local `.venv`/`venv`, then Python on `PATH`.
- Single-file C and C++ programs (`.c`, `.cc`, `.cpp`, `.cxx`) can likewise be built and run with Ctrl+F5 or compiled with debug symbols and opened in GDB with F5. GDB itself stays hidden while the debugged program opens its own visible console for input and output. Editor breakpoints are loaded before execution; at every stop, the Debug tab shows function arguments and local values and highlights the current line. Use F10 to step over, F11 to step into, Shift+F11 to step out, and F6 to continue from the tab, editor, or Run menu; `print expression`, `watch expression`, and `vars` remain available in the console. Running can use GCC/G++, Clang, Zig, or MSVC (`cl.exe`, found via the `Compiler.VcvarsallPath` value in `settings.json`); debugging requires GCC/G++ or Clang and uses the bundled GDB 17.1. `Compiler.GdbPath` can select another GDB folder, and `PATH` remains a fallback if the bundled copy is absent. MSVC is Run-only. `Compiler.Backend` pins Run/Debug to one of GCC, Clang, Zig, or MSVC instead of the default auto-detect order.
- Lua files (`.lua`) can be run with Ctrl+F5 using `lua`, a versioned `lua5.x`, or `luajit` found on `PATH`. Without one, Tools → Interpreters → Lua → Run Document runs the file with Tinker Editor's built-in Lua 5.4, in the plugin sandbox. Lua is Run-only; there is no Lua debugger integration.
- Run sends a program's output to the output panel docked under the editor (View → Panels → Output Panel, Ctrl+\`), with its ANSI colours. A program waiting for input reads it from the line along the panel's bottom, Stop ends the program and everything it started, and double-clicking a file location in an error message (GCC, Clang, MSVC, Python, Lua or PowerShell style) opens the file at that line. Run → Run in Separate Console keeps the old behaviour of a console window that stays open. Long lines scroll sideways rather than wrap. Bundled plugins show their longer reports on the same tab: Grammar Checker → Check, Constraint Writer → Check Constraints, CSV → Inspect and Validate, Logs → CLEF → Inspect, Count Occurrences, and Document Personality's Analyse and Readability. C and C++ programs fill their output buffer before writing to the panel, so a prompt printed without `fflush(stdout)` appears only after the input it asks for; use the separate console for programs like that.
- The panel's Terminal tab (View → Panels → Terminal, Ctrl+Shift+\`) is a real terminal: Windows PowerShell in the workspace folder, on a Windows pseudoconsole (ConPTY), so full-screen console programs (vim, less, PSReadLine's live completion) work as they would in any other terminal. While the terminal has focus, its input takes priority over queued output and Tinker Editor's editor shortcuts: control characters go to the shell, Alt plus a printable key sends the usual Meta sequence (for example, Alt+F sends `Escape`, `f`), and modified navigation and function keys use xterm-compatible VT sequences. Input is written through an ordered background queue, so a console program that is busy or producing heavy output does not block keyboard handling. Ctrl+Shift+C copies a selection, Ctrl+Shift+V or Shift+Insert pastes (confirming first if the clipboard holds more than one line, since each is run as its own command), and the mouse wheel scrolls back through its bounded history. AltGr remains available for keyboard-layout characters. In-process Lua runs temporarily show their own terminal grid while preserving PowerShell in the background; their input supports cursor editing, history, paste, and Ctrl+C. Stop ends the shell and everything it started; pressing any key afterwards starts it again.
- The panel's Git tab (Ctrl+Shift+G) shows staged, unstaged, untracked, and conflicted files alongside local and remote branches. Checkboxes stage or unstage files; double-clicking a file opens a coloured staged or working-tree diff with moved-line highlighting, rename/submodule details and a compact change summary. Double-click any diff hunk or row to open its working-tree file at the corresponding new line. Double-clicking a remote branch creates its local tracking branch. The Git menu also provides repository status, separate staged and unstaged diffs, commit, publish/push/pull/fetch, branch creation/switching/deletion, stash creation/apply/pop/drop, merge and rebase continue/skip/abort controls, and explicitly confirmed discard commands. Pushing a branch without an upstream first offers to publish it to a selected remote. Git operations use the active workspace folder and require `git.exe` on `PATH`.
- The panel's Debug tab opens automatically when a debug session starts (F5), showing its status, variables, and Continue/Step Over/Step Into/Step Out controls; it stays selectable afterwards like Output and Terminal.
- F9 (or a click in the marker gutter) toggles per-file Python/C/C++ breakpoints. Breakpoints follow edited lines and are restored for that file in later Tinker Editor sessions; Run → Clear All Breakpoints removes them together.
- Multiple document tabs with independent undo history, selections, dirty state, encoding, and paths. Ctrl+E opens a searchable, most-recently-used tab switcher. Tabs have close buttons, support middle-click closing, can be reopened, and are restored between sessions.
- Ctrl+Shift+P opens a fuzzy command palette containing every built-in command and every Lua menu command. Entries use their current customized menu paths, show shortcuts, check marks and unavailable state, and plugin commands appear automatically after a reload.
- Tabs can be pinned from the View menu or their right-click menu. Pinned tabs show a pin instead of a close button, survive Close/Close Others/Close to Right/Close All commands, and retain their pinned state between sessions when they have a saved path.
- Tabs can be reordered by dragging. Dirty and unsaved tabs are snapshotted every five seconds to a recovery area under Local AppData (or `portable-data` in portable mode) and can be restored after an interrupted session.
- A formatting toolbar (Bold, Italic, Strikethrough, Inline code, Code block, Headings, Blockquote, Lists, Link, Image, Table, Footnote, Horizontal rule, Alert, Toggle preview, Swap panel sides), shown while the editor pane is visible.
- Smart Markdown editing continues blockquotes and bullet, numbered, and task lists on Enter (incrementing numbered lists and resetting task items), terminates empty items, and pairs brackets and inline backticks.
- Smart four-column tabs use real tabs for leading indentation, nest a list item from anywhere on its line, and use spaces only for alignment within ordinary text. Enter preserves the current indentation, adds a level after C-family/JSON braces, Python/YAML colons, and shell block openers, and expands paired braces or brackets onto aligned lines. Closing braces typed on a blank indented line move back one level automatically.
- Changed lines are marked by a thin bar beside the text: amber for a change not yet saved, green for one saved since the file was opened. A line edited and then undone back to how it was loses its bar. View → Mark Changed Lines turns the bars off; the colours (`lineChanged`, `lineSaved`) can be changed in `themes.lua`.
- Line editing commands: duplicate, delete, join, and move lines up or down, plus trimming trailing whitespace across the document in a single undo step.
- Toggle Comment wraps the selected lines in an HTML comment (Markdown has no line-comment syntax) and unwraps them again.
- Select All Occurrences (Ctrl+Shift+L) turns every match of the word under the caret, or of the current selection, into its own selection for multi-caret editing.
- Convert Line Endings (Edit → Convert → Line Endings) rewrites the document to CRLF, LF, or CR and switches the editor's save format to match.
- Format JSON and Minify JSON (Tools menu) operate on the current selection, the fenced code block at the caret, or the whole document. Input is validated first; malformed JSON, including trailing commas or missing separators, is left untouched and its error is highlighted.
- Word wrap (View → Word Wrap or Alt+Z), remembered between sessions.
- The editor can be split horizontally or vertically. View → Show Document in Split, or Show in Editor Split on a tab's context menu, retains any open ordinary text document in the secondary pane while other tabs are selected in the main pane. Choosing the current tab again makes the split follow later tab changes. Both documents remain editable, dirty/recovery state follows the document actually changed, Save and Save As follow the focused pane, the retained tab is marked with `↔`, and saved split selections are restored with the session. Image and large-file tabs are excluded.
- A live status bar reports the caret line/column, word and Unicode character counts, UTF-8/BOM state, line endings, and save/recovery state.
- Native Windows spell checking (View → Spell Check), remembered between sessions. Misspelled Markdown prose gets a themed squiggle; links, inline code, and fenced code are excluded. Right-click a marked word for corrections, Ignore Word, or Add to Dictionary. View → Spell Check Language selects any installed Windows dictionary.
- Pasting a bitmap or dropping an image copies it into an `images` folder beside the saved document and inserts a relative Markdown image link; unsaved documents are offered Save As first.
- Stable heading permalinks, a floating preview outline, and an optional docked source outline whose entries jump directly to their headings.
- Inline (`$...$`) and display (`$$...$$` or fenced `math`) mathematical expressions.
- YAML front matter delimited by `---` (and closed by `---` or `...`) is treated as document metadata and omitted from the preview.
- Mermaid diagram rendering with a zoom/pan toolbar.
- Copy buttons on rendered fenced code blocks.
- Export of the enhanced preview—including highlighted code, rendered math, and available rendered diagrams—to an HTML document.
- Printing and PDF export of the enhanced rendered preview through WebView2.
- The table editor resizes an existing GFM table while preserving its cells, or creates a new table with a chosen number of rows and columns.
- A persistent workspace sidebar remembers the chosen folder and provides a hierarchical file tree with Open, New, Rename, and Refresh actions. Open the `..` entry, choose “Open Parent Folder”, or press Backspace while the tree is focused to move the workspace up one directory. Moving up from a filesystem root shows available drives and mapped network shares. Renaming a file or folder updates matching inline and reference-style Markdown links across the workspace, including links in open unsaved buffers.
- Quick Open searches file names in the active workspace, while Find in Folder searches supported text files and opens results at the matching line. If no workspace is open, either command first asks for a folder. Generated/dependency folders are skipped.
- Lua plugins: bundled extensions load from `plugins` beside `tinkeredit.exe`, followed by personal extensions in `%APPDATA%\tinkeredit\plugins` (or `portable-data\plugins` in portable mode). **Plugins → New Plugin**, **Open Plugins Folder**, **Manage Plugins**, and **Reload Plugins** cover the full edit/test/enable workflow without administrator rights. A plugin may be one `.lua` file or a folder containing `init.lua` and private modules. Commands can have keyboard shortcuts, check marks, persistent settings, and enabled predicates that grey them when their editor context is unsuitable. Edit and selection events are debounced before Lua runs, so live JSON, grammar, Markdown lint, and constraint checks do not rescan on every key message. The plugin guide, `plugins/README.md` (also Help → Plugin Guide), documents the API, and `plugins/example.lua` is a working example.
- CSV tools under **Tools → CSV** parse comma-, semicolon-, and tab-separated data with quoted fields. Commands inspect, normalize, visually align columns, convert, sort, filter, deduplicate, transpose, edit columns, and export to JSON or Markdown tables.
- Additional bundled plugins provide CLEF-aware log inspection and filtering, SQL formatting, natural line sorting, integer-base, Roman-numeral and Unix timestamp/ISO 8601 date conversion, aligning lines on a delimiter (`plugins/line-tools.lua`), a spelling report that lists every misspelled word and selects all its occurrences (`plugins/spelling-report.lua`), JSON Lines/query-string/key-value conversion, Base64/hex/URL/HTML-entity encoding and CRC-32/16/NMEA checksums (`plugins/encoding.lua`), JSON syntax checking with live highlighting and JSON-path copying (`plugins/json-check.lua`), an offline heuristic grammar checker whose rules can each be switched off, with English-only rules skipped for other spelling languages, and whose report lists every issue as a location to double-click (`plugins/grammar-checker.lua`), conservative Lua reindentation of the document or the selected lines (**Tools → Interpreters → Lua → Format Document** or **Format Selection**, described in `plugins/lua-format/README.md`), following the end of a file as another program writes to it, like `tail -f`, for watching logs (**View → Follow EOF**, in `plugins/tail.lua`: the file reloads without asking and moves to its last line), running Lua with Tinker Editor's own built-in Lua, with no installation needed (**Tools → Interpreters → Lua → Run Document**, **Run Selection** and **Evaluate Selection**, in `plugins/lua-runner.lua`; output, errors and `io.read()` input all use the Terminal tab, and scripts run in the plugin sandbox, so they can use `editor` but have no file access), and assemblers for the Z80 (ZX Spectrum TAP, including **Compile and Run** through the `.tap` file association), CHIP-8/SUPER-CHIP, 6502 (Commodore 64 PRG) and Game Boy SM83 (`.gb` ROM) described in `plugins/assemblers/README.md`.
- The whole menu bar is defined in Lua. Plugins → Customize Menus copies the default definition to `%APPDATA%\tinkeredit\menus.lua` (or `portable-data\menus.lua` in portable mode) and opens it; items there can be reordered, renamed, removed, or point at Lua functions, and Reload Plugins applies the changes. A definition with a mistake is reported and the default menus are used instead.
- Markdown formatting and its Markdown-aware Edit commands are written in focused Lua files under `src/lua/` and built into `tinkeredit.exe`. HTML snippets come from `plugins/html-tools.lua`; Markdown structure/link tools and linting come from `plugins/markdown-tools.lua` and `plugins/markdown-linter.lua`; document and selection counts come from `plugins/selection-statistics.lua`; and the Edit → Convert submenu is supplied entirely by `plugins/edit-convert.lua`. JSON syntax checking and error highlighting come from `plugins/json-check.lua`, and formatting, key sorting, string escaping, and JSON/YAML conversion from `plugins/json-tools.lua`. Bundled plugins are ordinary files in `plugins/`, so their commands and menus can be changed without duplicating native command implementations.
- Detection of files changed by another program, with an offer to reload the active document and an indicator on changed background tabs.
- Synchronized scrolling between the Markdown editor and rendered preview.
- Recently opened files (File → Recent Files), persisted across runs and pruned automatically if a file no longer exists.
- Insert File at Cursor (File menu) reads a chosen file's contents and inserts them at the caret, replacing any selection.
- Editor font is configurable (View → Font) and remembered between sessions.
- Editor zoom (View → Zoom In/Out/Reset or Ctrl+=/Ctrl+-/Ctrl+0) scales the editor text independently of the configured font size and is remembered between sessions.
- Open Terminal Here (File menu or toolbar) opens Windows Terminal, falling back to Command Prompt, in the current file's folder. Show in Explorer (File menu) opens Explorer with the current file pre-selected.
- An optional UTF-8 custom preview stylesheet (View → Markdown Theme → Custom Preview CSS), embedded into rendered HTML exports and remembered between sessions.
- Focus mode (View → Focus Mode or Ctrl+Shift+F11) hides the menu, tabs, and formatting toolbar and is remembered between sessions.
- Window position/size/maximized state, editor/preview order, workspace visibility, active workspace folder, and splitter position are remembered between sessions; View → Preview on Left controls the editor/preview order.
- Creating an empty `portable.mode` file beside `tinkeredit.exe` redirects settings, personal plugins and menu/theme customizations, session/recent state, recovery snapshots, and the WebView2 profile into `portable-data` beside the executable. Removing the marker returns to the normal per-user locations without deleting either set of state.
- Find and Replace (Ctrl+F, Ctrl+H) in a window that stays open while you edit: Find Next and Find Previous (F3, Shift+F3, also from the editor), match case, whole word, and regular expressions (ECMAScript syntax; `\1` in the replacement inserts a group). Count shows how many matches there are, and Replace All replaces them in one undo step; with In selection on, both work only within the selected text. The window's status line says when a search wraps round the document, finds nothing, or has an invalid regular expression. The search and replacement text and the options are remembered between sessions.
- A Markdown syntax reference (Help → Markdown Reference), opened as a live Markdown document so the preview renders every construct it documents. Help → Keyboard Shortcuts opens the shortcut list the same way, and Help → About Tinker Editor shows the version.
- Tools → Settings gathers appearance, font, word wrap, preview, editor, and compiler options in one dialog.
- Unicode filenames, long Windows paths, drag-and-drop, and document-relative links/images.
- Text encodings: UTF-8 (with or without a BOM), UTF-16 LE/BE (with or without a BOM), and the system ANSI code page are detected on open and kept on save; the status bar shows which one a document uses. File → Reopen with Encoding rereads a file whose encoding was guessed wrong, and Edit → Convert → Encoding changes the one it's saved in. Saving warns first if the document has characters the chosen encoding can't store (anything outside the ANSI code page, or bytes that aren't valid text when saving UTF-16). Insert File, Find in Folder, and Replace in Folder decode files the same way; Replace in Folder writes each file back in its own encoding and leaves alone any file it can't store the replacement in.
- Failure-safe saves: content is flushed to a temporary file before atomically replacing the destination.
- Large files, including ones past 2 GB, open in large-file mode once they reach `View.LargeFileThresholdMB` in `settings.json` (64 MB by default); a smaller open document that grows past the threshold switches modes too. Large opens and saves show cancellable progress. The file is shown as plain text with word wrap off; the preview, outline, word count, word completion, brace matching, smart Enter/Tab, and autosave recovery are switched off for that document so editing stays responsive. Find/Replace, Go to Line, bookmarks, and saving work as normal.
- Preview hardening for untrusted documents: raw HTML and dangerous URLs are disabled, Mermaid runs in strict mode, and the shell uses a restrictive content-security policy.

### Keyboard shortcuts

These application shortcuts apply when the editor and other application controls have focus. The Terminal tab captures its own keyboard input instead, except for its Ctrl+Shift+C copy and Ctrl+Shift+V/Shift+Insert paste commands described above.

| Shortcut | Action |
|---|---|
| Ctrl+N | New |
| Ctrl+O | Open |
| Ctrl+K | Quick Open in Folder |
| Ctrl+S | Save |
| Ctrl+Shift+S | Save As |
| Ctrl+Alt+S | Save All |
| Ctrl+P | Print rendered preview |
| Ctrl+W | Close current tab |
| Ctrl+Shift+T | Reopen last closed tab |
| Ctrl+E | Search open tabs |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Alt+Left | Go back to where the last wiki-link jump was made from |
| Ctrl+Z / Ctrl+Y | Undo / Redo |
| Ctrl+X / Ctrl+C / Ctrl+V | Cut / Copy / Paste |
| Ctrl+A | Select All |
| Tab / Shift+Tab | Smart indent / outdent |
| Ctrl+D | Duplicate line |
| Ctrl+Shift+K | Delete line |
| Ctrl+J | Join lines |
| Alt+Up / Alt+Down | Move line or selection up / down |
| Ctrl+/ | Toggle comment |
| Ctrl+Shift+D | Select next occurrence |
| Ctrl+Shift+L | Select all occurrences |
| Ctrl+] | Jump to matching brace |
| Ctrl+F | Find |
| F3 / Shift+F3 | Find Next / Find Previous |
| Ctrl+H | Replace |
| Ctrl+G | Go to Line |
| Ctrl+F2 | Toggle bookmark |
| F2 / Shift+F2 | Next / previous bookmark |
| Ctrl+Shift+V | Toggle preview pane |
| Ctrl+Shift+E | Toggle editor pane |
| Ctrl+Shift+X | Toggle file explorer |
| Ctrl+` | Toggle the output panel |
| Ctrl+Shift+` | Open the terminal |
| Ctrl+\\ | Toggle horizontal editor split |
| Ctrl+F6 / Ctrl+Shift+F6 | Cycle panes forward / back |
| Ctrl+Shift+F11 | Toggle focus mode |
| Alt+Z | Toggle word wrap |
| Ctrl+= / Ctrl+- | Zoom in / out |
| Ctrl+0 | Reset zoom |
| Ctrl+F5 | Run active Python, C, C++, or Lua file |
| F5 | Debug active Python, C, or C++ file |
| F6 | Continue the active debug session |
| F10 | Step over the current line |
| F11 | Step into the current call |
| Shift+F11 | Step out of the current call |
| F9 | Toggle debugger breakpoint |
