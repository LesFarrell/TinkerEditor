-- JSON Store: saves named JSON documents in a SQLite file via the `sqlite`
-- API (see plugins/README.md#sqlite) and reads them back. Each plugin has
-- its own private environment (see "The Lua environment" in the plugin
-- guide), so this can't reuse json-tools.lua's parser - it carries a small
-- JSON codec of its own instead.
--
-- Known limitation of this codec, like most small JSON libraries: a Lua
-- table has no way to hold a "null" value, so decoding JSON `null` inside
-- an array or object yields a missing/absent entry rather than a real null,
-- and a round trip renumbers array holes. Plain config-shaped JSON (nested
-- objects, arrays, strings, numbers, booleans) round-trips exactly.

----------------------------------------------------------------------
-- Encode (Lua value -> JSON text, sorted keys, optional pretty-printing)
----------------------------------------------------------------------

local function encode_string(s)
  local out = { '"' }
  for i = 1, #s do
    local byte, char = s:byte(i), s:sub(i, i)
    if char == '"' then out[#out + 1] = '\\"'
    elseif char == '\\' then out[#out + 1] = '\\\\'
    elseif byte == 8 then out[#out + 1] = '\\b'
    elseif byte == 9 then out[#out + 1] = '\\t'
    elseif byte == 10 then out[#out + 1] = '\\n'
    elseif byte == 12 then out[#out + 1] = '\\f'
    elseif byte == 13 then out[#out + 1] = '\\r'
    elseif byte < 0x20 then out[#out + 1] = string.format('\\u%04x', byte)
    else out[#out + 1] = char end
  end
  out[#out + 1] = '"'
  return table.concat(out)
end

local function is_array(t)
  local count = 0
  for _ in pairs(t) do count = count + 1 end
  return count == #t
end

local encode_value -- forward declaration (recursive)

-- indent is nil for compact output, or the current indent string while
-- pretty-printing (one more level than the value's container).
local function encode_container(parts, open, close, indent)
  if #parts == 0 then return open .. close end
  if not indent then return open .. table.concat(parts, ",") .. close end
  local inner = indent .. "  "
  return open .. "\n" .. inner .. table.concat(parts, ",\n" .. inner) .. "\n" .. indent .. close
end

encode_value = function(value, indent, seen)
  if value == nil then return "null" end
  local kind = type(value)
  if kind == "boolean" then return value and "true" or "false" end
  if kind == "number" then
    if value ~= value or value == math.huge or value == -math.huge then
      error("cannot represent a non-finite number in JSON")
    end
    return (math.type(value) == "integer") and tostring(value) or string.format("%.17g", value)
  end
  if kind == "string" then return encode_string(value) end
  if kind ~= "table" then error("cannot encode a " .. kind .. " value as JSON") end
  if seen[value] then error("cannot encode a table that contains itself") end
  seen[value] = true
  local nextIndent = indent and (indent .. "  ") or nil
  local parts = {}
  if is_array(value) then
    for i = 1, #value do parts[i] = encode_value(value[i], nextIndent, seen) end
    seen[value] = nil
    return encode_container(parts, "[", "]", indent)
  end
  local keys = {}
  for key in pairs(value) do
    if type(key) ~= "string" then error("JSON object keys must be strings") end
    keys[#keys + 1] = key
  end
  table.sort(keys)
  for i, key in ipairs(keys) do
    local sep = indent and ": " or ":"
    parts[i] = encode_string(key) .. sep .. encode_value(value[key], nextIndent, seen)
  end
  seen[value] = nil
  return encode_container(parts, "{", "}", indent)
end

-- Returns the JSON text, or nil, err if value contains something JSON can't
-- represent (a function, non-string key, NaN, and so on).
local function encode_json(value, pretty)
  local ok, result = pcall(encode_value, value, pretty and "" or nil, {})
  if ok then return result end
  return nil, result
end

----------------------------------------------------------------------
-- Decode (JSON text -> Lua value)
----------------------------------------------------------------------

-- Returns the decoded value (which may legitimately be nil for JSON
-- `null`), or nil, err on a syntax error - check err, not the value, to
-- tell the two apart.
local function decode_json(text)
  local position, length = 1, #text

  local function skip_ws()
    while position <= length do
      local byte = text:byte(position)
      if byte == 32 or byte == 9 or byte == 10 or byte == 13 then position = position + 1 else break end
    end
  end
  local function fail(message) error(message .. " at position " .. position, 0) end

  local function parse_string()
    position = position + 1 -- opening quote
    local out = {}
    while true do
      if position > length then fail("unterminated string") end
      local char = text:sub(position, position)
      if char == '"' then position = position + 1; break end
      if char ~= "\\" then
        out[#out + 1] = char
        position = position + 1
      else
        local escape = text:sub(position + 1, position + 1)
        local literal = ({ ['"'] = '"', ["\\"] = "\\", ["/"] = "/", b = "\b", f = "\f",
                            n = "\n", r = "\r", t = "\t" })[escape]
        if literal then
          out[#out + 1] = literal
          position = position + 2
        elseif escape == "u" then
          local code = tonumber(text:sub(position + 2, position + 5), 16)
          if not code then fail("invalid \\u escape") end
          position = position + 6
          if code >= 0xD800 and code <= 0xDBFF and text:sub(position, position + 1) == "\\u" then
            local low = tonumber(text:sub(position + 2, position + 5), 16)
            if low and low >= 0xDC00 and low <= 0xDFFF then
              code = 0x10000 + (code - 0xD800) * 0x400 + (low - 0xDC00)
              position = position + 6
            end
          end
          out[#out + 1] = utf8.char(code)
        else
          fail("invalid escape '\\" .. escape .. "'")
        end
      end
    end
    return table.concat(out)
  end

  local function parse_number()
    local start = position
    local function digits() while position <= length and text:byte(position) >= 48
      and text:byte(position) <= 57 do position = position + 1 end end
    if text:sub(position, position) == "-" then position = position + 1 end
    digits()
    if text:sub(position, position) == "." then position = position + 1; digits() end
    if text:sub(position, position):match("[eE]") then
      position = position + 1
      if text:sub(position, position):match("[+-]") then position = position + 1 end
      digits()
    end
    local number = tonumber(text:sub(start, position - 1))
    if not number then fail("invalid number") end
    return number
  end

  local parse_value
  parse_value = function()
    skip_ws()
    if position > length then fail("unexpected end of JSON text") end
    local char = text:sub(position, position)
    if char == '"' then return parse_string() end
    if char == "{" then
      position = position + 1
      local object = {}
      skip_ws()
      if text:sub(position, position) == "}" then position = position + 1; return object end
      while true do
        skip_ws()
        if text:sub(position, position) ~= '"' then fail("expected a string key") end
        local key = parse_string()
        skip_ws()
        if text:sub(position, position) ~= ":" then fail("expected ':'") end
        position = position + 1
        object[key] = parse_value()
        skip_ws()
        local sep = text:sub(position, position)
        if sep == "," then position = position + 1
        elseif sep == "}" then position = position + 1; break
        else fail("expected ',' or '}'") end
      end
      return object
    end
    if char == "[" then
      position = position + 1
      local array = {}
      skip_ws()
      if text:sub(position, position) == "]" then position = position + 1; return array end
      while true do
        array[#array + 1] = parse_value()
        skip_ws()
        local sep = text:sub(position, position)
        if sep == "," then position = position + 1
        elseif sep == "]" then position = position + 1; break
        else fail("expected ',' or ']'") end
      end
      return array
    end
    if text:sub(position, position + 3) == "true" then position = position + 4; return true end
    if text:sub(position, position + 4) == "false" then position = position + 5; return false end
    if text:sub(position, position + 3) == "null" then position = position + 4; return nil end
    if char == "-" or char:match("%d") then return parse_number() end
    fail("unexpected character '" .. char .. "'")
  end

  local ok, value = pcall(function()
    skip_ws()
    local result = parse_value()
    skip_ws()
    if position <= length then fail("unexpected content after the JSON value") end
    return result
  end)
  if ok then return value end
  return nil, value
end

----------------------------------------------------------------------
-- SQLite-backed store
----------------------------------------------------------------------

local function db_path()
  local path = config.get("db_path")
  if path then return path end
  local folder = ui.pick_folder("Choose a folder for the JSON store database")
  if not folder then return nil end
  path = folder .. "\\tinkeredit-json-store.db"
  config.set("db_path", path)
  return path
end

-- Opens the database, makes sure the table exists, runs fn(db), then closes
-- it again - every command gets its own short-lived connection.
local function with_db(fn)
  local path = db_path()
  if not path then return end
  local db, openErr = sqlite.open(path)
  if not db then
    ui.message("Could not open " .. path .. ": " .. tostring(openErr), "JSON Store", "error")
    return
  end
  local ready, createErr = db:exec([[
    create table if not exists json_store (
      key text primary key,
      json text not null,
      updated text not null
    )
  ]])
  if ready then
    local ok, err = pcall(fn, db)
    if not ok then
      db:close()
      error(err)
    end
  else
    ui.message("Could not prepare the JSON store database: " .. tostring(createErr), "JSON Store", "error")
  end
  db:close()
end

local function source_text()
  local s, f = editor.get_selection_range()
  if f < s then return editor.get_text() end
  return editor.get_range(s, f)
end

local function list_keys(db)
  local rows, err = db:query("select key from json_store order by key")
  if not rows then
    ui.message("Could not list keys: " .. tostring(err), "JSON Store", "error")
    return nil
  end
  local keys = {}
  for _, row in ipairs(rows) do keys[#keys + 1] = row.key end
  return keys
end

local function choose_key(db, prompt)
  local keys = list_keys(db)
  if not keys then return nil end
  if #keys == 0 then
    ui.message("The JSON store is empty.", "JSON Store")
    return nil
  end
  local _, key = ui.choose(keys, prompt or "Key:", "JSON Store")
  return key
end

----------------------------------------------------------------------
-- Commands
----------------------------------------------------------------------

menu.add("/Tools/JSON Store/Save Selection as Key...", function()
  with_db(function(db)
    local text = source_text()
    local value, err = decode_json(text)
    if err then
      ui.message("That isn't valid JSON: " .. err, "JSON Store", "error")
      return
    end
    local json = encode_json(value)
    if not json then
      ui.message("Could not re-encode the decoded value.", "JSON Store", "error")
      return
    end
    local key = ui.prompt("Save as key:", "", "JSON Store")
    if not key or key == "" then return end
    local ok, saveErr = db:exec(
      "insert into json_store (key, json, updated) values (?, ?, ?) " ..
      "on conflict(key) do update set json = excluded.json, updated = excluded.updated",
      key, json, app.date_time())
    if not ok then ui.message("Could not save the key: " .. tostring(saveErr), "JSON Store", "error") end
  end)
end)

menu.add("/Tools/JSON Store/Insert Key at Caret...", function()
  with_db(function(db)
    local key = choose_key(db, "Insert which key?")
    if not key then return end
    local rows, err = db:query("select json from json_store where key = ?", key)
    if not rows or #rows == 0 then
      ui.message("Could not read that key: " .. tostring(err), "JSON Store", "error")
      return
    end
    local value, decodeErr = decode_json(rows[1].json)
    if decodeErr then
      ui.message("Stored JSON for '" .. key .. "' is corrupt: " .. decodeErr, "JSON Store", "error")
      return
    end
    editor.replace_selection(encode_json(value, true))
  end)
end)

menu.add("/Tools/JSON Store/View Key...", function()
  with_db(function(db)
    local key = choose_key(db, "View which key?")
    if not key then return end
    local rows, err = db:query("select json, updated from json_store where key = ?", key)
    if not rows or #rows == 0 then
      ui.message("Could not read that key: " .. tostring(err), "JSON Store", "error")
      return
    end
    local value, decodeErr = decode_json(rows[1].json)
    local body = decodeErr and ("(corrupt JSON: " .. decodeErr .. ")\n" .. rows[1].json)
      or encode_json(value, true)
    output.report("JSON Store: " .. key .. " (saved " .. rows[1].updated .. ")", body)
  end)
end)

menu.add("/Tools/JSON Store/List Keys", function()
  with_db(function(db)
    local rows, err = db:query("select key, updated from json_store order by key")
    if not rows then
      ui.message("Could not list keys: " .. tostring(err), "JSON Store", "error")
      return
    end
    if #rows == 0 then
      ui.message("The JSON store is empty.", "JSON Store")
      return
    end
    local lines = {}
    for _, row in ipairs(rows) do lines[#lines + 1] = row.key .. "  (saved " .. row.updated .. ")" end
    output.report("JSON Store", table.concat(lines, "\n"))
  end)
end)

menu.add("/Tools/JSON Store/Delete Key...", function()
  with_db(function(db)
    local key = choose_key(db, "Delete which key?")
    if not key then return end
    if not ui.confirm("Delete '" .. key .. "' from the JSON store?", "JSON Store") then return end
    local ok, err = db:exec("delete from json_store where key = ?", key)
    if not ok then ui.message("Could not delete the key: " .. tostring(err), "JSON Store", "error") end
  end)
end)

menu.add("/Tools/JSON Store/Choose Database File...", function()
  config.set("db_path", nil)
  local path = db_path()
  if path then ui.message("JSON store database: " .. path, "JSON Store") end
end)
