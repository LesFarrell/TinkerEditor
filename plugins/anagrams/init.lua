-- Anagram solving and creation. On Plugins > Anagrams (British English):
--   Solve Anagram           single words that use every letter
--   Words From Letters      every word the letters can spell
--   Create Anagram Phrases  phrases of up to eight words that use every letter
--   Scramble Words          jumbles each selected word into a puzzle
--
-- The commands work on the selection, or ask for letters when nothing is
-- selected; a chosen result replaces the selection.
--
-- When View > Spell Check Language is English, or there's no spelling
-- dictionary, words come from words_en_gb.lua in this folder: British
-- English from SCOWL (see COPYING), so any number of letters is solved at
-- once.
--
-- For other languages the Windows spelling dictionary is used. It can say
-- whether a word is in it but can't list the words, so arrangements of the
-- letters are generated here and checked in batches. Checking takes time, so
-- every search has a budget: it stops at CHECK_LIMIT candidates, or when
-- about TIME_BUDGET seconds have passed, comfortably inside the 5 second
-- limit on a command.

local CHECK_LIMIT = 150000
local BATCH = 5000
local TIME_BUDGET = 3
local MAX_PHRASE_WORDS = 8
local MAX_PHRASES = 5000
-- Keep enough headroom for long names and phrases while retaining a guard
-- against accidentally submitting an unbounded search to the plugin host.
local MAX_LETTERS = 256

local function configured_number(key, default, minimum, maximum)
  local value = tonumber(config.get(key, default)) or default
  return math.max(minimum, math.min(maximum, math.floor(value)))
end

local function max_letters() return configured_number("max_letters", MAX_LETTERS, 2, 2000) end
local function check_limit() return configured_number("check_limit", CHECK_LIMIT, 1000, 1000000) end
local function time_budget() return configured_number("time_budget", TIME_BUDGET, 1, 5) end
local function max_phrases() return configured_number("max_phrases", MAX_PHRASES, 100, 10000) end

-- The word list has every letter and many rare two-letter words, which
-- would crowd English phrases, so phrases use only these short words.
local SHORT_ENGLISH = {
  "a", "i", "am", "an", "as", "at", "be", "by", "do", "go", "he", "hi", "if",
  "in", "is", "it", "me", "my", "no", "of", "oh", "on", "or", "ox", "so", "to",
  "up", "us", "we"
}

-- ---- letters ----

