# Writing Tinker Editor plugins

A Tinker Editor plugin is a Lua script that adds commands, menus, and automatic actions to the editor. Plugins can read and change the document, ask simple questions, and show messages. They can't access arbitrary files, start programs, or reach the network, with two narrow, explicit exceptions - see [`sqlite`](#sqlite) and [`fs`](#fs) below. A folder extension can load its own Lua modules through the restricted `require` function.

This guide covers everything a plugin can do. [`example.lua`](example.lua), in this folder, is a working plugin that uses most of it. The `plugins` folder also contains ready-to-use data conversion, JSON, CSV, SQL, CLEF log, number conversion, natural sorting, grammar, spelling report, readability, anagram, Lua runner, tail (follow a log file), Markdown structure/link/lint tools, HTML snippets, selection statistics, text, calculator, assembler, Lua formatting, SQLite-backed notes/JSON/YAML stores, a SQLite browser, a wikilink/backlinks notebook, and developer-oriented extensions.

Paths below use `%APPDATA%\tinkeredit`; in portable mode use `portable-data` beside `tinkeredit.exe` instead.

**Contents:** [Quick start](#quick-start) · [How plugins are loaded](#how-plugins-are-loaded) · [Positions, ranges, and lines](#positions-ranges-and-lines) · [API reference](#api-reference) · [Where commands appear](#where-commands-appear) · [Replacing built-in commands](#replacing-built-in-commands) · [Customizing the menu bar](#customizing-the-menu-bar) · [How commands run](#how-commands-run) · [The Lua environment](#the-lua-environment) · [Limits](#limits) · [Recipes](#recipes) · [Troubleshooting](#troubleshooting)

## Quick start

1. Choose **Plugins → New Plugin**, enter `hello.lua`, and edit the file Tinker Editor creates in `%APPDATA%\tinkeredit\plugins`:

   ```lua
   menu.add("Say Hello", function()
     ui.message("Hello from " .. (editor.file_path() or "an unsaved document"))
   end, "Ctrl+Alt+H")

   menu.add("Text/Shout", function()
     editor.replace_selection(editor.get_selection():upper() .. "!")
   end)
   ```

2. Choose **Plugins → Reload Plugins** (or restart Tinker Editor).

3. **Plugins → Say Hello** and **Plugins → Text → Shout** are now in the menu.

Use **Plugins → Open Plugins Folder** whenever you need the folder in Explorer. Lua Language Server metadata is created there automatically, so compatible editors provide completion and API documentation.

## How plugins are loaded

- Tinker Editor loads bundled extensions from `plugins` beside `tinkeredit.exe`, then personal extensions from `%APPDATA%\tinkeredit\plugins`. Within each location, entries load alphabetically (ignoring case), so a personal extension can replace a bundled command.
- An extension is either a single `.lua` file or a folder containing `init.lua`. A folder extension can split code into private modules and load them at startup or later from a callback with `require("module")`; dots address subfolders, as in `require("text.wrap")`. Paths outside that extension folder are rejected.
- **Plugins → Manage Plugins** enables or disables discovered extensions. The state is stored in `settings.json`; disabled extensions remain visible in the manager and do not run.
- Loading a plugin runs its top-level code once. That code normally registers commands and event handlers; the functions it registers run later, when they're used.
- **Plugins → Reload Plugins** throws away everything loaded (commands, handlers, and all Lua variables) and loads every plugin again. It also reloads the menu definition, so changes to plugins and menus take effect without restarting.
- If a plugin's top-level code fails, Tinker Editor lists the error with its file name and line and offers to open the first failing source. A plugin that fails part-way adds none of its commands or handlers. The other plugins still load.
- Plugin files must be UTF-8 text (with or without a byte-order mark) and at most 1 MiB.
- Every plugin has a private global environment, private copies of library/API tables, and a private module cache. Globals in one extension cannot overwrite another extension's globals. Modules belonging to the same folder extension share its environment.

## Positions, ranges, and lines

The `editor` functions describe places in the document the way Lua's `string.sub` does:

- **A position** is a 1-based byte offset into the document's UTF-8 text: position 1 is the first byte, and the position just past the end is `editor.length() + 1`.
- **A range** is two positions, `start, finish`, that include both ends, so `editor.get_text():sub(start, finish)` is the range's text. A range with `finish == start - 1` is empty: it marks a place between two bytes. For example, `editor.get_selection_range()` returns `5, 4` when nothing is selected and the caret is before the 5th byte.
- **Lines** are numbered from 1. A line's text and range never include its line ending; `editor.eol()` gives the document's line ending (`"\r\n"`, `"\n"`, or `"\r"`), which is what to use when inserting new lines.
- Positions count bytes, not characters. A character such as `é` is two bytes, so don't split text in the middle of one. Use Lua's `utf8` library to work in characters.

Functions that take a position, range, or line number raise an error when it's outside the document.

## API reference

Plugins see the `menu`, `contextmenu`, `commands`, `events`, `editor`, `documents`, `ui`, `config`, `app`, `spell`, `output`, `zip`, and (where available) `sqlite` tables. Square brackets mark optional arguments.

Names, labels, menu paths, and dialog text must be valid UTF-8 and cannot contain NUL bytes. Document text passed through `editor.*` is length-counted separately, so those functions preserve every byte in the document.

### menu

#### `menu.add(label, fn [, shortcut [, enabled]])`

Adds a command. Choosing it runs `fn()`. `label` decides where it appears; see [Where commands appear](#where-commands-appear). The same label can be added more than once. Each becomes its own item. `shortcut` may combine Ctrl and/or Alt, optional Shift, and a letter, digit, F1–F24, Tab, Space, Insert, or Delete, for example `"Ctrl+Alt+M"`. Built-in shortcuts win if two commands use the same keys.

When `enabled` is supplied, Tinker Editor evaluates it while refreshing menu state and greys out the command when it returns `false` or `nil`. The editor API is available. Pass `nil` for `shortcut` when only an enabled predicate is needed:

```lua
menu.add("Text/Uppercase Selection", uppercase, nil, function()
  return editor.get_selection() ~= ""
end)
```

#### `menu.checkbox(label, key, default, fn [, shortcut])`

Adds a command with a check mark. `key` identifies its saved state inside this plugin, `default` is a boolean used until a state has been saved, and choosing the item toggles it before running `fn(checked)` with the new boolean value. If the function fails, Tinker Editor restores the previous state. The optional shortcut uses the same format as `menu.add`.

```lua
menu.checkbox("Text/Trim Whitespace on Save", "trim_on_save", false, function(checked)
  ui.message(checked and "Trim on save is on." or "Trim on save is off.")
end)
```

The state persists across restarts and plugin reloads. Read it elsewhere in the plugin with `config.get(key, default)`.

#### `menu.separator([path])`

Adds a separator line after whatever has been added to a menu so far, so the next item added there starts a new group. `path` names the menu the same way a label's leading parts do, but with no item name at the end:

| Path | The separator goes in |
|---|---|
| none, or `""` | The Plugins menu |
| `"Lines"` | Plugins → Lines |
| `"/Snippets"` | The **Snippets** menu itself |
| `"/Snippets/Markdown"` | Snippets → Markdown |

```lua
menu.add("Lines/Sort", sort_lines)
menu.add("Lines/Reverse", reverse_lines)
menu.separator("Lines")
menu.add("Lines/Number Lines", number_lines)
```

Separators follow the same rules as `"-"` in the menu definition: one at the start or end of a menu, or next to another separator, is left out. A separator doesn't create a menu, so add at least one item to a menu before separating items in it. Each separator counts towards the [command limit](#limits).

### contextmenu

#### `contextmenu.add(label, fn [, enabled])`

Adds an item to the editor's right-click menu and returns its numeric handle. Choosing the item runs `fn()` with the editor APIs acting on the pane that was clicked. Items appear after Tinker Editor's standard editing and spelling commands. Slash-separated label parts create or reuse submenus, up to eight levels deep; for example, `"Formatting/Inline/Code"` creates **Formatting → Inline → Code**.

When the optional `enabled` function is supplied, Tinker Editor evaluates it each time the menu opens and greys out the item when it returns `false` or `nil`. The editor API is available, so `function() return editor.get_selection() ~= "" end` enables a command only while text is selected.

#### `contextmenu.remove(item)`

Removes an item previously added by this plugin. It returns `true` when the item was removed and `false` when the handle is no longer registered. A plugin cannot remove another plugin's items.

#### `contextmenu.checkbox(label, key, default, fn [, enabled])`

Adds a checkable right-click item and returns its numeric handle. Its state is stored in this plugin's settings under `key`, survives restarts and plugin reloads, and begins at the boolean `default` until it has been changed. Choosing it toggles the state before calling `fn(checked)`; if the callback fails, Tinker Editor restores the previous state. Slash-separated labels and the optional `enabled` function work as they do for `contextmenu.add`.

```lua
local item

local function add_item()
  item = contextmenu.add("Formatting/Inline/Wrap Selection in Backticks", function()
    editor.replace_selection("`" .. editor.get_selection() .. "`")
  end, function()
    return editor.get_selection() ~= ""
  end)
end

add_item()

contextmenu.checkbox("Formatting/Inline/Use Triple Backticks", "triple_backticks", false,
  function(checked)
    ui.status(checked and "Triple backticks enabled" or "Triple backticks disabled")
  end)

menu.add("Toggle Backticks Context Item", function()
  if item then
    contextmenu.remove(item)
    item = nil
  else
    add_item()
  end
end)
```

### commands

#### `commands.register(name, fn [, title])`

Makes the built-in command called `name` run `fn()` instead of its usual code, wherever it's used from: menu, toolbar, or keyboard shortcut. `title` is used as the title of dialogs `fn` shows (default `"Tinker Editor"`). If several plugins register the same name, the last one loaded wins. See [Replacing built-in commands](#replacing-built-in-commands).

`commands.execute(name)` queues a named built-in command to run after the current plugin callback finishes and returns whether the name exists. The `run.*` commands (run, debug, step, breakpoints) can't be executed from a plugin and raise an error.

### events

#### `events.on(name, fn)`

Runs `fn(path)` each time the event happens, where `path` is the document's file path, or `nil` if it has never been saved. It returns a subscription number; `events.off(subscription)` removes it. Handlers run in registration order.

| Event | When it happens | `path` |
|---|---|---|
| `"save"` | Just before the document is written to disk, so anything the handler changes is saved. | Where it's being saved, including a new name chosen with Save As. |
| `"open"` | After a file is loaded into the editor, including when a file changed by another program is reloaded. | The loaded file. |
| `"reload"` | After `"open"`, when the file loaded was the document's own, reloaded because another program changed it. The selection and scroll position have been put back by then, so a handler can move them. | The reloaded file. |
| `"tab_changed"` | After a different document becomes the active one: switching tabs, opening a new tab, or closing one. Brief switches Tinker Editor makes itself, such as Save All visiting each tab, don't count. | The newly active document. |
| `"before_save"` / `"after_save"` | Immediately before or after a successful write. `"save"` remains the compatibility alias for `"before_save"`. | The save destination. |
| `"close"` | Before a tab asks to save/discard and closes. | The document being closed. |
| `"text_changed"` / `"selection_changed"` | After a short host-side debounce following an edit or selection move. Expensive live analysis can run here directly; do not add another debounce unless it also combines other event sources. | The active document. |
| `"workspace_changed"` / `"theme_changed"` | After the workspace or editor theme changes. | The active document (workspace handlers can call `app.workspace_path()`). |
| `"shutdown"` | As the main window shuts down. | The final active document. |

Handlers work on the active document, exactly as commands do. A handler that fails is reported, but doesn't stop what caused the event: a document is still saved if its `save` handler fails.

### config

Settings are private to each plugin and persist in `%APPDATA%\tinkeredit\settings.json` alongside application preferences. Keys are 1 to 64 characters and may contain letters, digits, `.`, `_`, and `-`.

| Function | Does |
|---|---|
| `config.get(key [, default])` | Returns the saved value. If none exists, returns `default`, or `nil`. A boolean or number default converts the stored text to that type. |
| `config.set(key, value)` | Saves a string, number, boolean, nested table, or array. Passing `nil` removes the value. A table default passed to `get` restores structured data. |

### app

`app.date()`, `app.time()`, and `app.date_time()` return the local time. `app.version()`, `app.api_version()`, `app.platform()`, `app.memory_used()`, and `app.workspace_path()` expose safe runtime metadata. `app.traceback([message])` builds a Lua stack traceback for protected error handlers. `app.capabilities()` returns named feature flags; API 3 adds `eol_conversion`, `save_encoding`, `tab_width`, `spelling` (the [`spell`](#spell) table) and `output_panel` (the [`output`](#output) table), API 4 adds `context_menu`, API 5 adds the [`problems`](#problems) diagnostics table, API 6 adds `save_and_open`, API 7 adds `configured_runner`, API 8 adds `menu_enabled`, API 9 adds the [`terminal`](#terminal) table and `terminal_panel`, API 10 adds panel-wide clearing and `app.traceback()`, and API 11 adds `relative_file_read` and `editor.read_relative_file()`. `app.defer(milliseconds, fn)` schedules one UI-thread callback (up to 60 seconds) and returns a token accepted by `app.cancel(token)`; it is intended for debouncing and incremental work rather than blocking background computation.

### documents

`documents.count()` and `documents.active()` describe the tabs. `documents.activate(index)`, `new()`, `open(path)`, `save([save_as])`, and `close([index])` manage them. `documents.open(path)` asks the user before opening a file that isn't already in a tab, and does nothing if they decline. `documents.path([index])`, `modified([index])`, and `encoding()` return document metadata. `documents.auto_reload([index] [, on])` returns whether a document reloads changes made on disk by another program without asking, and sets it when `on` is given (a boolean); Tinker Editor still asks when the document has unsaved edits. It lasts until the tab closes. Passing `"utf-8"`, `"utf-8-bom"`, `"utf-16le"`, `"utf-16be"`, or `"ansi"` to `documents.encoding(value)` changes how the active document will be saved and marks it modified. Save-encoding changes are document metadata and are not reverted by text Undo. Indices are 1-based. File pickers remain user-controlled through `ui.pick_file()` and `ui.pick_folder()`.

### output

These write to the Output tab of the output panel docked under the editor (View → Output Panel), where Run shows a program's output.

| Function | Does |
|---|---|
| `output.write(text, ...)` | Appends each string. Text isn't followed by a line break unless it ends with one, and ANSI colour sequences such as `"[32m"` colour it. The panel isn't opened. |
| `output.clear()` | Empties the Output tab. |
| `output.show()` | Opens the panel on the Output tab, without moving the keyboard focus. |
| `output.report(title, text)` | Appends `text` under a coloured heading naming the report and the document ("Grammar Checker - notes.md"), followed by a blank line, and opens the panel. Tinker Editor's bundled plugins show their longer reports this way. |
| `output.hide()` | Closes the panel, whichever tab it is showing. Programs running in it keep running. |
| `output.visible()` | Returns `true` while the panel is open. |

```lua
menu.add("Report TODOs", function()
  output.clear()
  for n = 1, editor.line_count() do
    if editor.get_line(n):find("TODO", 1, true) then output.write(("line %d: %s
"):format(n, editor.get_line(n))) end
  end
  output.show()
end)
```

### terminal

These run a bounded in-process interactive session on the Terminal tab. `terminal.begin()` preserves the current PowerShell session in the background and opens a clear Lua-facing grid. Output accepts ANSI colour sequences. `terminal.read()` focuses the terminal and returns one echoed line; Escape or Stop returns `nil`, while Ctrl+C interrupts the Lua command. `terminal.finish()` leaves the transcript visible, and the next key returns to the preserved shell. The bundled Lua runner uses this API for `print`, `io.write`, errors and `io.read`.

| Function | Does |
|---|---|
| `terminal.begin()` | Opens and clears the Terminal tab for an in-process session. |
| `terminal.write(text, ...)` | Writes UTF-8/ANSI text to the terminal grid. |
| `terminal.read([prompt])` | Optionally writes `prompt`, then reads and echoes one line. Escape or Stop returns `nil`. |
| `terminal.clear()` | Clears the active Terminal screen and drops its scrollback. |
| `terminal.finish()` | Ends the in-process session and leaves its transcript visible. |

### problems

These publish location-aware diagnostics on the Problems tab. A report uses the usual `path:line: message` text format.

| Function | Does |
|---|---|
| `problems.report(source, text)` | Replaces one named producer's diagnostics. Empty text removes that producer. |
| `problems.clear(source)` | Removes one named producer's diagnostics. |
| `problems.clear_all()` | Clears the entire Problems tab, including reports from other producers. |

### spell

These check words against the Windows spelling dictionary chosen in **View → Spell Check Language**, whether or not spell checking is turned on. Words added to the dictionary count; words ignored with **Ignore Word** don't.

| Function | Does |
|---|---|
| `spell.language()` | Returns the dictionary's locale name, such as `"en-GB"`, or `nil` when Windows has no spelling dictionary. The dictionary is opened just after the window appears, so call this from a command rather than a plugin's top-level code. |
| `spell.check(word)` | Returns `true` when the dictionary accepts `word`. Text containing several words is accepted only if all of them are. A repeated word isn't a spelling mistake. |
| `spell.check(words)` | Checks a table of up to 50,000 strings at once and returns a table with one boolean for each. Checking a batch is much faster than checking the words one at a time. |

Both `spell.check` forms raise an error when there's no dictionary. Checking takes about a tenth of a millisecond for each rejected word, spread over the processor's cores, and counts towards the [time limit](#how-commands-run), so check large searches in batches of a few thousand. [`anagrams/init.lua`](anagrams/init.lua) is an example.

### editor

These work only while a command or event handler is running. Calling them from a plugin's top-level code raises an error.

#### Reading text

| Function | Returns |
|---|---|
| `editor.get_text()` | The whole document. |
| `editor.get_range(start, finish)` | The text of a range. |
| `editor.get_selection([n])` | The text of selection `n`, or `""` when it is a caret. Defaults to the main selection. |
| `editor.get_line(n)` | The text of line `n`, without its line ending. |
| `editor.length()` | The document's length in bytes. |
| `editor.eol([value])` | The document's line ending: `"\r\n"`, `"\n"`, or `"\r"`. Passing one of those values converts existing endings and changes newly inserted endings. |
| `editor.tab_width()` | The configured number of columns in a tab stop. |

#### Changing text

| Function | Does |
|---|---|
| `editor.replace_selection(text [, n])` | Replaces selection `n` with `text`, or inserts it at its caret. The caret ends just after the new text. Without `n`, it acts on the main selection and removes the others for compatibility. |
| `editor.replace_range(start, finish, text)` | Replaces a range with `text`. An empty range inserts `text` there. The selection moves with the text around it. |
| `editor.insert(text [, position])` | Inserts `text` at `position`, or at the caret. |
| `editor.set_text(text)` | Replaces the whole document. The caret position isn't kept; prefer `replace_range` for partial changes. |
| `editor.begin_undo()` / `editor.end_undo()` | Mark the start and end of a group of changes that Undo reverses together. Pairs may nest. A command or event handler is already [one undo step](#how-commands-run), so a group inside one never splits it. A group still open when the callback returns is closed then; `end_undo` without a matching `begin_undo` raises an error. |

#### Selection and caret

| Function | Does |
|---|---|
| `editor.get_selection_range([n])` | Returns `start, finish` of selection `n` (an empty range at its caret when nothing is selected). Defaults to the main selection. |
| `editor.set_selection(start, finish [, n])` | Selects a range, or changes selection `n` when supplied. Without `n`, leaves just one selection. |
| `editor.selection_count()` | Returns the number of selections and carets. |
| `editor.get_selections()` | Returns a snapshot table containing `{ start, finish, caret, anchor }` for every selection. |
| `editor.replace_selections(texts)` | Replaces every selection safely. `texts` is one string used for all selections, or a table with one string per selection. Leaves a caret after each replacement. |
| `editor.add_selection(start, finish)` | Adds a selection (an empty range adds a caret) and returns its number. |
| `editor.drop_selection(n)` | Removes selection `n`. The only remaining selection cannot be removed. |
| `editor.get_main_selection()` | Returns the number of the main selection. |
| `editor.set_main_selection(n)` | Makes selection `n` the main selection. |
| `editor.get_caret([n])` | Returns the caret position of selection `n`. Defaults to the main selection. |
| `editor.set_caret(position [, n])` | Moves caret `n` there. Without `n`, removes the other selections. |

Selection numbers are 1-based. Existing plugins that omit `n` continue to work on the main selection; selection-changing calls without `n` intentionally leave just one caret.

For a transformation that applies independently to every caret, take one snapshot and replace everything in one call. This avoids offsets changing underneath a Lua loop:

```lua
local selections = editor.get_selections()
local replacements = {}
for i, range in ipairs(selections) do
  replacements[i] = editor.get_range(range.start, range.finish):upper()
end
editor.replace_selections(replacements)
```

#### Lines

| Function | Returns |
|---|---|
| `editor.line_count()` | The number of lines (at least 1). |
| `editor.current_line()` | The number of the line holding the caret. |
| `editor.line_at(position)` | The number of the line holding `position`. |
| `editor.line_range(n)` | `start, finish` of line `n`, without its line ending (an empty range for an empty line). |

#### The document

| Function | Returns |
|---|---|
| `editor.file_path()` | The document's full path, or `nil` if it has never been saved. |
| `editor.read_relative_file(relativePath)` | Reads a UTF-8 text file near the saved document, resolved against its own folder (`..` can still reach a sibling project folder, but `relativePath` itself must not be absolute). Returns the text, or `nil` plus an error message - the document not being saved yet is one such error. |
| `editor.language()` | How Tinker Editor is treating the document: `"markdown"`, `"json"`, `"xml"`, `"html"`, `"yaml"`, `"cpp"` (C, C++, JavaScript, and TypeScript), `"python"`, `"bash"`, `"powershell"`, `"lua"`, `"css"`, `"sql"`, `"ini"` (also `.cfg`, `.conf` and `.properties`), `"toml"`, `"batch"` (`.bat` and `.cmd`), `"makefile"`, or `"text"`. |
| `editor.modified()` / `editor.read_only([value])` | The modified and read-only states. |
| `editor.visible_range()` / `editor.scroll_to(position)` | The visible byte range, or move the caret to a position and scroll it into view. Moving the caret replaces the selections, so select after scrolling. |
| `editor.find(text [, start [, options]])` | Finds text; options support `match_case`, `whole_word`, `word_start`, and `regex`. Returns the matching range or `nil`. A `regex` pattern that looks like classic catastrophic backtracking (e.g. `(a+)+`), or a search spanning more than 1,000,000 characters, raises an error instead of running. |
| `editor.annotation(...)`, `editor.marker(...)` | Adds/removes line annotations and markers; matching clear functions remove them. |
| `editor.indicator(...)` / `editor.clear_indicator(...)` | Adds or removes temporary highlighted ranges using plugin indicator numbers 8–19. Markers use the same reserved numeric range so they cannot overwrite Tinker Editor's bookmarks, debugger markers, folding markers, or spelling indicator. |
| `editor.fold(line [, expanded])` / `editor.style_at(position)` | Controls folding and reads the lexer style at a position. |
| `editor.clipboard([text])` | Reads Unicode clipboard text, or replaces it when `text` is supplied. |

### ui

These collect input with Tinker Editor's own dialogs, except for `ui.message`, which writes a non-interactive notice to the output panel. Time spent waiting for a dialog doesn't count towards the [time limit](#how-commands-run). Unless given a `title`, a notice or dialog is titled with the command's label (or `script.lua: event` for an event handler). `ui.message` can also be used from a plugin's top-level code, while it loads; the others are for commands and event handlers.

#### `ui.message(text [, title [, kind]])`

Appends `text` under a coloured title in the output panel's Output tab and opens the panel without moving keyboard focus. `kind` chooses the title colour: `"info"` (the default), `"warning"`, or `"error"`. Notices remain in the output history instead of interrupting editing with an OK button. For a longer document-specific report, see [`output.report`](#output).

#### `ui.prompt(label [, default [, title]])`

Asks for one line of text, showing `label` above a text box that starts with `default` (empty if not given). Returns the text, or `nil` if the user cancels.

#### `ui.choose(items [, label [, title]])`

Shows a searchable list of the strings in `items` (1 to 10,000 of them) and lets the user pick one. Typing filters the list. Returns the chosen item's index and the item, or `nil` if the user cancels.

```lua
local index, choice = ui.choose({ "Note", "Tip", "Warning" }, "Alert type:")
if not index then return end
```

#### `ui.save_binary(default_name, data [, title [, open_after_save [, application [, arguments]]]])`

Shows a Save As dialog initialized with `default_name` and writes the Lua string
`data` byte-for-byte. It returns `true` after saving and `false` if the user
cancels. When `open_after_save` is `true`, Windows opens the newly saved file
with its associated application. Supplying `application` runs that executable
instead, with `arguments` followed by the quoted saved-file path. Embedded NUL
bytes and all other byte values are preserved. Output is limited to 64 MiB;
the plugin still cannot choose a destination or launch anything until the
user accepts the Save As dialog.

`ui.confirm(text [, title])` asks Yes/No. `ui.pick_file([title])` and `ui.pick_folder([title])` return a user-selected path or `nil`. `ui.status(text)` shows a temporary status message, and `ui.log(text)` writes a plugin-tagged line to the debugger output and to the output panel's Output tab.

```lua
ui.save_binary("output.bin", string.char(0, 1, 255), "Export binary")
```

### zip

Builds and reads ZIP archives entirely in memory - no path ever touches a real file, so this doesn't relax "plugins can't touch arbitrary files": pack a plugin's own generated files into one archive, hand the bytes to `ui.save_binary`, or unpack bytes a plugin already has (for example, from a future data source) to read what's inside.

#### `zip.pack(entries)`

`entries` is an array of `{ name = "path/in/archive.txt", data = "bytes" }` tables. Returns the archive as a Lua string, or `nil, err` on failure (for example, a bad entry).

#### `zip.unpack(bytes)`

Returns an array of `{ name = ..., data = ... }` tables, one per file in the archive named by `bytes` (a ZIP read from `zip.pack` or elsewhere), or `nil, err` if `bytes` isn't a valid ZIP.

```lua
local archive = assert(zip.pack({
  { name = "notes.txt", data = "hello" },
  { name = "data/values.csv", data = "1,2,3\n" },
}))
ui.save_binary("export.zip", archive)

local entries = assert(zip.unpack(archive))
for _, entry in ipairs(entries) do
  ui.log(entry.name .. ": " .. #entry.data .. " bytes")
end
```

### sqlite

A deliberate exception to "plugins can't touch arbitrary files": `sqlite.open(path)` opens or creates a real SQLite database file at `path`, backed by `winsqlite3.dll`, the SQLite build Windows 10/11 already ships for its own apps - no extra install, no bundled library. Use it for structured data a plugin wants to keep, query, or sort, such as a personal log or notes database. `sqlite` is undefined on a system without `winsqlite3.dll` (older Windows), so check `sqlite ~= nil` before relying on it, or let the resulting error surface.

#### `sqlite.open(path)`

Returns a database object, or `nil, err` on failure. The file is created if it doesn't exist.

#### `db:exec(sql [, ...])`

Runs one statement (DDL, insert/update/delete, or even a `select` whose rows aren't needed). `...` are bound in order to `?` placeholders in `sql`; each must be `nil`, a boolean, a number, or a string. Returns `true, rows_changed`, or `nil, err`.

#### `db:query(sql [, ...])`

Runs a `select` the same way and returns an array of row tables (column name to value) plus the statement's column names in their declared order, or `nil, err`. A SQL `NULL` is simply absent from the row table. The second return exists because a row table's own key order isn't reliable - use it whenever you need columns in the order the query declared them, such as rendering a table.

#### `db:close()`

Closes the database. A database still open when its Lua value is garbage-collected, or when `local db <close> = sqlite.open(...)` goes out of scope, closes automatically either way.

```lua
local db = assert(sqlite.open(app.workspace_path() .. "\\notes.db"))
assert(db:exec("create table if not exists notes (id integer primary key, text text)"))
assert(db:exec("insert into notes (text) values (?)", "Remember to write docs"))
local rows = assert(db:query("select id, text from notes order by id"))
for _, row in ipairs(rows) do
  ui.log(row.id .. ": " .. row.text)
end
db:close()
```

[`sqlite-notes.lua`](sqlite-notes.lua) is a complete example: a small per-file notes command backed by this API. [`json-store.lua`](json-store.lua) and [`yaml-store.lua`](yaml-store.lua) go further, each keeping a small JSON or YAML codec of its own and using `sqlite` as a named-document store: validate a selection, save it under a key, and read it back later from any document. [`sqlite-browser.lua`](sqlite-browser.lua) uses `db:query`'s column order directly, to run arbitrary SQL against any database file and render the result as a Markdown table, and to do the reverse: read a Markdown table out of the document and load it into a new SQLite table.

### fs

The other deliberate exception to "plugins can't touch arbitrary files": read-only, whole-folder access, for a plugin that genuinely needs to look at more than the open document - a wikilink/backlinks index across a notes folder, say. Both functions only ever read; there is no `fs.write_text` or similar.

#### `fs.list_files(folder [, pattern])`

Every file under `folder`, recursed into every subfolder, whose name matches `pattern` (`*` and `?` wildcards; default `"*"`, so `"*.md"` finds every Markdown file). Returns absolute paths, or `nil, err` if `folder` isn't a folder. Dot-folders (`.git`, `.obsidian`, ...) and reparse points (symlinks/junctions, which could otherwise recurse forever) are skipped. Like a runaway `db:query` (see `sqlite` above), a folder large enough to take more than a few seconds to walk stops with `nil, err` instead of hanging the editor.

#### `fs.read_text(path)`

The whole file at `path`, decoded to UTF-8 from whatever encoding it was actually saved as - the same detection the editor itself uses to open a file. Returns the text, or `nil, err`.

```lua
for _, path in ipairs(assert(fs.list_files(folder, "*.md"))) do
  local text = fs.read_text(path)
  if text then
    for target in text:gmatch("%[%[([^%]|]+)") do
      ui.log(path .. " links to " .. target)
    end
  end
end
```

## Where commands appear

A command's label is a path separated by `/`. Each part before the last names a submenu; the last part is the item. An `&` before a letter marks it as the item's access key (`&&` shows a literal `&`).

| Label | Appears in |
|---|---|
| `"Word Count"` | Plugins → Word Count |
| `"Text/Word Count"` | Plugins → Text → Word Count |
| `"Text/Count/Words"` | Plugins → Text → Count → Words |
| `"/Extras/Insert Date"` | A new top-level **Extras** menu, placed before Help |
| `"/Snippets/Insert Date"` | The existing **Snippets** menu, after a separator |
| `"/Tools/Word Count"` | The existing **Tools** menu, after a separator |
| `"/Snippets/Markdown/Word Count"` | The existing Snippets → Markdown submenu |

- A leading `/` places the command relative to the menu bar. Its first part names a top-level menu: an existing one if the name matches (ignoring case and `&`), otherwise a new one. Plugins can add new top-level menus until the menu bar holds 24.
- Submenus are found the same way, by name, and created when they don't exist yet.
- A label of just `/Name` has no menu to go in, so it goes on the Plugins menu like `Name`.
- "The Plugins menu" means wherever the menu definition puts its plugins list (see [Customizing the menu bar](#customizing-the-menu-bar)). If the definition has no plugins list, Tinker Editor adds a Plugins menu for these commands.
- A path has at most 8 parts; anything beyond that is ignored.
- Top-level menu names are drawn using the system's ANSI code page, so plain letters, digits, and common punctuation are safest there. Submenu and item names can use any characters.

## Replacing built-in commands

`commands.register` replaces what a built-in command does everywhere it can be used: the menu, the formatting toolbar, and keyboard shortcuts. Tinker Editor's own internal actions aren't affected. For example, replacing `file.save` changes the Save command and Ctrl+S, but not the save Tinker Editor offers when closing a document with unsaved changes.

```lua
-- Bold with __ instead of **.
commands.register("markdown.bold", function()
  local text = editor.get_selection()
  editor.replace_selection("__" .. text .. "__")
end)
```

Tinker Editor's Markdown formatting commands, plus Toggle Comment and Trim Trailing Whitespace (the ones marked (Lua) below), are written this way in the `src/lua/formatting_*.lua` files built into Tinker Editor. HTML snippets, general Edit conversions and JSON support are standalone bundled plugins in `plugins/html-tools.lua`, `plugins/edit-convert.lua` and `plugins/json-tools.lua`; those plugins create their own menu entries rather than overriding built-in command names.

### Built-in command names

These are the names `commands.register` and the menu definition use.

- **File:** `file.new`, `file.open`, `file.reopen_utf8`, `file.reopen_utf16le`, `file.reopen_utf16be`, `file.reopen_ansi`, `file.quick_open`, `file.find_in_folder`, `file.replace_in_folder`, `file.open_workspace`, `file.insert_file`, `file.save`, `file.save_as`, `file.save_all`, `file.export_html`, `file.export_pdf`, `file.print`, `file.open_terminal`, `file.show_in_explorer`, `file.close_tab`, `file.reopen_closed_tab`, `file.go_back`, `file.exit`
- **Edit** (Lua): `edit.toggle_comment`, `edit.trim_whitespace`
- **Edit:** `edit.undo`, `edit.redo`, `edit.cut`, `edit.copy`, `edit.paste`, `edit.delete`, `edit.select_all`, `edit.duplicate_line`, `edit.move_line_up`, `edit.move_line_down`, `edit.delete_line`, `edit.join_lines`, `edit.select_next`, `edit.select_all_occurrences`, `edit.match_brace`, `edit.bookmark_toggle`, `edit.bookmark_next`, `edit.bookmark_previous`, `edit.bookmark_clear`, `edit.find`, `edit.find_next`, `edit.replace`, `edit.go_to_line`
- **View:** `view.preview`, `view.editor`, `view.next_tab`, `view.previous_tab`, `view.tab_switcher`, `view.pin_tab`, `view.cycle_panes`, `view.theme_light`, `view.theme_dark`, `view.preview_theme_light`, `view.preview_theme_dark`, `view.custom_css`, `view.clear_custom_css`, `view.font`, `view.preview_on_left`, `view.focus_mode`, `view.word_wrap`, `view.spell_check`, `view.spell_check_language`, `view.outline`, `view.file_explorer`, `view.output_panel`, `view.terminal`, `view.minimap`, `view.split_editor`, `view.split_editor_vertical`, `view.reset_split_editor`, `view.show_whitespace`, `view.indent_guides`, `view.use_spaces`, `view.toggle_fold`, `view.fold_all`, `view.unfold_all`, `view.zoom_in`, `view.zoom_out`, `view.zoom_reset`
- **Markdown** (Lua): `markdown.bold`, `markdown.italic`, `markdown.strikethrough`, `markdown.inline_code`, `markdown.heading1`, `markdown.heading2`, `markdown.heading3`, `markdown.blockquote`, `markdown.bullet_list`, `markdown.numbered_list`, `markdown.task_list`, `markdown.link`, `markdown.image`, `markdown.table`, `markdown.table_editor`, `markdown.table_row_above`, `markdown.table_row_below`, `markdown.table_row_delete`, `markdown.table_column_left`, `markdown.table_column_right`, `markdown.table_column_delete`, `markdown.table_sort_ascending`, `markdown.table_sort_descending`, `markdown.table_align_left`, `markdown.table_align_center`, `markdown.table_align_right`, `markdown.code_block`, `markdown.footnote`, `markdown.horizontal_rule`, `markdown.alert`
- **Tools:** `tools.settings`, `tools.markdown_check_workspace_links`, `tools.markdown_backlinks`
- **Run:** `run.run_file`, `run.debug_file`, `run.continue`, `run.step_over`, `run.step_into`, `run.step_out`, `run.toggle_breakpoint`, `run.clear_breakpoints`, `run.use_console`
- **Plugins:** `plugins.reload`, `plugins.new`, `plugins.open_folder`, `plugins.manage`, `plugins.customize_menus`, `plugins.customize_theme`
- **Help:** `help.markdown_reference`, `help.keyboard_shortcuts`, `help.plugin_guide`, `help.about`

## Customizing theme colours

The colours of the light and dark themes come from `%APPDATA%\tinkeredit\themes.lua` when it exists, over the built-in ones. **Plugins → Customize Theme Colours** writes that file (if it isn't there yet), listing every colour with its built-in value and what it paints, and opens it. Edit it, save it, then choose **Plugins → Reload Plugins**. Delete a line to go back to that colour's built-in value, or the file to go back to all of them.

```lua
return {
  light = { selectionBg = "#FFE8A3" },
  dark  = { bg = "#0D1117", keyword = "#FF7B72" },
}
```

- Colours are `"#RRGGBB"`; names are the ones the generated file lists. Unknown names and malformed colours are reported and skipped.
- Only `light` and `dark` exist; **View** and **Settings → Appearance** still switch between them.
- The file runs in a Lua state of its own with only the base and string libraries, so it can compute colours but can't see plugins or the editor.
- It covers the editor and window. The rendered preview is styled by CSS; use **View → Custom CSS** for that.

[examples/themes.lua](examples/themes.lua) is a complete example: Solarized colours for both themes, built from one function, with in-between shades computed by a small `mix()` helper. Copy it to `%APPDATA%\tinkeredit\themes.lua` to try it. Folders in `plugins` without an `init.lua`, like `examples`, aren't loaded as plugins.

## Customizing the menu bar

The whole menu bar is defined by a Lua file. Tinker Editor uses `%APPDATA%\tinkeredit\menus.lua` when it exists, and otherwise its built-in default menus. **Plugins → Customize Menus** copies the default menus to that file (if it isn't there yet) and opens it. Edit it, save it, then choose **Plugins → Reload Plugins**. Delete the file to go back to the default menus.

The file returns a list of top-level menus. Each menu is `{ label, { items } }`, and each item is one of:

| Item | Meaning |
|---|---|
| `{ "Label", "command.name" }` | A built-in command (see [Built-in command names](#built-in-command-names)). Its shortcut and check mark are shown automatically. |
| `{ "Label", function() ... end }` | Lua code, run like a plugin command, with the same API. |
| `{ "Label", { items } }` | A submenu. |
| `"-"` | A separator. |
| `{ list = "recent_files" }` | The recently opened files, and Clear Recent Files. |
| `{ list = "recent_workspaces" }` | The recent workspace folders, and Clear Recent Workspaces. |
| `{ list = "plugins" }` | The commands plugins add without a leading `/`. |

```lua
return {
  { "&File", {
    { "&New", "file.new" },
    { "&Open...", "file.open" },
    { "&Save", "file.save" },
    "-",
    { "Recent &Files", { { list = "recent_files" } } },
    "-",
    { "E&xit", "file.exit" },
  } },
  { "&Snippets", {
    { "&Signature", function() editor.insert("Best regards," .. editor.eol()) end },
  } },
  { "&Plugins", {
    { list = "plugins" },
    "-",
    { "&Reload Plugins", "plugins.reload" },
    { "&New Plugin...", "plugins.new" },
    { "Open Plugins &Folder", "plugins.open_folder" },
    { "&Manage Plugins...", "plugins.manage" },
    { "&Customize Menus...", "plugins.customize_menus" },
    { "Customize &Theme Colours...", "plugins.customize_theme" },
  } },
}
```

- Moving or renaming an item doesn't change its keyboard shortcut. Leaving a command out of the menus doesn't remove its shortcut.
- `{codepage}` in a label is replaced with the system's ANSI code page number.
- Separators at the start or end of a menu, or next to another separator, are left out, so a list that turns out to be empty leaves no gap.
- Plugins place `/Menu/...` commands by menu name, so renaming a menu changes where those commands go.
- Keep the plugin management commands somewhere convenient, especially **Reload Plugins** and **Customize Menus**.
- If the file has a mistake (Lua that doesn't run, or a table in the wrong shape) Tinker Editor says what and where, and uses the default menus instead. An unknown command name is reported and just that item is left out.

## How commands run

- **Commands act on the pane being edited**: the split editor's second pane when it has the focus, otherwise the main one. Event handlers act on the main pane.
- **One undo step.** Everything a command or event handler changes is undone together by a single Undo.
- **Errors are reported and undone.** If a command raises an error (from `error(...)`, a failed `assert`, or a bug), Tinker Editor shows the message and a traceback, and undoes any changes the command had made.
- **The time limit.** A command, event handler, or plugin's top-level code that runs for more than 5 seconds is stopped, with an error, and its changes are undone. Time spent waiting for the user in a `ui` dialog doesn't count. Tinker Editor's own formatting commands have no limit.
- **One plugin at a time.** While a plugin's dialog is open, events (such as an automatic save) don't run other plugins' handlers, and plugin commands can't be started.

## The Lua environment

Plugins run in Lua 5.4, built into `tinkeredit.exe`. No separate Lua installation is needed.

- **Available:** the basic functions (`pairs`, `ipairs`, `tostring`, `tonumber`, `pcall`, `error`, `assert`, `select`, `type`, `setmetatable`, and so on) and the `string`, `table`, `math`, `utf8`, and `coroutine` libraries.
- **Not available:** `io`, `os`, `package`, `debug`, `dofile`, and `loadfile`. Plugins can't touch arbitrary files or run programs, with two deliberate exceptions: [`sqlite.open`](#sqlite) opens a real file on disk, and [`fs.list_files`/`fs.read_text`](#fs) read a whole folder, read-only.
- **Folder extension modules:** `require` works both during startup and later in that extension's commands/events. It reads Lua source only below the extension folder and caches each module privately. Single-file extensions cannot load other files.
- **`load`** accepts Lua source text only, not precompiled chunks.
- **`print`** writes nowhere visible. To see a value while developing a plugin, use `ui.message(tostring(value))`.
- **Strings are bytes.** `string.upper`, `string.lower`, and character classes such as `%a` only understand ASCII letters, and `#text` counts bytes. Use `utf8.len`, `utf8.codes`, and `utf8.offset` for characters.

## Limits

| Limit | Value |
|---|---|
| Size of a plugin file | 1 MiB |
| Commands, in total | 1,024, shared by plugins (`menu.add`, `menu.checkbox`, `menu.separator` and `commands.register`), functions in the menu definition, and Tinker Editor's built-in Lua commands |
| Event handlers, in total | 100 |
| Editor context-menu items, in total | 256 registered; the first 64 are shown |
| Time a call may run | 5 seconds (not counting dialogs) |
| Lua heap | 64 MiB shared by the sandboxed state |
| Deferred callbacks | 64 pending; delays from 0 to 60 seconds |
| Parts in a menu or context-menu label | 8 |
| Top-level menus | 24, including the ones from the menu definition |
| Items in `ui.choose` | 10,000 |
| Words in one `spell.check` | 50,000 |
| Text returned by `ui.prompt` | One line, about 4,000 characters |
| Menu definition nesting | 8 levels, 4,000 items |

## Recipes

### Wrap the selection, or the word at the caret

```lua
menu.add("Text/Highlight", function()
  local s, f = editor.get_selection_range()
  if f < s then
    -- Nothing selected: find the word around the caret.
    local line = editor.current_line()
    local lineStart = editor.line_range(line)
    local text = editor.get_line(line)
    local column = editor.get_caret() - lineStart + 1
    local first, last = 1, 0
    for wordStart, wordEnd in text:gmatch("()%w+()") do
      if wordStart <= column and column <= wordEnd then first, last = wordStart, wordEnd - 1 end
    end
    if last < first then return end
    s, f = lineStart + first - 1, lineStart + last - 1
  end
  editor.replace_range(s, f, "==" .. editor.get_range(s, f) .. "==")
end)
```

### Change every line of the selection

```lua
menu.add("Lines/Indent with Two Spaces", function()
  local s, f = editor.get_selection_range()
  local first, last = editor.line_at(s), editor.line_at(math.max(f, s))
  -- From the bottom up, so earlier positions don't move.
  for n = last, first, -1 do
    local start = editor.line_range(n)
    editor.insert("  ", start)
  end
end)
```

### Ask a question first

```lua
menu.add("/Snippets/Callout...", function()
  local kinds = { "NOTE", "TIP", "IMPORTANT", "WARNING", "CAUTION" }
  local _, kind = ui.choose(kinds, "Callout type:")
  if not kind then return end
  local title = ui.prompt("First line:", "")
  if title == nil then return end
  local e = editor.eol()
  editor.replace_selection("> [!" .. kind .. "]" .. e .. "> " .. title .. e)
end)
```

### Act only on some documents

```lua
-- Markdown files are saved ending with exactly one line ending.
events.on("save", function()
  if editor.language() ~= "markdown" then return end
  local text = editor.get_text()
  local body = text:gsub("[\r\n]+$", "")
  if body == "" or body .. editor.eol() == text then return end
  editor.replace_range(#body + 1, editor.length(), editor.eol())
end)
```

### Keep a setting

Use `config` for values that should survive plugin reloads and Tinker Editor restarts.

```lua
menu.add("Text/Set Wrap Width...", function()
  local answer = ui.prompt("Wrap width:", tostring(config.get("wrap_width", 80)))
  local value = answer and tonumber(answer)
  if value and value >= 10 then config.set("wrap_width", math.floor(value)) end
end)
```

## Troubleshooting

| Problem | What to check |
|---|---|
| The plugin's commands don't appear. | Is it a `.lua` file in `%APPDATA%\tinkeredit\plugins`, or a folder there with `init.lua`? Is it enabled in **Manage Plugins**? Did you reload plugins? Was a load error reported? |
| "the editor is only available while a plugin command runs" | `editor` functions were called from a plugin's top-level code. Move that code into the function passed to `menu.add` or `events.on`. |
| "stopped after 5 seconds without finishing" | A loop that never ends, or work that's too slow on a large document. Read only the lines or range you need rather than the whole text. |
| A command does nothing visible. | Check for an `if ... then return end` that always returns. `ui.message` is the quickest way to see what a command is doing. |
| Two plugins appear to interfere. | Their Lua globals are isolated; check whether both replace the same built-in command, use the same editor indicator/marker number, or respond to the same event. |
| A `commands.register` replacement doesn't take effect. | The name must match a [built-in command name](#built-in-command-names) exactly. If two plugins replace the same command, the one whose file name sorts later wins. |
| A shortcut does nothing. | Use Ctrl or Alt, optionally Shift, with a supported key. Built-in shortcuts take priority when keys conflict. |
| A module cannot be loaded. | `require` works only for folder extensions and searches only inside that folder, both at startup and from callbacks. Use dots for subfolders and omit `.lua`. |
