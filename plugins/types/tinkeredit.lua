---@meta
-- Tinker Editor Lua API definitions for Lua Language Server. This file documents
-- globals supplied by Tinker Editor; it is not loaded as an extension.

---@class TinkerEditorSelection
---@field start integer
---@field finish integer
---@field caret integer
---@field anchor integer

---@class TinkerEditorMenu
menu = {}
---@param label string
---@param fn fun()
---@param shortcut? string Ctrl/Alt plus a letter, digit, F1-F24, Tab, Space, Insert or Delete.
---@param enabled? fun():boolean Evaluated whenever menu state is refreshed.
function menu.add(label, fn, shortcut, enabled) end
---@param label string
---@param key string
---@param default boolean
---@param fn fun(checked:boolean)
---@param shortcut? string
function menu.checkbox(label, key, default, fn, shortcut) end
---@param path? string
function menu.separator(path) end

contextmenu = {}
---Adds an item to the editor's right-click menu; slashes create submenus.
---@param label string
---@param fn fun()
---@param enabled? fun():boolean Evaluated whenever the context menu opens.
---@return integer item
function contextmenu.add(label, fn, enabled) end
---Adds a persistent checkable item; slashes create submenus.
---@param label string
---@param key string
---@param default boolean
---@param fn fun(checked:boolean)
---@param enabled? fun():boolean Evaluated whenever the context menu opens.
---@return integer item
function contextmenu.checkbox(label, key, default, fn, enabled) end
---Removes an item added by this plugin.
---@param item integer
---@return boolean removed
function contextmenu.remove(item) end

commands = {}
---@param name string
---@param fn fun()
---@param title? string
function commands.register(name, fn, title) end
---Raises an error for the run.* commands, which plugins can't execute.
---@param name string
---@return boolean queued
function commands.execute(name) end

events = {}
---@param name 'before_save'|'save'|'after_save'|'open'|'close'|'tab_changed'|'text_changed'|'selection_changed'|'workspace_changed'|'theme_changed'|'shutdown'|'reload'
---@param fn fun(path: string|nil)
---@return integer subscription
function events.on(name, fn) end
---@param subscription integer
---@return boolean removed
function events.off(subscription) end

config = {}
---@generic T
---@param key string
---@param default? T
---@return T?
function config.get(key, default) end
---@param key string
---@param value string|number|boolean|table|nil
function config.set(key, value) end

app = {}
---@return string
function app.date() end
---@return string
function app.time() end
---@return string
function app.date_time() end
---@return string
function app.version() end
---@return integer
function app.api_version() end
---@return 'windows'
function app.platform() end
---@return integer
function app.memory_used() end
---@return string?
function app.workspace_path() end
---Builds a Lua stack traceback for an error handler.
---@param message? string
---@return string
function app.traceback(message) end
---@return table<string, boolean>
function app.capabilities() end
---@param milliseconds integer 0 to 60000.
---@param fn fun()
---@return integer token
function app.defer(milliseconds, fn) end
---@param token integer
---@return boolean
function app.cancel(token) end

ui = {}
---Appends a titled notice to the output panel's Output tab and opens it.
---@param text string
---@param title? string
---@param kind? 'info'|'warning'|'error'
function ui.message(text, title, kind) end
---@param label string
---@param default? string
---@param title? string
---@return string?
function ui.prompt(label, default, title) end
---@param items string[]
---@param label? string
---@param title? string
---@return integer?, string?
function ui.choose(items, label, title) end

---@param default_name string
---@param data string Binary data; embedded NUL bytes are preserved.
---@param title? string
---@param open_after_save? boolean Open the saved file with its Windows-associated application.
---@param application? string Application to run instead of the file association.
---@param arguments? string Arguments placed before the quoted saved-file path.
---@return boolean saved False when the user cancels the Save As dialog.
function ui.save_binary(default_name, data, title, open_after_save, application, arguments) end
---@param text string
---@param title? string
---@return boolean
function ui.confirm(text, title) end
---@param text string
function ui.status(text) end
---@param text string
function ui.log(text) end
---@param title? string
---@return string?
function ui.pick_file(title) end
---@param title? string
---@return string?
function ui.pick_folder(title) end

documents = {}
---@alias TinkerEditorEncoding 'utf-8'|'utf-8-bom'|'utf-16le'|'utf-16be'|'ansi'
---@return integer
function documents.count() end
---@return integer
function documents.active() end
---@param index integer
---@return boolean
function documents.activate(index) end
---@return boolean
function documents.new() end
---Asks the user first unless the file is already open in a tab.
---@param path string
---@return boolean
function documents.open(path) end
---@param save_as? boolean
---@return boolean
function documents.save(save_as) end
---@param index? integer
---@return boolean
function documents.close(index) end
---@param index? integer
---@return string?
function documents.path(index) end
---@param index? integer
---@return boolean
function documents.modified(index) end
---Whether the document reloads changes on disk without asking (while it has no
---unsaved edits); sets it when `on` is given.
---@overload fun(): boolean
---@overload fun(on: boolean): boolean
---@param index integer
---@param on? boolean
---@return boolean
function documents.auto_reload(index, on) end
---@overload fun(): TinkerEditorEncoding
---@param value TinkerEditorEncoding
---@return boolean success
function documents.encoding(value) end

