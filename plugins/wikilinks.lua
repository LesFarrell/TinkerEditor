-- Wikilinks: Obsidian-style [[Note Name]] / [[Note Name|Display Text]] links
-- across a folder of Markdown files, with a SQLite-backed index (see
-- plugins/README.md#sqlite and #fs) giving backlinks and a quick switcher.
-- Unlike sqlite-browser.lua, this plugin needs every Markdown file under a
-- chosen vault folder, not just the open document - that's what `fs` is
-- for (plugins/README.md#fs), read-only, after an explicit folder choice.

----------------------------------------------------------------------
-- The link index
----------------------------------------------------------------------

local function vault_folder()
  return config.get("vault_folder")
end

-- A dot-prefixed name so fs.list_files's own vault scan (and a human
-- browsing the vault) both skip the index database as just more noise.
local function index_db_path()
  local vault = vault_folder()
  return vault and (vault .. "\\.tinkeredit-links.db") or nil
end

local function with_db(fn)
  local path = index_db_path()
  if not path then
    ui.message("Choose a vault folder first (Tools > Wikilinks > Choose Vault Folder...).",
      "Wikilinks", "warning")
    return
  end
  local db, err = sqlite.open(path)
  if not db then
    ui.message("Could not open the link index: " .. tostring(err), "Wikilinks", "error")
    return
  end
  local ok, err2 = pcall(fn, db)
  db:close()
  if not ok then error(err2) end
end

-- "C:\vault\sub\Note Name.md" -> "Note Name". Two files sharing a name in
-- different subfolders collide here (the later one wins in the index) -
-- the same restriction Obsidian itself has for plain [[wikilinks]].
local function note_name_from_path(path)
  local name = path:match("([^\\/]+)$") or path
  return (name:gsub("%.%w+$", ""))
end

-- Every [[target]] or [[target|display text]] in text, target only, in
-- order (duplicates kept - Rebuild Link Index counts them as found, not
-- as distinct backlinks, which dedupes with `select distinct`).
local function extract_links(text)
  local targets = {}
  for link in text:gmatch("%[%[(.-)%]%]") do
    local target = (link:match("^(.-)%s*|") or link):match("^%s*(.-)%s*$")
    if target ~= "" then targets[#targets + 1] = target end
  end
  return targets
end

menu.add("/Tools/Wikilinks/Choose Vault Folder...", function()
  local folder = ui.pick_folder("Choose your notes vault folder")
  if not folder then return end
  config.set("vault_folder", folder)
  ui.message("Vault folder set to " .. folder .. ". Run Rebuild Link Index next.", "Wikilinks")
end)

menu.add("/Tools/Wikilinks/Rebuild Link Index", function()
  local vault = vault_folder()
  if not vault then
    ui.message("Choose a vault folder first (Tools > Wikilinks > Choose Vault Folder...).",
      "Wikilinks", "warning")
    return
  end
  local files, listErr = fs.list_files(vault, "*.md")
  if not files then
    ui.message("Could not scan the vault: " .. tostring(listErr), "Wikilinks", "error")
    return
  end
  with_db(function(db)
    assert(db:exec("begin"))
    assert(db:exec("drop table if exists notes"))
    assert(db:exec("drop table if exists links"))
    assert(db:exec("create table notes (name text, path text primary key)"))
    assert(db:exec("create table links (source_path text, target_name text)"))
    local linkCount = 0
    for _, path in ipairs(files) do
      assert(db:exec("insert into notes (name, path) values (?, ?)", note_name_from_path(path), path))
      local text = fs.read_text(path)
      if text then
        for _, target in ipairs(extract_links(text)) do
          assert(db:exec("insert into links (source_path, target_name) values (?, ?)", path, target))
          linkCount = linkCount + 1
        end
      end
    end
    assert(db:exec("commit"))
    ui.message(#files .. " note(s) indexed, " .. linkCount .. " link(s) found.", "Wikilinks")
  end)
end)

----------------------------------------------------------------------
-- Navigating links
----------------------------------------------------------------------

-- The target of the [[wikilink]] the caret is currently inside, or nil.
local function link_at_caret()
  local lineNumber = editor.current_line()
  local lineStart = editor.line_range(lineNumber)
  local line = editor.get_line(lineNumber)
  local column = editor.get_caret() - lineStart + 1
  local searchFrom = 1
  while true do
    local openStart, openEnd = line:find("%[%[", searchFrom)
    if not openStart then return nil end
    local closeStart, closeEnd = line:find("%]%]", openEnd + 1)
    if not closeStart then return nil end
    if column >= openStart and column <= closeEnd then
      local target = (line:sub(openEnd + 1, closeStart - 1):match("^(.-)%s*|") or
        line:sub(openEnd + 1, closeStart - 1)):match("^%s*(.-)%s*$")
      return target ~= "" and target or nil
    end
    searchFrom = closeEnd + 1
  end
end

menu.add("/Tools/Wikilinks/Open Link at Cursor", function()
  local target = link_at_caret()
  if not target then
    ui.message("Place the caret inside a [[wikilink]] first.", "Wikilinks", "warning")
    return
  end
  with_db(function(db)
    local rows, err = db:query("select path from notes where lower(name) = lower(?) limit 1", target)
    if not rows then
      ui.message("Could not read the index: " .. tostring(err), "Wikilinks", "error")
      return
    end
    if #rows == 0 then
      ui.message("No note named '" .. target .. "' in the index. Create the file, " ..
        "then run Rebuild Link Index.", "Wikilinks", "warning")
      return
    end
    documents.open(rows[1].path)
  end)
end)

menu.add("/Tools/Wikilinks/Show Backlinks", function()
  local path = editor.file_path()
  if not path then
    ui.message("Save this note before looking up its backlinks.", "Wikilinks", "warning")
    return
  end
  local name = note_name_from_path(path)
  with_db(function(db)
    local rows, err = db:query(
      "select distinct source_path from links where lower(target_name) = lower(?) order by source_path", name)
    if not rows then
      ui.message("Could not read the index: " .. tostring(err), "Wikilinks", "error")
      return
    end
    if #rows == 0 then
      ui.message("No notes in the index link to '" .. name .. "' yet.", "Wikilinks")
      return
    end
    local labels = {}
    for i, row in ipairs(rows) do labels[i] = row.source_path end
    local index = ui.choose(labels, "Notes linking to " .. name .. ":", "Wikilinks")
    if not index then return end
    documents.open(labels[index])
  end)
end)

menu.add("/Tools/Wikilinks/Quick Switch to Note...", function()
  with_db(function(db)
    local rows, err = db:query("select name, path from notes order by name")
    if not rows then
      ui.message("Could not read the index: " .. tostring(err), "Wikilinks", "error")
      return
    end
    if #rows == 0 then
      ui.message("The index is empty. Run Rebuild Link Index first.", "Wikilinks", "warning")
      return
    end
    local labels = {}
    for i, row in ipairs(rows) do labels[i] = row.name end
    local index = ui.choose(labels, "Open note:", "Wikilinks")
    if not index then return end
    documents.open(rows[index].path)
  end)
end)
