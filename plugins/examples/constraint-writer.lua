-- Writing challenges with persistent rules and optional live highlighting.
-- Checks are intentionally simple and explainable; adverb detection is a heuristic.
-- Check Constraints reports in the output panel.

local FORBIDDEN_INDICATOR = 14
local SENTENCE_INDICATOR = 15
local STYLE_INDICATOR = 16

-- Live highlighting skips documents larger than this (about 40,000 words of
-- prose), so typing in a very large file stays responsive; the commands
-- still handle them.
local LIVE_LIMIT = 256 * 1024

-- Bytes 128-255 count as word characters so non-ASCII letters work, which
-- would include common UTF-8 punctuation: curly quotes, en/em dashes, the
-- ellipsis and no-break space. Those become spaces of the same byte length,
-- so word positions still match the text.
local function mask_punctuation(text)
  return (text:gsub("\226\128[\147\148\152\153\156\157\166]", "   "):gsub("\194\160", "  "))
end

-- Words are held as parallel arrays (key[i], first[i], last[i], with n
-- words in all) rather than one table per word: a table costs far more
-- than the few fields it would hold, and at one table per word that
-- overhead alone could run past the plugin memory limit on a document of
-- only a megabyte or two of prose.
local function words_in(text, offset)
  local words = { key = {}, first = {}, last = {}, n = 0 }
  offset = offset or 0
  for first, word, after in mask_punctuation(text):gmatch("()([A-Za-z0-9_\128-\255]+)()") do
    local n = words.n + 1
    words.key[n] = word:lower()
    words.first[n] = offset + first
    words.last[n] = offset + after - 1
    words.n = n
  end
  return words
end

