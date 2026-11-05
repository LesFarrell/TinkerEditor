-- YAML Store: saves named YAML documents in a SQLite file via the `sqlite`
-- API (see plugins/README.md#sqlite) and reads them back. Each plugin has
-- its own private environment (see "The Lua environment" in the plugin
-- guide), so this carries its own small YAML codec rather than sharing one
-- with json-store.lua or json-tools.lua.
--
-- This reads a practical subset of YAML - block mappings and sequences,
-- flow collections ([...] / {...}, parsed as JSON), double/single-quoted
-- and plain scalars, comments - and does not support block scalars (| or
-- >), tags, anchors, aliases, merge keys, or multiple documents in one
-- file; json-tools.lua's YAML support documents the same limits. To avoid
-- YAML's many plain-scalar quoting hazards (a bare value starting with a
-- symbol character, "no"/"yes" as booleans in some YAML variants, and so
-- on), everything this plugin writes back out double-quotes every string,
-- which stays entirely unambiguous to read back in. Like most small JSON/
-- YAML libraries, a Lua table has no way to hold a "null" entry, so a null
-- read from the middle of a mapping or sequence becomes a missing key or a
-- renumbered array hole on round trip.

----------------------------------------------------------------------
-- A small JSON decoder, used for double-quoted scalars and flow ([]/{})
-- collections, which are valid JSON wherever YAML allows them.
----------------------------------------------------------------------

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
    if position > length then fail("unexpected end of text") end
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
    if position <= length then fail("unexpected content after the value") end
    return result
  end)
  if ok then return value end
  return nil, value
end

local function encode_json_string(s)
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

----------------------------------------------------------------------
-- Decode (YAML text -> Lua value)
----------------------------------------------------------------------

-- content ends just before a top-level, unquoted ":" followed by a space
-- or end of line - the same rule a YAML mapping key/value split uses.
local function strip_yaml_comment(text)
  local quote, escaped, position = nil, false, 1
  while position <= #text do
    local character = text:sub(position, position)
    if quote == '"' then
      if escaped then escaped = false
      elseif character == "\\" then escaped = true
      elseif character == quote then quote = nil end
    elseif quote == "'" then
      if character == "'" and text:sub(position + 1, position + 1) == "'" then position = position + 1
      elseif character == "'" then quote = nil end
    elseif character == '"' or character == "'" then
      quote = character
    elseif character == "#" and (position == 1 or text:sub(position - 1, position - 1):match("%s")) then
      return text:sub(1, position - 1)
    end
    position = position + 1
  end
  return text
end

local function yaml_key_value(content)
  local quote, escaped, position = nil, false, 1
  while position <= #content do
    local character = content:sub(position, position)
    if quote == '"' then
      if escaped then escaped = false
      elseif character == "\\" then escaped = true
      elseif character == quote then quote = nil end
    elseif quote == "'" then
      if character == "'" and content:sub(position + 1, position + 1) == "'" then position = position + 1
      elseif character == "'" then quote = nil end
    elseif character == '"' or character == "'" then
      quote = character
    elseif character == ":" then
      local next_character = content:sub(position + 1, position + 1)
      if next_character == "" or next_character:match("%s") then
        return content:sub(1, position - 1):match("^%s*(.-)%s*$"),
          content:sub(position + 1):match("^%s*(.-)%s*$")
      end
    end
    position = position + 1
  end
end

-- Returns ok, valueOrErrorMessage. A successful null is ok=true, value=nil.
local function scalar(raw)
  if raw:match("^[|>][%+%-]?$") then
    return false, "block scalars ('|' or '>') are not supported; use a quoted string"
  end
  if raw:match("^[&*!]") or raw:find("<<:", 1, true) then
    return false, "YAML tags, anchors, aliases, and merge keys are not supported"
  end
  if raw:sub(1, 1) == '"' or raw:sub(1, 1) == "[" or raw:sub(1, 1) == "{" then
    local value, err = decode_json(raw)
    if err then return false, err end
    return true, value
  end
  if raw:sub(1, 1) == "'" then
    if raw:sub(-1) ~= "'" or #raw < 2 then return false, "unterminated single-quoted string" end
    local inner = raw:sub(2, -2)
    local probe = inner:gsub("''", "")
    if probe:find("'", 1, true) then return false, "a single quote inside a YAML string must be doubled" end
    return true, (inner:gsub("''", "'"))
  end
  local lower = raw:lower()
  if lower == "null" or raw == "~" then return true, nil end
  if lower == "true" then return true, true end
  if lower == "false" then return true, false end
  if raw:match("^[%-%+]?%d+%.?%d*$") or raw:match("^[%-%+]?%.%d+$") then
    return true, tonumber(raw)
  end
  return true, raw