output = {}
---Appends text to the output panel's Output tab; ANSI colour sequences colour it.
---@param ... string
function output.write(...) end
function output.clear() end
---Opens the output panel on the Output tab.
function output.show() end
---Appends text under a heading naming the report and the document, and opens the panel.
---@param title string
---@param text string
function output.report(title, text) end
---Shows rows as a sortable grid on the Table tab (click a column header to sort,
---click again to reverse) and opens the panel on it; replaces whatever the grid
---held before. `rows` is keyed by the names in `columns` - the same shape
---`db:query` returns, so its two results can be passed straight through.
---@param columns string[]
---@param rows table<string, boolean|number|string|nil>[]
function output.table(columns, rows) end
---Closes the output panel; programs running in it keep running.
function output.hide() end
---@return boolean
function output.visible() end

terminal = {}
---Starts an in-process session on the Terminal tab, preserving its shell.
function terminal.begin() end
---Writes UTF-8/ANSI text to the active terminal session.
---@param ... string
function terminal.write(...) end
---Reads and echoes one line. Escape or Stop returns nil.
---@param prompt? string
---@return string?
function terminal.read(prompt) end
---Clears the active terminal session's screen and scrollback.
function terminal.clear() end
---Ends the session and leaves its transcript visible.
function terminal.finish() end

problems = {}
---Replaces a named source's report in the Problems tab without removing other sources.
---@param source string
---@param text string
function problems.report(source, text) end
---Removes one named source's report from the Problems tab.
---@param source string
function problems.clear(source) end
---Removes every report from the Problems tab.
function problems.clear_all() end

spell = {}
---The Windows spelling dictionary's locale name, such as "en-GB", or nil when
---Windows has none.
---@return string?
function spell.language() end
---Whether the dictionary accepts a word, or each of up to 50,000 words.
---Text containing several words is accepted only if all of them are.
---@overload fun(word: string): boolean
---@param words string[]
---@return boolean[]
function spell.check(words) end

editor = {}
---@return string
function editor.get_text() end
---@param start integer
---@param finish integer
---@return string
function editor.get_range(start, finish) end
---@param n? integer
---@return string
function editor.get_selection(n) end
---@param n integer
---@return string
function editor.get_line(n) end
---@return integer
function editor.length() end
---@alias TinkerEditorLineEnding '\r\n'|'\n'|'\r'
---@overload fun(): TinkerEditorLineEnding
---@param value TinkerEditorLineEnding
---@return TinkerEditorLineEnding
function editor.eol(value) end
---@return integer
function editor.tab_width() end
---@param text string
---@param n? integer
function editor.replace_selection(text, n) end
---@param start integer
---@param finish integer
---@param text string
function editor.replace_range(start, finish, text) end
---@param text string
---@param position? integer
function editor.insert(text, position) end
---@param text string
function editor.set_text(text) end
---@param n? integer
---@return integer start, integer finish
function editor.get_selection_range(n) end
---@param start integer
---@param finish integer
---@param n? integer
function editor.set_selection(start, finish, n) end
---@return integer
function editor.selection_count() end
---@return TinkerEditorSelection[]
function editor.get_selections() end
---@param texts string|string[]
function editor.replace_selections(texts) end
---@param start integer
---@param finish integer
---@return integer
function editor.add_selection(start, finish) end
---@param n integer
function editor.drop_selection(n) end
---@return integer
function editor.get_main_selection() end
---@param n integer
function editor.set_main_selection(n) end
---@param n? integer
---@return integer
function editor.get_caret(n) end
---@param position integer
---@param n? integer
function editor.set_caret(position, n) end
---@return integer
function editor.line_count() end
---@return integer
function editor.current_line() end
---@param position integer
---@return integer
function editor.line_at(position) end
---@param n integer
---@return integer start, integer finish
function editor.line_range(n) end
---@return string?
function editor.file_path() end
---Reads a UTF-8 text file near the active document, resolved relative to its
---own folder (the document must be saved). The path must be relative, not
---absolute. Returns the text, or nil plus an error message.
---@param relativePath string
---@return string? text, string? error
function editor.read_relative_file(relativePath) end
---@return string
function editor.language() end
---@return boolean
function editor.modified() end
---@param value? boolean
---@return boolean
function editor.read_only(value) end
---@return integer start, integer finish
function editor.visible_range() end
---Moves the caret there, replacing the selections, and scrolls it into view.
---@param position integer
function editor.scroll_to(position) end
---@class TinkerEditorFindOptions
---@field match_case boolean|nil
---@field whole_word boolean|nil
---@field word_start boolean|nil
---@field regex boolean|nil

---Finds text, or with `options.regex` a pattern, from `start` onward.
---Raises an error instead of searching if the pattern looks like classic
---catastrophic backtracking (e.g. `(a+)+`), or if a regex search would span
---more than 1,000,000 characters.
---@param text string
---@param start? integer
---@param options? TinkerEditorFindOptions
---@return integer? start, integer? finish
function editor.find(text, start, options) end
function editor.begin_undo() end
function editor.end_undo() end
---@param line integer
---@param text? string
---@param style? integer
function editor.annotation(line, text, style) end
function editor.clear_annotations() end
---@param line integer
---@param marker? integer 8 to 19.
---@param enabled? boolean
function editor.marker(line, marker, enabled) end
---@param marker? integer 8 to 19; omit to clear every plugin marker.
function editor.clear_markers(marker) end
---@param start integer
---@param finish integer
---@param number integer 8 to 19.
---@param colour? integer RGB value, for example 0xffc107.
function editor.indicator(start, finish, number, colour) end
---@param number integer
---@param start? integer
---@param finish? integer
function editor.clear_indicator(number, start, finish) end
---@param line integer
---@param expanded? boolean
---@return boolean
function editor.fold(line, expanded) end
---@param position integer
---@return integer
function editor.style_at(position) end
---@param text? string When supplied, replaces the clipboard text.
---@return string?
function editor.clipboard(text) end
