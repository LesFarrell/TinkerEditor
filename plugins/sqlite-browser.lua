-- SQLite Browser: open or create any SQLite file, run SQL against it, and
-- see the result as a Markdown table - plus the reverse: read a Markdown
-- table out of the document and load it into a new table in the database.
-- Uses the `sqlite` API (see plugins/README.md#sqlite); unlike
-- sqlite-notes.lua/json-store.lua/yaml-store.lua, this plugin is meant to
-- point at any database file, not just one of its own, so it asks for a
-- database explicitly rather than picking a folder the first time any
-- command runs.

----------------------------------------------------------------------
-- The current database
----------------------------------------------------------------------

local function current_db_path()
  return config.get("db_path")
end

-- Opens the current database, runs fn(db), then closes it again - every
-- command gets its own short-lived connection.
local function with_db(fn)
  local path = current_db_path()
  if not path then
    ui.message("Open or create a database first " ..
      "(Plugins > SQLite Browser > Open Existing/Create New Database).", "SQLite Browser", "warning")
    return
  end
  local db, openErr = sqlite.open(path)
  if not db then
    ui.message("Could not open " .. path .. ": " .. tostring(openErr), "SQLite Browser", "error")
    return
  end
  local ok, err = pcall(fn, db)
  db:close()
  if not ok then error(err) end
end

menu.add("/Tools/SQLite Browser/Open Existing Database...", function()
  local path = ui.pick_file("Choose a SQLite database")
  if not path then return end
  local db, err = sqlite.open(path)
  if not db then
    ui.message("Could not open " .. path .. ": " .. tostring(err), "SQLite Browser", "error")
    return
  end
  db:close()
  config.set("db_path", path)
  ui.message("Opened " .. path, "SQLite Browser")
end)

menu.add("/Tools/SQLite Browser/Create New Database...", function()
  local folder = ui.pick_folder("Choose a folder for the new database")
  if not folder then return end
  local name = ui.prompt("New database file name:", "database.db", "SQLite Browser")
  if not name or name == "" then return end
  local path = folder .. "\\" .. name
  local db, err = sqlite.open(path)
  if not db then
    ui.message("Could not create " .. path .. ": " .. tostring(err), "SQLite Browser", "error")
    return
  end
  db:close()
  config.set("db_path", path)
  ui.message("Created " .. path, "SQLite Browser")
end)

menu.add("/Tools/SQLite Browser/Current Database", function()
  local path = current_db_path()
  ui.message(path and ("Current database: " .. path) or "No database is open yet.", "SQLite Browser")
end)

----------------------------------------------------------------------
-- Markdown table rendering (SQL results -> Markdown)
----------------------------------------------------------------------

-- A cell's text must be valid UTF-8 to pass through output.report/editor.*
-- (plugins work in UTF-8 throughout - see "The Lua environment" in the
-- plugin guide); a BLOB column's raw bytes usually aren't, so those get a
-- placeholder instead of breaking the whole report.
local function escape_markdown_cell(value)
  if value == nil then return "" end
  if type(value) == "number" then return tostring(value) end
  local text = tostring(value)
  if utf8.len(text) == nil then return "<binary: " .. #text .. " bytes>" end
  text = text:gsub("\\", "\\\\"):gsub("|", "\\|"):gsub("\r\n", "<br>"):gsub("[\r\n]", "<br>")
  return text
end

local function render_markdown_table(columns, rows)
  if #columns == 0 then return "(no columns)" end
  local lines = { "| " .. table.concat(columns, " | ") .. " |" }
  local separators = {}
  for _ = 1, #columns do separators[#separators + 1] = "---" end
  lines[2] = "| " .. table.concat(separators, " | ") .. " |"
  for _, row in ipairs(rows) do
    local cells = {}
    for i, column in ipairs(columns) do cells[i] = escape_markdown_cell(row[column]) end
    lines[#lines + 1] = "| " .. table.concat(cells, " | ") .. " |"
  end
  return table.concat(lines, "\n")
end

----------------------------------------------------------------------
-- Running SQL (one statement, or several separated by ';')
----------------------------------------------------------------------

local QUERY_KEYWORDS = { select = true, pragma = true, ["with"] = true, explain = true, values = true }

local function is_query_statement(sql)
  local word = sql:match("^%s*(%a+)")
  return word ~= nil and QUERY_KEYWORDS[word:lower()] == true
end

local function source_text()
  local s, f = editor.get_selection_range()
  if f < s then return editor.get_text() end
  return editor.get_range(s, f)
end

-- Splits sql on top-level ';' characters, skipping over quoted strings,
-- quoted identifiers, and comments - the same quoting db:exec/db:query
-- themselves understand, so a semicolon inside a string literal or a
-- comment never ends a statement early.
local function split_statements(sql)
  local statements, start, quote, lineComment, blockComment = {}, 1, nil, false, false
  local i, n = 1, #sql
  while i <= n do
    local char, pair = sql:sub(i, i), sql:sub(i, i + 1)
    if lineComment then
      if char == "\n" then lineComment = false end
      i = i + 1
    elseif blockComment then
      if pair == "*/" then blockComment = false; i = i + 2 else i = i + 1 end
    elseif quote then
      if char == quote then
        if sql:sub(i + 1, i + 1) == quote then i = i + 2 else quote = nil; i = i + 1 end
      else
        i = i + 1
      end
    elseif char == "'" or char == '"' or char == "`" then
      quote = char; i = i + 1
    elseif char == "[" then
      quote = "]"; i = i + 1
    elseif pair == "--" then
      lineComment = true; i = i + 2
    elseif pair == "/*" then
      blockComment = true; i = i + 2
    elseif char == ";" then
      local piece = sql:sub(start, i - 1)
      if piece:match("%S") then statements[#statements + 1] = piece end
      start = i + 1
      i = i + 1
    else
      i = i + 1
    end
  end
  local tail = sql:sub(start)
  if tail:match("%S") then statements[#statements + 1] = tail end
  return statements
end

-- Runs every statement in sql in order (so a pasted schema-plus-data script
-- works in one go); only the last statement's result is reported, since
-- that mirrors what most quick SQL tools show for a multi-statement run.
-- Returns { kind = "rows", rows = ..., columns = ... } or
-- { kind = "changed", changed = N }, or nil, err.
local function run_sql(db, sql)
  local statements = split_statements(sql)
  if #statements == 0 then return nil, "no SQL statement found" end
  local totalChanged = 0
  for i = 1, #statements - 1 do
    local ok, changedOrErr = db:exec(statements[i])
    if not ok then return nil, "statement " .. i .. " failed: " .. tostring(changedOrErr) end
    totalChanged = totalChanged + changedOrErr
  end
  local last = statements[#statements]
  if is_query_statement(last) then
    local rows, columns = db:query(last)
    if not rows then return nil, columns end
    return { kind = "rows", rows = rows, columns = columns }
  end
  local ok, changed = db:exec(last)
  if not ok then return nil, changed end
  return { kind = "changed", changed = totalChanged + changed }
end

menu.add("/Tools/SQLite Browser/Run SQL", function()
  with_db(function(db)
    local sql = source_text()
    if not sql:match("%S") then
      ui.message("Select some SQL to run, or write it in the document.", "SQLite Browser", "warning")
      return
    end
    local result, err = run_sql(db, sql)
    if not result then
      ui.message("SQL failed: " .. tostring(err), "SQLite Browser", "error")
      return
    end
    if result.kind == "rows" then
      if #result.columns == 0 then
        output.report("SQLite Browser", "The query ran successfully and returned no columns.")
      else
        output.report("SQLite Browser",
          render_markdown_table(result.columns, result.rows) .. "\n\n" .. #result.rows .. " row(s).")
      end
    else
      output.report("SQLite Browser", result.changed .. " row(s) affected.")
    end
  end)
end)

menu.add("/Tools/SQLite Browser/Show Tables", function()
  with_db(function(db)
    local rows, columns = db:query(
      "select name, type from sqlite_master where type in ('table', 'view') order by name")
    if not rows then
      ui.message("Could not list tables: " .. tostring(columns), "SQLite Browser", "error")
      return
    end
    if #rows == 0 then
      ui.message("This database has no tables yet.", "SQLite Browser")
      return
    end
    output.report("SQLite Browser: Tables", render_markdown_table(columns, rows))
  end)
end)

menu.add("/Tools/SQLite Browser/Describe Table...", function()
  with_db(function(db)
    local tables, listErr = db:query("select name from sqlite_master where type = 'table' order by name")
    if not tables then
      ui.message("Could not list tables: " .. tostring(listErr), "SQLite Browser", "error")
      return
    end
    if #tables == 0 then
      ui.message("This database has no tables yet.", "SQLite Browser")
      return
    end
    local names = {}
    for _, row in ipairs(tables) do names[#names + 1] = row.name end
    local _, name = ui.choose(names, "Describe which table?", "SQLite Browser")
    if not name then return end
    local rows, columns = db:query('pragma table_info("' .. name:gsub('"', '""') .. '")')
    if not rows then
      ui.message("Could not describe '" .. name .. "': " .. tostring(columns), "SQLite Browser", "error")
      return
    end
    output.report("SQLite Browser: " .. name, render_markdown_table(columns, rows))
  end)
end)

----------------------------------------------------------------------
-- Markdown table parsing (Markdown -> SQL table)
----------------------------------------------------------------------

-- Splits one "| a | b |" row into { "a", "b" }, understanding "\|" as a
-- literal pipe inside a cell (the usual Markdown table escape).
local function split_table_row(line)
  local trimmed = line:match("^%s*(.-)%s*$")
  if trimmed:sub(1, 1) == "|" then trimmed = trimmed:sub(2) end
  if trimmed:sub(-1) == "|" then
    local backslashes, i = 0, #trimmed - 1
    while i >= 1 and trimmed:sub(i, i) == "\\" do backslashes = backslashes + 1; i = i - 1 end
    if backslashes % 2 == 0 then trimmed = trimmed:sub(1, -2) end
  end
  local cells, cell, i = {}, {}, 1
  while i <= #trimmed do
    local char = trimmed:sub(i, i)
    if char == "\\" and trimmed:sub(i + 1, i + 1) == "|" then
      cell[#cell + 1] = "|"
      i = i + 2
    elseif char == "|" then
      cells[#cells + 1] = table.concat(cell):match("^%s*(.-)%s*$")
      cell = {}
      i = i + 1
    else
      cell[#cell + 1] = char
      i = i + 1
    end
  end
  cells[#cells + 1] = table.concat(cell):match("^%s*(.-)%s*$")
  return cells
end

-- A Markdown table separator row: only |, -, :, and whitespace, with at
-- least one '-' (distinguishes "| --- | --- |" from an ordinary data row).
local function is_separator_row(line)
  local trimmed = line:match("^%s*(.-)%s*$")
  if trimmed == "" or not trimmed:match("^[%s|:%-]+$") then return false end
  return trimmed:find("-", 1, true) ~= nil
end

-- Finds the first "header row, then separator row" pair in text and reads
-- every table row after it until a line without a '|' ends the table.
-- Returns headers, rows, or nil, err.
local function parse_markdown_table(text)
  local lines = {}
  for line in (text .. "\n"):gmatch("([^\n]*)\n") do lines[#lines + 1] = (line:gsub("\r$", "")) end
  local headerIndex
  for i, line in ipairs(lines) do
    if line:find("|", 1, true) and line:match("%S") and lines[i + 1] and is_separator_row(lines[i + 1]) then
      headerIndex = i
      break
    end
  end
  if not headerIndex then
    return nil, "no Markdown table found (a header row followed by a '---' separator row)"
  end
  local headers = split_table_row(lines[headerIndex])
  local rows, i = {}, headerIndex + 2
  while i <= #lines and lines[i]:find("|", 1, true) and lines[i]:match("%S") do
    rows[#rows + 1] = split_table_row(lines[i])
    i = i + 1
  end
  if #rows == 0 then return nil, "the table has a header but no data rows" end
  return headers, rows
end

-- A safe, unique SQL identifier for each header, in order: non-identifier
-- characters are dropped, a name that collapses to nothing or starts with
-- a digit gets a fallback, and a repeat gets a numeric suffix.
local function unique_identifiers(names)
  local result, seen = {}, {}
  for i, name in ipairs(names) do
    local id = name:gsub("%s+", "_"):gsub("[^%w_]", "")
    if id == "" then id = "column" .. i
    elseif id:match("^%d") then id = "c_" .. id end
    local base, suffix = id, 1
    while seen[id] do suffix = suffix + 1; id = base .. "_" .. suffix end
    seen[id] = true
    result[i] = id
  end
  return result
end

-- A cell's text as a SQL value: numbers import as numbers (so sorting and
-- arithmetic work on them later), an empty cell imports as NULL, and
-- everything else stays text.
local function cell_value(text)
  if text == nil or text == "" then return nil end
  if text:match("^[%-%+]?%d+%.?%d*$") or text:match("^[%-%+]?%.%d+$") then return tonumber(text) end
  return text
end

menu.add("/Tools/SQLite Browser/Import Markdown Table into Database...", function()
  with_db(function(db)
    local headers, rows = parse_markdown_table(source_text())
    if not headers then
      ui.message("Could not find a Markdown table: " .. tostring(rows), "SQLite Browser", "error")
      return
    end
    local tableName = ui.prompt("Table name:", "", "SQLite Browser")
    if not tableName or tableName == "" then return end
    local quotedTable = '"' .. tableName:gsub('"', '""') .. '"'
    local columns = unique_identifiers(headers)
    local quotedColumns = {}
    for i, column in ipairs(columns) do quotedColumns[i] = '"' .. column .. '"' end

    local ok, createErr = db:exec("create table if not exists " .. quotedTable ..
      " (" .. table.concat(quotedColumns, ", ") .. ")")
    if not ok then
      ui.message("Could not create the table: " .. tostring(createErr), "SQLite Browser", "error")
      return
    end

    local placeholders = {}
    for _ = 1, #columns do placeholders[#placeholders + 1] = "?" end
    local insertSql = "insert into " .. quotedTable .. " (" .. table.concat(quotedColumns, ", ") ..
      ") values (" .. table.concat(placeholders, ", ") .. ")"
    for rowIndex, row in ipairs(rows) do
      local values = {}
      for i = 1, #columns do values[i] = cell_value(row[i]) end
      local insertOk, insertErr = db:exec(insertSql, table.unpack(values))
      if not insertOk then
        ui.message("Row " .. rowIndex .. " failed: " .. tostring(insertErr), "SQLite Browser", "error")
        return
      end
    end
    ui.message(#rows .. " row(s) imported into " .. quotedTable .. ".", "SQLite Browser")
  end)
end)
