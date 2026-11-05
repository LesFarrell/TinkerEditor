-- Quick Notes: a tiny demo of the `sqlite` API. Notes are kept in a real
-- SQLite file on disk (see sqlite.open in the plugin guide) so they survive
-- restarts and can be queried or backed up like any other .db file.

local function db_path()
  local path = config.get("db_path")
  if path then return path end
  local folder = ui.pick_folder("Choose a folder for the notes database")
  if not folder then return nil end
  path = folder .. "\\tinkeredit-notes.db"
  config.set("db_path", path)
  return path
end

-- Opens the database, makes sure the table exists, runs fn(db), then closes
-- it again - every command gets its own short-lived connection rather than
-- keeping one open for the life of the plugin.
local function with_db(fn)
  local path = db_path()
  if not path then return end
  local db, err = sqlite.open(path)
  if not db then
    ui.message("Could not open " .. path .. ": " .. tostring(err), "Quick Notes", "error")
    return
  end
  local ok, createErr = db:exec([[
    create table if not exists notes (
      id integer primary key autoincrement,
      file text,
      text text,
      created text
    )
  ]])
  if ok then
    local callOk, callErr = pcall(fn, db)
    if not callOk then
      db:close()
      error(callErr)
    end
  else
    ui.message("Could not prepare the notes database: " .. tostring(createErr), "Quick Notes", "error")
  end
  db:close()
end

menu.add("/Tools/Notes/Add Note for This File...", function()
  with_db(function(db)
    local text = ui.prompt("Note text:", "", "Quick Notes")
    if not text or text == "" then return end
    local ok, err = db:exec("insert into notes (file, text, created) values (?, ?, ?)",
      editor.file_path() or "(unsaved)", text, app.date_time())
    if not ok then ui.message("Could not save the note: " .. tostring(err), "Quick Notes", "error") end
  end)
end)

menu.add("/Tools/Notes/Show Notes for This File", function()
  with_db(function(db)
    local path = editor.file_path() or "(unsaved)"
    local rows, err = db:query("select created, text from notes where file = ? order by id", path)
    if not rows then
      ui.message("Could not read notes: " .. tostring(err), "Quick Notes", "error")
      return
    end
    if #rows == 0 then
      ui.message("No notes for this file yet.", "Quick Notes")
      return
    end
    local lines = {}
    for _, row in ipairs(rows) do lines[#lines + 1] = row.created .. "  " .. row.text end
    output.report("Quick Notes", table.concat(lines, "\n"))
  end)
end)

menu.add("/Tools/Notes/Show All Notes", function()
  with_db(function(db)
    local rows, err = db:query("select created, file, text from notes order by id")
    if not rows then
      ui.message("Could not read notes: " .. tostring(err), "Quick Notes", "error")
      return
    end
    if #rows == 0 then
      ui.message("No notes yet.", "Quick Notes")
      return
    end
    local lines = {}
    for _, row in ipairs(rows) do lines[#lines + 1] = row.created .. "  " .. row.file .. "  " .. row.text end
    output.report("Quick Notes", table.concat(lines, "\n"))
  end)
end)

menu.add("/Tools/Notes/Choose Notes Database File...", function()
  config.set("db_path", nil)
  local path = db_path()
  if path then ui.message("Notes database: " .. path, "Quick Notes") end
end)
