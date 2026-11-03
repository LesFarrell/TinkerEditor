-- Bracket/brace matching for the whole document, on Tools > Check Brackets:
--   Check Now                      scans the document and reports every
--                                  unmatched or mismatched (), [], {} - and,
--                                  for languages whose lexer flags it, a
--                                  string left without its closing quote -
--                                  to the Problems panel
--   Check as You Type (on by       rechecks after each edit, and when a
--   default)                       document is opened or switched to
--   Clear Problems                 removes this plugin's reports
--
-- Scintilla's own brace match (Ctrl+]) only looks at the brace under the
-- caret; this looks at the whole file at once, so a brace opened on line 3
-- and never closed shows up without having to go hunting for it.
--
-- Brackets inside a string or comment don't count - for every bundled
-- language except batch and makefile (bracket-light languages left
-- unclassified; nothing in them is checked, rather than risk false alarms
-- from an unclassified style). Unterminated-string detection relies on the
-- lexer's own end-of-line string style, which only some lexers emit; where
-- the active lexer doesn't, only the bracket check still runs.
--
-- Uses only the plugin API documented in README.md in this folder.

local SOURCE = "Bracket Checker"
local LIVE_LIMIT = 1024 * 1024

-- Each language's skip set names every lexer style that's inside a string or
-- comment (brackets there don't count); its unterminated set names the
-- style the lexer gives the rest of a line once a string runs off the end
-- without being closed. Numbers are Scintilla/Lexilla style constants for
-- the lexer editor_language.c assigns that language (see SciLexer.h);
-- unlisted languages get no classification at all.
local LANGUAGE_STYLES = {
  cpp = {
    skip = { [1] = true, [2] = true, [3] = true, [15] = true, [6] = true, [7] = true,
              [12] = true, [13] = true },
    unterminated = { [12] = true },
  },
  lua = {
    skip = { [1] = true, [2] = true, [3] = true, [6] = true, [7] = true, [8] = true, [12] = true },
    unterminated = { [12] = true },
  },
  python = {
    skip = { [1] = true, [12] = true, [3] = true, [4] = true, [6] = true, [7] = true,
              [13] = true, [16] = true },
    unterminated = { [13] = true },
  },
  json = {
    skip = { [6] = true, [7] = true, [2] = true, [3] = true },
    unterminated = { [3] = true },
  },
  html = { skip = { [6] = true, [7] = true, [19] = true, [9] = true, [17] = true } },
  xml = { skip = { [6] = true, [7] = true, [19] = true, [9] = true, [17] = true } },
  bash = { skip = { [2] = true, [5] = true, [6] = true, [11] = true, [13] = true } },
  powershell = { skip = { [1] = true, [13] = true, [2] = true, [3] = true } },
  css = { skip = { [9] = true, [13] = true, [14] = true } },
  sql = { skip = { [1] = true, [2] = true, [3] = true, [6] = true, [7] = true } },
  yaml = { skip = { [1] = true } },
  ini = { skip = { [1] = true } },
  toml = {
    skip = { [1] = true, [9] = true, [10] = true, [11] = true, [12] = true, [15] = true },
    unterminated = { [15] = true },
  },
}

local OPENERS = { ["("] = ")", ["["] = "]", ["{"] = "}" }
local NAMES = { ["("] = "parenthesis", [")"] = "parenthesis",
               ["["] = "bracket", ["]"] = "bracket",
               ["{"] = "brace", ["}"] = "brace" }

local function location(line)
  local path = editor.file_path()
  return path and string.format("%s:%d", path, line) or string.format("Line %d", line)
end

-- Scans the whole document, skipping brackets whose style is in `skip`
-- (nil means nothing is skipped - no classification for this language), and
-- returns a list of "location: message" issues.
local function scan_brackets(skip)
  local text = editor.get_text()
  local issues, stack = {}, {}
  local at = 1
  while true do
    local found = text:find("[%(%)%[%]{}]", at)
    if not found then break end
    local char = text:sub(found, found)
    at = found + 1
    local position = found
    if not (skip and skip[editor.style_at(position)]) then
      if OPENERS[char] then
        stack[#stack + 1] = { char = char, position = position }
      else
        local top = stack[#stack]
        if not top then
          issues[#issues + 1] = location(editor.line_at(position)) ..
            string.format(": Unmatched closing %s '%s'.", NAMES[char], char)
        elseif OPENERS[top.char] ~= char then
          issues[#issues + 1] = location(editor.line_at(position)) .. string.format(
            ": Closing %s '%s' doesn't match the '%s' opened on line %d; expected '%s'.",
            NAMES[char], char, top.char, editor.line_at(top.position), OPENERS[top.char])
          stack[#stack] = nil
        else
          stack[#stack] = nil
        end
      end
    end
  end
  for i = #stack, 1, -1 do
    local open = stack[i]
    issues[#issues + 1] = location(editor.line_at(open.position)) ..
      string.format(": Unmatched opening %s '%s'.", NAMES[open.char], open.char)
  end
  return issues
end

-- Appends one issue per line whose lexer style says a string ran off its
-- end without being closed; does nothing when the language has no such
-- style (`unterminated` is nil).
local function scan_unterminated_strings(unterminated, issues)
  if not unterminated then return end
  for line = 1, editor.line_count() do
    local start, finish = editor.line_range(line)
    if finish >= start and unterminated[editor.style_at(finish)] then
      issues[#issues + 1] = location(line) .. ": Unterminated string (missing closing quote)."
    end
  end
end

local function check()
  local info = LANGUAGE_STYLES[editor.language()]
  local issues = scan_brackets(info and info.skip)
  scan_unterminated_strings(info and info.unterminated, issues)
  if #issues == 0 then problems.clear(SOURCE)
  else problems.report(SOURCE, table.concat(issues, "\n")) end
  return #issues
end

local function check_now()
  local count = check()
  ui.status(count == 0 and "No bracket problems found." or
    (count .. " bracket problem" .. (count == 1 and "" or "s") .. " found."))
end

local function live_check()
  if config.get("live", true) and editor.length() <= LIVE_LIMIT then check()
  else problems.clear(SOURCE) end
end

menu.add("/Tools/Check Brackets/Check &Now", check_now)
menu.add("/Tools/Check Brackets/&Clear Problems", function() problems.clear(SOURCE) end)
menu.checkbox("/Tools/Check Brackets/Check as &You Type", "live", true, function(checked)
  if checked then live_check() else problems.clear(SOURCE) end
end)

events.on("text_changed", live_check)
events.on("open", live_check)
events.on("tab_changed", live_check)