end

-- Returns the decoded value (nil for an empty/null document), or nil, err
-- on a syntax error - check err, not the value, to tell the two apart.
local function decode_yaml(text)
  if utf8.len(text) == nil then return nil, "invalid UTF-8 byte" end
  local lines, position, lineNumber = {}, 1, 1
  while position <= #text + 1 do
    local newline = text:find("\n", position, true)
    local finish = newline and newline - 1 or #text
    local raw = text:sub(position, finish)
    if raw:sub(-1) == "\r" then raw = raw:sub(1, -2) end
    if raw:match("^ *\t") then return nil, "line " .. lineNumber .. ": tabs cannot be used for indentation" end
    local withoutComment = strip_yaml_comment(raw):gsub("%s+$", "")
    if not withoutComment:match("^%s*$") then
      local spaces = #(withoutComment:match("^ *") or "")
      lines[#lines + 1] = { indent = spaces, content = withoutComment:sub(spaces + 1), line = lineNumber }
    end
    if not newline then break end
    position, lineNumber = newline + 1, lineNumber + 1
  end

  if #lines > 0 and lines[1].content == "---" then table.remove(lines, 1) end
  if #lines > 0 and lines[#lines].content == "..." then table.remove(lines) end
  if #lines == 0 then return nil end -- an empty document decodes as null
  for _, line in ipairs(lines) do
    if line.content == "---" or line.content == "..." then
      return nil, "line " .. line.line .. ": multiple YAML documents are not supported"
    end
  end

  local function err_at(line, message) return false, "line " .. line.line .. ": " .. message end

  local parse_block
  local function parse_mapping(index, indent, firstContent, firstLine)
    local object, seen = {}, {}
    local content, line = firstContent, firstLine
    while content do
      local keyText, valueText = yaml_key_value(content)
      if not keyText or keyText == "" then return err_at(line, "expected a mapping key followed by ':'") end
      local key = keyText
      if keyText:sub(1, 1) == '"' or keyText:sub(1, 1) == "'" then
        local ok, decoded = scalar(keyText)
        if not ok then return err_at(line, "invalid quoted mapping key: " .. decoded) end
        if type(decoded) ~= "string" then return err_at(line, "a mapping key must be a string") end
        key = decoded
      end
      if seen[key] then return err_at(line, "duplicate mapping key '" .. key .. "'") end
      seen[key] = true
      local ok, value
      if valueText == "" then
        if index <= #lines and lines[index].indent > indent then
          ok, value, index = parse_block(index, lines[index].indent)
          if not ok then return false, value end
        elseif index <= #lines and lines[index].indent == indent
            and (lines[index].content == "-" or lines[index].content:match("^%-%s")) then
          ok, value, index = parse_block(index, indent)
          if not ok then return false, value end
        else
          ok, value = true, nil
        end
      else
        ok, value = scalar(valueText)
        if not ok then return err_at(line, value) end
      end
      object[key] = value
      if index > #lines or lines[index].indent ~= indent then break end
      content, line = lines[index].content, lines[index]
      if content:match("^%-%s") or content == "-" then break end
      index = index + 1
    end
    return true, object, index
  end

  parse_block = function(index, indent)
    local first = lines[index]
    if not first or first.indent ~= indent then
      return err_at(first or lines[#lines], "inconsistent indentation")
    end
    if first.content == "-" or first.content:match("^%-%s") then
      local array = {}
      while index <= #lines and lines[index].indent == indent do
        local line = lines[index]
        if line.content ~= "-" and not line.content:match("^%-%s") then break end
        local rest = line.content == "-" and "" or line.content:sub(2):match("^%s+(.-)%s*$")
        index = index + 1
        local ok, value
        if rest == "" then
          if index <= #lines and lines[index].indent > indent then
            ok, value, index = parse_block(index, lines[index].indent)
            if not ok then return false, value end
          else
            ok, value = true, nil
          end
        elseif yaml_key_value(rest) then
          ok, value, index = parse_mapping(index, indent + #line.content:match("^%-%s*"), rest, line)
          if not ok then return false, value end
        else
          ok, value = scalar(rest)
          if not ok then return err_at(line, value) end
          if index <= #lines and lines[index].indent > indent then
            return err_at(lines[index], "a scalar sequence item cannot have nested content")
          end
        end
        array[#array + 1] = value
      end
      return true, array, index
    end
    local key = yaml_key_value(first.content)
    if key then return parse_mapping(index + 1, indent, first.content, first) end
    local ok, value = scalar(first.content)
    if not ok then return err_at(first, value) end
    return true, value, index + 1
  end

  local ok, root, nextIndex = parse_block(1, lines[1].indent)
  if not ok then return nil, root end
  if nextIndex <= #lines then
    local _, message = err_at(lines[nextIndex], "unexpected indentation or content")
    return nil, message
  end
  return root
end

----------------------------------------------------------------------
-- Encode (Lua value -> block-style YAML text, sorted keys)
----------------------------------------------------------------------

local function encode_scalar(value)
  if value == nil then return "null" end
  local kind = type(value)
  if kind == "boolean" then return value and "true" or "false" end
  if kind == "number" then
    if value ~= value or value == math.huge or value == -math.huge then
      error("cannot represent a non-finite number in YAML")
    end
    return (math.type(value) == "integer") and tostring(value) or string.format("%.17g", value)
  end
  if kind == "string" then return encode_json_string(value) end
  error("cannot encode a " .. kind .. " value as YAML")
end

local encode_block -- forward declaration (mutually recursive with the two below)

local function encode_mapping(value, indent, lines)
  local keys = {}
  for key in pairs(value) do
    if type(key) ~= "string" then error("YAML mapping keys must be strings") end
    keys[#keys + 1] = key
  end
  table.sort(keys)
  for _, key in ipairs(keys) do
    local nested = value[key]
    local keyText = encode_json_string(key)
    if type(nested) == "table" and next(nested) ~= nil then
      lines[#lines + 1] = indent .. keyText .. ":"
      encode_block(nested, indent .. "  ", lines)
    elseif type(nested) == "table" then
      lines[#lines + 1] = indent .. keyText .. ": " .. (is_array(nested) and "[]" or "{}")
    else
      lines[#lines + 1] = indent .. keyText .. ": " .. encode_scalar(nested)
    end
  end
end

local function encode_sequence(value, indent, lines)
  for _, item in ipairs(value) do
    if type(item) == "table" and next(item) ~= nil then
      lines[#lines + 1] = indent .. "-"
      encode_block(item, indent .. "  ", lines)
    elseif type(item) == "table" then
      lines[#lines + 1] = indent .. "- " .. (is_array(item) and "[]" or "{}")
    else
      lines[#lines + 1] = indent .. "- " .. encode_scalar(item)
    end
  end
end

encode_block = function(value, indent, lines)
  if is_array(value) then encode_sequence(value, indent, lines)
  else encode_mapping(value, indent, lines) end
end

-- Returns the YAML text, or nil, err if value contains something YAML
-- (as this codec represents it) can't hold.
local function encode_yaml(value)
  if type(value) ~= "table" then
    local ok, result = pcall(encode_scalar, value)
    if ok then return result end
    return nil, result
  end
  if next(value) == nil then return is_array(value) and "[]" or "{}" end
  local lines = {}
  local ok, err = pcall(encode_block, value, "", lines)
  if not ok then return nil, err end
  return table.concat(lines, "\n")
end

----------------------------------------------------------------------
-- SQLite-backed store
----------------------------------------------------------------------

local function db_path()
  local path = config.get("db_path")
  if path then return path end
  local folder = ui.pick_folder("Choose a folder for the YAML store database")
  if not folder then return nil end
  path = folder .. "\\tinkeredit-yaml-store.db"
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
    ui.message("Could not open " .. path .. ": " .. tostring(openErr), "YAML Store", "error")
    return
  end
  local ready, createErr = db:exec([[
    create table if not exists yaml_store (
      key text primary key,
      yaml text not null,
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
    ui.message("Could not prepare the YAML store database: " .. tostring(createErr), "YAML Store", "error")
  end
  db:close()
end

local function source_text()
  local s, f = editor.get_selection_range()
  if f < s then return editor.get_text() end
  return editor.get_range(s, f)
end

local function choose_key(db, prompt)
  local rows, err = db:query("select key from yaml_store order by key")
  if not rows then
    ui.message("Could not list keys: " .. tostring(err), "YAML Store", "error")
    return nil
  end
  if #rows == 0 then
    ui.message("The YAML store is empty.", "YAML Store")
    return nil
  end
  local keys = {}
  for _, row in ipairs(rows) do keys[#keys + 1] = row.key end
  local _, key = ui.choose(keys, prompt or "Key:", "YAML Store")
  return key
end

----------------------------------------------------------------------
-- Commands
----------------------------------------------------------------------

menu.add("/Tools/YAML Store/Save Selection as Key...", function()
  with_db(function(db)
    local value, err = decode_yaml(source_text())
    if err then
      ui.message("That isn't valid YAML: " .. err, "YAML Store", "error")
      return
    end
    local yaml, encodeErr = encode_yaml(value)
    if not yaml then
      ui.message("Could not re-encode the decoded value: " .. tostring(encodeErr), "YAML Store", "error")
      return
    end
    local key = ui.prompt("Save as key:", "", "YAML Store")
    if not key or key == "" then return end
    local ok, saveErr = db:exec(
      "insert into yaml_store (key, yaml, updated) values (?, ?, ?) " ..
      "on conflict(key) do update set yaml = excluded.yaml, updated = excluded.updated",
      key, yaml, app.date_time())
    if not ok then ui.message("Could not save the key: " .. tostring(saveErr), "YAML Store", "error") end
  end)
end)

menu.add("/Tools/YAML Store/Insert Key at Caret...", function()
  with_db(function(db)
    local key = choose_key(db, "Insert which key?")
    if not key then return end
    local rows, err = db:query("select yaml from yaml_store where key = ?", key)
    if not rows or #rows == 0 then
      ui.message("Could not read that key: " .. tostring(err), "YAML Store", "error")
      return
    end
    local value, decodeErr = decode_yaml(rows[1].yaml)
    if decodeErr then
      ui.message("Stored YAML for '" .. key .. "' is corrupt: " .. decodeErr, "YAML Store", "error")
      return
    end
    editor.replace_selection((encode_yaml(value)))
  end)
end)

menu.add("/Tools/YAML Store/View Key...", function()
  with_db(function(db)
    local key = choose_key(db, "View which key?")
    if not key then return end
    local rows, err = db:query("select yaml, updated from yaml_store where key = ?", key)
    if not rows or #rows == 0 then
      ui.message("Could not read that key: " .. tostring(err), "YAML Store", "error")
      return
    end
    output.report("YAML Store: " .. key .. " (saved " .. rows[1].updated .. ")", rows[1].yaml)
  end)
end)

menu.add("/Tools/YAML Store/List Keys", function()
  with_db(function(db)
    local rows, err = db:query("select key, updated from yaml_store order by key")
    if not rows then
      ui.message("Could not list keys: " .. tostring(err), "YAML Store", "error")
      return
    end
    if #rows == 0 then
      ui.message("The YAML store is empty.", "YAML Store")
      return
    end
    local lines = {}
    for _, row in ipairs(rows) do lines[#lines + 1] = row.key .. "  (saved " .. row.updated .. ")" end
    output.report("YAML Store", table.concat(lines, "\n"))
  end)
end)

menu.add("/Tools/YAML Store/Delete Key...", function()
  with_db(function(db)
    local key = choose_key(db, "Delete which key?")
    if not key then return end
    if not ui.confirm("Delete '" .. key .. "' from the YAML store?", "YAML Store") then return end
    local ok, err = db:exec("delete from yaml_store where key = ?", key)
    if not ok then ui.message("Could not delete the key: " .. tostring(err), "YAML Store", "error") end
  end)
end)

menu.add("/Tools/YAML Store/Choose Database File...", function()
  config.set("db_path", nil)
  local path = db_path()
  if path then ui.message("YAML store database: " .. path, "YAML Store") end
end)