-- Partitions the document's words (already computed once by words_in, and
-- passed in rather than rescanned here) into sentences by walking them in
-- order alongside the sentence-ending punctuation, recording each
-- sentence's share of `words` as a first/last index pair rather than a
-- copy of the words themselves - a word can't straddle a sentence
-- boundary, since sentences split on punctuation that's never itself a
-- word character.
local function sentence_ranges(text, offset, words)
  local ranges, position, wordIndex = {}, 1, 1
  offset = offset or 0
  local function take_through(last)
    local start = wordIndex
    while wordIndex <= words.n and words.last[wordIndex] <= last do
      wordIndex = wordIndex + 1
    end
    if wordIndex > start then
      ranges[#ranges + 1] = {
        first = words.first[start], last = last, wordFirst = start, wordLast = wordIndex - 1
      }
    end
  end
  while position <= #text do
    local punctuation = text:find("[.!?]", position)
    if not punctuation then break end
    local finish = punctuation
    while finish < #text and text:sub(finish + 1, finish + 1):match("[.!?]") do
      finish = finish + 1
    end
    take_through(offset + finish)
    position = finish + 1
  end
  take_through(offset + #text)
  return ranges
end

local function forbidden_words()
  local result = {}
  for word in mask_punctuation(config.get("forbidden_words", "")):gmatch("[A-Za-z0-9_\128-\255]+") do
    result[word:lower()] = true
  end
  return result
end

local adverb_exceptions = {
  ally = true, early = true, family = true, friendly = true, likely = true,
  lively = true, lonely = true, lovely = true, only = true, reply = true,
  silly = true, ugly = true
}

local function likely_adverb(word)
  return #word >= 5 and word:sub(-2) == "ly" and not adverb_exceptions[word]
end

local function inspect(text, offset)
  local words = words_in(text, offset)
  local sentences = sentence_ranges(text, offset, words)
  local forbidden = forbidden_words()
  local letter = config.get("forbidden_letter", ""):lower()
  local maximum = config.get("max_sentence_words", 20)
  local no_adverbs = config.get("no_adverbs", false)
  local unique_starts = config.get("unique_starts", false)

  local result = {
    word_count = words.n,
    forbidden_words = {},
    forbidden_letters = {},
    long_sentences = {},
    adverbs = {},
    repeated_starts = {},
    repeated_start_kinds = 0
  }

  for index = 1, words.n do
    local key = words.key[index]
    if forbidden[key] or (no_adverbs and likely_adverb(key)) then
      local entry = { first = words.first[index], last = words.last[index] }
      if forbidden[key] then result.forbidden_words[#result.forbidden_words + 1] = entry end
      if no_adverbs and likely_adverb(key) then result.adverbs[#result.adverbs + 1] = entry end
    end
  end

  if letter ~= "" then
    for position = 1, #text do
      if text:sub(position, position):lower() == letter then
        result.forbidden_letters[#result.forbidden_letters + 1] = {
          first = offset + position,
          last = offset + position
        }
      end
    end
  end

  if maximum > 0 then
    for _, sentence in ipairs(sentences) do
      if sentence.wordLast - sentence.wordFirst + 1 > maximum then
        result.long_sentences[#result.long_sentences + 1] = sentence
      end
    end
  end

  if unique_starts then
    local counts = {}
    for _, sentence in ipairs(sentences) do
      local key = words.key[sentence.wordFirst]
      counts[key] = (counts[key] or 0) + 1
    end
    local kinds = {}
    for _, sentence in ipairs(sentences) do
      local key = words.key[sentence.wordFirst]
      if counts[key] > 1 then
        result.repeated_starts[#result.repeated_starts + 1] =
          { first = words.first[sentence.wordFirst], last = words.last[sentence.wordFirst] }
        kinds[key] = true
      end
    end
    for _ in pairs(kinds) do result.repeated_start_kinds = result.repeated_start_kinds + 1 end
  end

  return result
end

local function clear_highlights()
  editor.clear_indicator(FORBIDDEN_INDICATOR)
  editor.clear_indicator(SENTENCE_INDICATOR)
  editor.clear_indicator(STYLE_INDICATOR)
end

local function apply_highlights(result)
  clear_highlights()
  for _, entry in ipairs(result.forbidden_words) do
    editor.indicator(entry.first, entry.last, FORBIDDEN_INDICATOR, 0xef5350)
  end
  for _, entry in ipairs(result.forbidden_letters) do
    editor.indicator(entry.first, entry.last, FORBIDDEN_INDICATOR, 0xef5350)
  end
  for _, sentence in ipairs(result.long_sentences) do
    editor.indicator(sentence.first, sentence.last, SENTENCE_INDICATOR, 0xffa726)
  end
  for _, entry in ipairs(result.adverbs) do
    editor.indicator(entry.first, entry.last, STYLE_INDICATOR, 0xab47bc)
  end
  for _, entry in ipairs(result.repeated_starts) do
    editor.indicator(entry.first, entry.last, STYLE_INDICATOR, 0xab47bc)
  end
end

local function highlight_document()
  apply_highlights(inspect(editor.get_text(), 0))
end

local function live_highlight()
  if editor.length() <= LIVE_LIMIT then highlight_document() else clear_highlights() end
end

local function refresh_if_live()
  if config.get("live_highlighting", false) then live_highlight() end
end

local function plural(count, singular, plural_form)
  return count == 1 and singular or (plural_form or singular .. "s")
end

local function report_line(label, count, detail)
  return string.format("%s: %d %s", label, count, plural(count, detail))
end

local function check_constraints()
  local selected = editor.get_selection()
  local start = 1
  if selected ~= "" then start = editor.get_selection_range() end
  local text = selected ~= "" and selected or editor.get_text()
  local result = inspect(text, start - 1)
  local lines = {
    (selected ~= "" and "Selection" or "Document") .. " constraint check",
    "Words: " .. result.word_count,
    ""
  }
  local failures = 0
  local forbidden = config.get("forbidden_words", "")
  local letter = config.get("forbidden_letter", "")
  local target = config.get("word_target", 0)
  local maximum = config.get("max_sentence_words", 20)

  if forbidden ~= "" then
    local count = #result.forbidden_words
    lines[#lines + 1] = report_line("Forbidden words", count, "occurrence")
    failures = failures + count
  end
  if letter ~= "" then
    local count = #result.forbidden_letters
    lines[#lines + 1] = string.format("Forbidden letter %s: %d %s", letter:upper(), count,
      plural(count, "occurrence"))
    failures = failures + count
  end
  if target > 0 then
    local difference = result.word_count - target
    if difference == 0 then
      lines[#lines + 1] = "Word target: exactly " .. target .. " — achieved"
    else
      lines[#lines + 1] = string.format("Word target: exactly %d — %d word%s %s", target,
        math.abs(difference), math.abs(difference) == 1 and "" or "s",
        difference < 0 and "short" or "over")
      failures = failures + 1
    end
  end
  if maximum > 0 then
    local count = #result.long_sentences
    lines[#lines + 1] = string.format("Sentence limit (%d words): %d too long", maximum, count)
    failures = failures + count
  end
  if config.get("no_adverbs", false) then
    local count = #result.adverbs
    lines[#lines + 1] = report_line("Likely adverbs", count, "word")
    failures = failures + count
  end
  if config.get("unique_starts", false) then
    local count = #result.repeated_starts
    lines[#lines + 1] = string.format("Repeated openings: %d sentence%s across %d word%s", count,
      count == 1 and "" or "s", result.repeated_start_kinds,
      result.repeated_start_kinds == 1 and "" or "s")
    failures = failures + count
  end
  if failures == 0 then lines[#lines + 1] = "All active constraints are satisfied." end
  local report = table.concat(lines, "\n")
  if failures > 0 then problems.report("Constraint Writer", report)
  else problems.clear("Constraint Writer") end
  output.report("Constraint Writer", report)
end

local function set_number(key, label, default, maximum)
  local current = config.get(key, default)
  local answer = ui.prompt(label .. " (leave blank to disable):",
    current > 0 and tostring(current) or "")
  if answer == nil then return end
  if answer:match("^%s*$") then
    config.set(key, 0)
    refresh_if_live()
    return
  end
  local value = tonumber(answer)
  if not value or value % 1 ~= 0 or value < 1 or value > maximum then
    ui.message("Enter a whole number from 1 to " .. maximum .. ", or leave it blank.",
      nil, "warning")
    return
  end
  config.set(key, value)
  refresh_if_live()
end

menu.add("Constraint Writer/Check Constraints", check_constraints)
menu.add("Constraint Writer/Highlight Issues", highlight_document)
menu.add("Constraint Writer/Clear Highlights", clear_highlights)
menu.separator("Constraint Writer")

menu.add("Constraint Writer/Rules/Forbidden Words...", function()
  local answer = ui.prompt("Words to avoid (separate with commas or spaces):",
    config.get("forbidden_words", ""))
  if answer == nil then return end
  config.set("forbidden_words", answer:match("^%s*(.-)%s*$"))
  refresh_if_live()
end)

menu.add("Constraint Writer/Rules/Forbidden Letter...", function()
  local answer = ui.prompt("Letter to avoid (leave blank to disable):",
    config.get("forbidden_letter", ""))
  if answer == nil then return end
  answer = answer:match("^%s*(.-)%s*$")
  if answer ~= "" and (#answer ~= 1 or not answer:match("[A-Za-z]")) then
    ui.message("Enter one ASCII letter, or leave it blank.", nil, "warning")
    return
  end
  config.set("forbidden_letter", answer:lower())
  refresh_if_live()
end)

menu.add("Constraint Writer/Rules/Exact Word Target...", function()
  set_number("word_target", "Exact word target", 0, 1000000)
end)

menu.add("Constraint Writer/Rules/Maximum Sentence Length...", function()
  set_number("max_sentence_words", "Maximum words per sentence", 20, 1000)
end)

menu.checkbox("Constraint Writer/Rules/Avoid Likely Adverbs", "no_adverbs", false, function()
  refresh_if_live()
end)

menu.checkbox("Constraint Writer/Rules/Unique Sentence Openings", "unique_starts", false, function()
  refresh_if_live()
end)

menu.separator("Constraint Writer")

menu.checkbox("Constraint Writer/Live Highlighting", "live_highlighting", false, function(checked)
  if checked then live_highlight() else clear_highlights() end
end)

events.on("text_changed", refresh_if_live)
events.on("open", refresh_if_live)
events.on("tab_changed", refresh_if_live)