-- The letters of text, lower-cased: ASCII letters and any character beyond
-- ASCII that isn't punctuation or a symbol (Lua can only lower-case ASCII).
local function letters_of(text)
  local letters = {}
  for _, code in utf8.codes(text) do
    if code < 128 then
      if string.char(code):match("%a") then letters[#letters + 1] = string.char(code):lower() end
    elseif code > 0xbf and code ~= 0xd7 and code ~= 0xf7 and (code < 0x2000 or code > 0x206f) then
      letters[#letters + 1] = utf8.char(code)
    end
  end
  return letters
end

-- Longest first by letters, not bytes, then alphabetically.
local function longest_first(a, b)
  local a_length, b_length = utf8.len(a), utf8.len(b)
  if a_length ~= b_length then return a_length > b_length end
  return a < b
end

-- Curly quotes, the curly apostrophe, en/em dashes, the ellipsis and the
-- no-break space become spaces of the same byte length, so they aren't
-- taken for letters and word positions still match the text.
local function mask_punctuation(text)
  return (text:gsub("\226\128[\147\148\152\153\156\157\166]", "   "):gsub("\194\160", "  "))
end

-- A multiset of letters: counts[letter] and its distinct keys, sorted.
local function tally(letters)
  local pool = { counts = {}, keys = {}, size = #letters }
  for _, letter in ipairs(letters) do
    if not pool.counts[letter] then pool.keys[#pool.keys + 1] = letter end
    pool.counts[letter] = (pool.counts[letter] or 0) + 1
  end
  table.sort(pool.keys)
  return pool
end

-- The pool's letters in sorted order: the same for every anagram.
local function signature(pool)
  local parts = {}
  for _, letter in ipairs(pool.keys) do parts[#parts + 1] = letter:rep(pool.counts[letter]) end
  return table.concat(parts)
end

-- Every distinct arrangement of exactly length letters from pool, or nil
-- when there are more than limit of them.
local function arrangements(pool, length, limit)
  local found, chosen, overflow = {}, {}, false
  local counts = pool.counts
  local function extend(depth)
    if depth > length then
      if #found >= limit then overflow = true return end
      found[#found + 1] = table.concat(chosen)
      return
    end
    for _, letter in ipairs(pool.keys) do
      local left = counts[letter]
      if left > 0 then
        counts[letter] = left - 1
        chosen[depth] = letter
        extend(depth + 1)
        counts[letter] = left
        if overflow then return end
      end
    end
  end
  extend(1)
  if overflow then return nil end
  return found
end

-- ---- the dictionary ----

-- app.time() has whole seconds only, so a search ends between TIME_BUDGET - 1
-- and TIME_BUDGET + 1 seconds after it starts, leaving time for the batch in
-- progress.
local function seconds_now()
  local hours, minutes, seconds = app.time():match("(%d+):(%d+):(%d+)")
  return tonumber(hours) * 3600 + tonumber(minutes) * 60 + tonumber(seconds)
end

local function start_clock()
  local start = seconds_now()
  local budget = time_budget()
  return function() return (seconds_now() - start) % 86400 >= budget end
end

-- The candidates the dictionary accepts, and whether every one was checked
-- before the time ran out.
local function dictionary_words(candidates, out_of_time)
  local accepted = {}
  for first = 1, #candidates, BATCH do
    if out_of_time() then return accepted, false end
    local batch = table.move(candidates, first, math.min(first + BATCH - 1, #candidates), 1, {})
    for index, correct in ipairs(spell.check(batch)) do
      if correct then accepted[#accepted + 1] = batch[index] end
    end
  end
  return accepted, true
end

-- ---- the English word list ----

-- words_en_gb.lua's words, and the words for each signature, built on first
-- use and kept while the plugin is loaded.
local word_list

-- The bundled list is ASCII a-z. Counting its letters is linear and avoids
-- allocating and sorting a table of characters for every one of 79,000 words.
local function word_signature(word)
  local counts = {}
  for index = 1, #word do
    local letter = word:byte(index) - 96
    counts[letter] = (counts[letter] or 0) + 1
  end
  local parts = {}
  for letter = 1, 26 do
    if counts[letter] then
      parts[#parts + 1] = string.char(letter + 96):rep(counts[letter])
    end
  end
  return table.concat(parts)
end

local function signature_fits(key, counts)
  local position = 1
  while position <= #key do
    local code = key:byte(position)
    local letter = string.char(code)
    local finish = position + 1
    while finish <= #key and key:byte(finish) == code do finish = finish + 1 end
    if finish - position > (counts[letter] or 0) then return false end
    position = finish
  end
  return true
end

local function english_words()
  if not word_list then
    local words, signatures, by_signature = {}, {}, {}
    for word in require("words_en_gb"):gmatch("%l+") do
      local index = #words + 1
      local key = word_signature(word)
      words[index], signatures[index] = word, key
      by_signature[key] = by_signature[key] or {}
      by_signature[key][#by_signature[key] + 1] = word
    end
    word_list = { words = words, signatures = signatures, by_signature = by_signature }
  end
  return word_list
end

-- Listed words of shortest..longest letters that pool can spell.
local function listed_words_from(pool, shortest, longest)
  local found = {}
  local list = english_words()
  for index, word in ipairs(list.words) do
    if #word >= shortest and #word <= longest and
        signature_fits(list.signatures[index], pool.counts) then
      found[#found + 1] = word
    end
  end
  return found
end

-- Dictionary words of shortest..longest letters from pool, shortest first,
-- for as many lengths as budget candidates allow. Also returns the longest
-- length searched completely.
local function words_from(pool, shortest, longest, budget, out_of_time)
  local words, complete = {}, shortest - 1
  for length = shortest, longest do
    local candidates = arrangements(pool, length, budget)
    if not candidates then break end
    local accepted, finished = dictionary_words(candidates, out_of_time)
    table.move(accepted, 1, #accepted, #words + 1, words)
    if not finished then break end
    complete, budget = length, budget - #candidates
  end
  return words, complete
end

-- ---- input and output ----

-- The text to work on, its letters and whether to use the English word list
-- (the spelling language is English, or there's no spelling dictionary), or
-- nil after telling the user why not.
local function read_input(prompt)
  local language = spell.language()
  local text = editor.get_selection()
  if text == "" then
    text = ui.prompt(prompt)
    if not text then return nil end
  end
  local letters = letters_of(text)
  if #letters < 2 then
    ui.message("Enter at least two letters.", nil, "warning")
    return nil
  elseif #letters > max_letters() then
    ui.message(string.format("Use at most %d letters.", max_letters()), nil, "warning")
    return nil
  end
  return text, letters, not language or language:lower():match("^en") ~= nil
end

-- result with the capitalization of model: all capitals, or a capital first
-- letter. Only ASCII letters change case.
local function like(result, model)
  local letters = model:gsub("[^%a]", "")
  if #letters > 1 and letters == letters:upper() then return result:upper() end
  if letters:match("^%u") then return result:sub(1, 1):upper() .. result:sub(2) end
  return result
end

local function choose_and_insert(items, label, model)
  local _, choice = ui.choose(items, label)
  if choice then editor.replace_selection(like(choice, model)) end
end

local function plural(count, noun)
  return string.format("%d %s%s", count, noun, count == 1 and "" or "s")
end

-- ---- Solve Anagram ----

local function solve()
  local text, letters, english = read_input("Letters to unscramble:")
  if not text then return end
  local found, finished
  if english then
    found, finished = english_words().by_signature[signature(tally(letters))] or {}, true
  else
    local out_of_time = start_clock()
    local candidates = arrangements(tally(letters), #letters, check_limit())
    if not candidates then
      ui.message(string.format("%d letters have too many arrangements to check. " ..
        "Try Create Anagram Phrases, or fewer letters.", #letters), nil, "warning")
      return
    end
    found, finished = dictionary_words(candidates, out_of_time)
  end
  local original = table.concat(letters)
  local anagrams = {}
  for _, word in ipairs(found) do
    if word ~= original then anagrams[#anagrams + 1] = word end
  end
  table.sort(anagrams)
  local stopped = finished and "" or " (the search ran out of time)"
  if #anagrams == 0 then
    ui.message(string.format("No single-word anagrams of \"%s\" are in the dictionary%s.",
      original, stopped))
    return
  end
  choose_and_insert(anagrams, string.format("%s of \"%s\"%s:",
    plural(#anagrams, "anagram"), original, stopped), text)
end

-- ---- Words From Letters ----

local function words_from_letters()
  local text, letters, english = read_input("Letters to find words in:")
  if not text then return end
  local words, complete
  if english then
    words, complete = listed_words_from(tally(letters), 3, #letters), #letters
  else
    words, complete = words_from(tally(letters), 3, #letters, check_limit(), start_clock())
  end
  if #words == 0 then
    ui.message("No words of three or more letters are in the dictionary.")
    return
  end
  table.sort(words, longest_first)
  local note = complete < #letters
    and string.format(" (searched words of up to %d letters)", complete) or ""
  local all = string.format("Insert all %s, grouped by length", plural(#words, "word"))
  local items = { all }
  table.move(words, 1, #words, 2, items)
  local _, choice = ui.choose(items, plural(#words, "word") .. note .. ":")
  if not choice then return end
  if choice ~= all then
    editor.replace_selection(like(choice, text))
    return
  end
  local lines, length = {}, nil
  for _, word in ipairs(words) do
    if utf8.len(word) ~= length then
      length = utf8.len(word)
      lines[#lines + 1] = { length = length }
    end
    table.insert(lines[#lines], word)
  end
  for index, line in ipairs(lines) do
    lines[index] = string.format("%d letters: %s", line.length, table.concat(line, ", "))
  end
  editor.replace_selection(table.concat(lines, editor.eol()))
end

-- ---- Create Anagram Phrases ----

-- The phrase's words, longest first.
local function phrase_text(chosen)
  local words = table.move(chosen, 1, #chosen, 1, {})
  table.sort(words, longest_first)
  return table.concat(words, " "), words
end

-- Pool words as { text, length, need = { [letter] = count } }, and the
-- words for each signature, for finding the last word of a phrase at once.
local function index_words(texts)
  local words, by_signature = {}, {}
  for _, text in ipairs(texts) do
    local letters = letters_of(text)
    local pool = tally(letters)
    local word = { text = text == "i" and "I" or text, length = #letters, need = pool.counts }
    words[#words + 1] = word
    local key = signature(pool)
    by_signature[key] = by_signature[key] or {}
    table.insert(by_signature[key], word)
  end
  return words, by_signature
end

local function fits(need, counts)
  for letter, count in pairs(need) do
    if (counts[letter] or 0) < count then return false end
  end
  return true
end

local function take(need, counts, sign)
  for letter, count in pairs(need) do counts[letter] = counts[letter] - sign * count end
end

-- Phrases of up to MAX_PHRASE_WORDS words using exactly the letters in
-- pool. Each step picks the remaining letter fewest words contain, and tries
-- only the words containing it. Returns the phrases and whether the search
-- finished.
local function find_phrases(pool, words, by_signature, out_of_time)
  local containing = {}
  for _, letter in ipairs(pool.keys) do containing[letter] = {} end
  for _, word in ipairs(words) do
    for letter in pairs(word.need) do table.insert(containing[letter], word) end
  end
  local order = table.move(pool.keys, 1, #pool.keys, 1, {})
  table.sort(order, function(a, b) return #containing[a] < #containing[b] end)

  local counts = {}
  for letter, count in pairs(pool.counts) do counts[letter] = count end
  local phrases, seen, chosen, steps, stopped = {}, {}, {}, 0, false

  local function add(extra)
    chosen[#chosen + 1] = extra
    local text, sorted = phrase_text(chosen)
    chosen[#chosen] = nil
    if seen[text] then return end
    seen[text] = true
    phrases[#phrases + 1] = { text = text, words = #sorted, longest = utf8.len(sorted[1]) }
    if #phrases >= max_phrases() then stopped = true end
  end

  local function search(remaining)
    if #chosen == MAX_PHRASE_WORDS - 1 then
      local key = signature({ keys = pool.keys, counts = counts })
      for _, word in ipairs(by_signature[key] or {}) do
        if not stopped then add(word.text) end
      end
      return
    end
    local letter
    for _, candidate in ipairs(order) do
      if counts[candidate] > 0 then letter = candidate break end
    end
    for _, word in ipairs(containing[letter]) do
      steps = steps + 1
      if steps % 2000 == 0 and out_of_time() then stopped = true end
      if stopped then return end
      if word.length <= remaining and fits(word.need, counts) then
        if word.length == remaining then
          add(word.text)
        else
          take(word.need, counts, 1)
          chosen[#chosen + 1] = word.text
          search(remaining - word.length)
          chosen[#chosen] = nil
          take(word.need, counts, -1)
        end
      end
    end
  end

  search(pool.size)
  return phrases, not stopped
end

local function create_phrases()
  local text, letters, english = read_input("Name or phrase to make anagrams of:")
  if not text then return end
  local out_of_time = start_clock()
  local pool = tally(letters)
  local texts, complete
  if english then
    texts, complete = listed_words_from(pool, 3, #letters - 1), #letters - 1
  else
    -- A smaller budget for the words leaves time to combine them.
    texts, complete = words_from(pool, 2, #letters - 1, check_limit() // 2, out_of_time)
  end
  if english then
    for _, short in ipairs(SHORT_ENGLISH) do
      if fits(tally(letters_of(short)).counts, pool.counts) then texts[#texts + 1] = short end
    end
  end
  local words, by_signature = index_words(texts)
  local phrases, finished = find_phrases(pool, words, by_signature, out_of_time)
  local original = table.concat(letters)
  local note = ""
  if not finished then
    note = " (the search stopped early)"
  elseif complete < #letters - 1 then
    note = string.format(" (using words of up to %d letters)", complete)
  end
  if #phrases == 0 then
    ui.message(string.format("No anagram phrases of \"%s\" were found%s.", original, note))
    return
  end
  -- Fewest words first, then those with the longest words.
  table.sort(phrases, function(a, b)
    if a.words ~= b.words then return a.words < b.words end
    if a.longest ~= b.longest then return a.longest > b.longest end
    return a.text < b.text
  end)
  local items = {}
  for index, phrase in ipairs(phrases) do items[index] = phrase.text end
  choose_and_insert(items, string.format("%s of \"%s\"%s:",
    plural(#items, "phrase"), original, note), text)
end

-- ---- Scramble Words ----

local SCRAMBLE_TRIES = 8

-- Up to SCRAMBLE_TRIES shuffles of characters that differ from them, as
-- strings. Capitals stay where they were, so a name keeps its capital.
local function shuffles(characters)
  local original = table.concat(characters)
  local tries, seen = {}, {}
  for _ = 1, SCRAMBLE_TRIES * 4 do
    local letters = {}
    for index, character in ipairs(characters) do letters[index] = character:lower() end
    for index = #letters, 2, -1 do
      local other = math.random(index)
      letters[index], letters[other] = letters[other], letters[index]
    end
    for index, character in ipairs(characters) do
      if character:match("^%u$") then letters[index] = letters[index]:upper() end
    end
    local result = table.concat(letters)
    if result:lower() ~= original:lower() and not seen[result] then
      seen[result] = true
      tries[#tries + 1] = result
      if #tries == SCRAMBLE_TRIES then break end
    end
  end
  return tries
end

-- Jumbles every word in the selection. A jumble that spells another word,
-- like "silent" for "listen", would make a poor puzzle, so jumbles the
-- dictionary accepts are used only when there's nothing else.
local function scramble()
  local text = editor.get_selection()
  if text == "" then
    ui.message("Select the words to scramble.", nil, "warning")
    return
  end
  local spans, candidates = {}, {}
  for first, word, after in mask_punctuation(text):gmatch("()([%a\128-\255]+)()") do
    local characters = {}
    for _, code in utf8.codes(word) do characters[#characters + 1] = utf8.char(code) end
    local tries = shuffles(characters)
    if #tries > 0 then
      spans[#spans + 1] = { first = first, after = after, tries = tries, offset = #candidates }
      table.move(tries, 1, #tries, #candidates + 1, candidates)
    end
  end
  if #spans == 0 then
    ui.message("The selection has no words that can be scrambled.", nil, "warning")
    return
  end
  local correct = {}
  if spell.language() then
    for first = 1, #candidates, BATCH do
      local batch = table.move(candidates, first, math.min(first + BATCH - 1, #candidates), 1, {})
      table.move(spell.check(batch), 1, #batch, first, correct)
    end
  end
  local parts, position = {}, 1
  for _, span in ipairs(spans) do
    local choice = span.tries[1]
    for index, try in ipairs(span.tries) do
      if not correct[span.offset + index] then choice = try break end
    end
    parts[#parts + 1] = text:sub(position, span.first - 1)
    parts[#parts + 1] = choice
    position = span.after
  end
  parts[#parts + 1] = text:sub(position)
  editor.replace_selection(table.concat(parts))
end

local function configure()
  local fields = {
    { key = "max_letters", label = "Maximum input letters", default = max_letters(), minimum = 2, maximum = 2000 },
    { key = "check_limit", label = "Non-English candidates", default = check_limit(), minimum = 1000, maximum = 1000000 },
    { key = "time_budget", label = "Search seconds", default = time_budget(), minimum = 1, maximum = 5 },
    { key = "max_phrases", label = "Maximum phrase results", default = max_phrases(), minimum = 100, maximum = 10000 }
  }
  for _, field in ipairs(fields) do
    local answer = ui.prompt(field.label .. " (" .. field.minimum .. "–" .. field.maximum .. "):", tostring(field.default))
    if not answer then return end
    local value = tonumber(answer)
    if not value or value < field.minimum or value > field.maximum then
      ui.message(string.format("Enter a number from %d to %d.", field.minimum, field.maximum), nil, "warning")
      return
    end
    config.set(field.key, math.floor(value))
  end
  ui.status("Anagram settings saved.")
end

menu.add("Anagrams (British English)/Solve Anagram...", solve)
menu.add("Anagrams (British English)/Words From Letters...", words_from_letters)
menu.add("Anagrams (British English)/Create Anagram Phrases...", create_phrases)
menu.add("Anagrams (British English)/Scramble Words", scramble)
menu.add("Anagrams (British English)/Configure...", configure)
