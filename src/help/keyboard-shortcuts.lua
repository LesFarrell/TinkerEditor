-- Help > Keyboard Shortcuts: builds the page as Markdown each time it opens.
--
-- The page is generated rather than written out by hand, so it always
-- matches the shortcuts Tinker Editor really has and the menus the user really
-- sees, including a customised user menus.lua (%APPDATA%\tinkeredit normally,
-- or portable-data in portable mode).
--
-- How it's run: tinkeredit.rc builds this file into tinkeredit.exe, and
-- plugin_host_render_page() runs it in a Lua state of its own (not the one
-- plugins share), with the base, string, table and utf8 libraries only. It
-- gets one argument, a table:
--
--   menus      the loaded menu definition, as a list of top-level menus.
--              Each item is a table with a `kind`:
--                "submenu"    label, items
--                "command"    label, command (a built-in name, "file.new")
--                "function"   label (Lua code in the definition)
--                "list"       list ("recent_files", "plugins", ...)
--                "separator"
--   shortcuts  command -> shortcut text, for every command that has one:
--              built-in commands by name, e.g. shortcuts["file.new"] ==
--              "Ctrl+N" (from src/menu_commands.c), and plugin commands by
--              their menu label, e.g. shortcuts["/Tools/JSON/Format JSON"]
--              == "Ctrl+Alt+J" (from src/internal/help_and_menu.inc)
--
-- and returns the page. Changes here need a rebuild; tests/test_menus.c
-- checks the page lists every shortcut.

local data = ...
local menus, shortcuts = data.menus, data.shortcuts

-- Built-in command names are lower case and dotted, "file.new"; anything
-- else in shortcuts is a plugin command's label.
local function is_builtin(name)
  return name:match("^[%l%d_]+%.[%l%d_.]+$") ~= nil
end

-- Keys that aren't built-in commands, so neither the menus nor the command
-- table know about them.
local OTHER_KEYS = {
  { "Tab / Shift+Tab", "Smart indent / outdent" },
  { "Ctrl+Shift+F6", "Cycle panes backwards" },
  { "Ctrl+Numpad + / Ctrl+Numpad -", "Zoom in / out" },
  { "Alt+drag", "Column (rectangular/block) selection" },
  { "Alt+Shift+Arrow keys", "Extend a column selection" },
}

-- A menu label as shown: "&&" is a literal "&", any other "&" only marks
-- the access key.
local function plain(label)
  return (label:gsub("&(.?)", "%1"))
end

-- Text safe inside a Markdown table cell.
local function cell(text)
  return (text:gsub("[\\|`*_]", "\\%0"))
end

local out = {}
local function line(text) out[#out + 1] = text or "" end

-- A "## heading" and a two-column table of rows ({ keys, action } pairs),
-- or nothing when there are no rows.
local function section(heading, rows)
  if #rows == 0 then return end
  line("## " .. heading)
  line()
  line("| Shortcut | Action |")
  line("|---|---|")
  for _, row in ipairs(rows) do line("| " .. cell(row[1]) .. " | " .. cell(row[2]) .. " |") end
  line()
end

-- The rows for every command with a shortcut in items, depth first. An
-- item in a submenu is named by its path: "Bookmarks > Next Bookmark".
local listed = {}
local function collect(items, path, rows)
  for _, item in ipairs(items) do
    if item.kind == "submenu" then
      collect(item.items, path .. plain(item.label) .. " > ", rows)
    elseif item.kind == "command" and shortcuts[item.command] and not listed[item.command] then
      listed[item.command] = true
      rows[#rows + 1] = { shortcuts[item.command], path .. plain(item.label) }
    end
  end
  return rows
end

line("# Tinker Editor Keyboard Shortcuts")
line()
line("Every command is also in the menus.")
line("This page is built from Tinker Editor's current menus each time it opens. It's an")
line("unsaved document, so editing it changes nothing.")
line()

for _, menu in ipairs(menus) do
  section(plain(menu.label), collect(menu.items, "", {}))
end

-- A plugin command's label as the menu path it appears at: a leading "/"
-- places it relative to the menu bar, anything else (or a lone "/Name") on
-- the Plugins menu, as plugins/README.md describes.
local function plugin_path(label)
  local parts = {}
  for part in label:gmatch("[^/]+") do parts[#parts + 1] = plain(part) end
  if label:sub(1, 1) ~= "/" or #parts < 2 then table.insert(parts, 1, "Plugins") end
  return table.concat(parts, " > ")
end

-- Built-in commands left out of the menus keep their shortcuts. Plugin
-- commands are never in the menu definition, so they get a section of
-- their own.
local unlisted, plugins = {}, {}
for name, keys in pairs(shortcuts) do
  if not is_builtin(name) then plugins[#plugins + 1] = { keys, plugin_path(name) }
  elseif not listed[name] then unlisted[#unlisted + 1] = { keys, name } end
end
local function by_action(a, b) return a[2] < b[2] end
table.sort(unlisted, by_action)
table.sort(plugins, by_action)
section("Not in the menus", unlisted)
section("Plugin commands", plugins)

section("Other keys", OTHER_KEYS)

return table.concat(out, "\n")
