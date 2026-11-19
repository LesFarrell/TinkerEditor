-- A playful, transparent description of a document's writing style.
-- The results are simple heuristics, not a linguistic or psychological test.
-- Analyse and Readability report in the output panel, or in message boxes
-- with Reports in Output Panel switched off.

local LONG_SENTENCE_INDICATOR = 18
local REPEATED_WORD_INDICATOR = 19
local READING_WORDS_PER_MINUTE = 200 -- a typical adult silent-reading pace

local stop_words = {
  a = true, an = true, ["and"] = true, are = true, as = true, at = true, be = true,
  ["but"] = true, by = true, ["for"] = true, from = true, had = true, has = true,
  have = true, he = true, her = true, his = true, i = true, ["in"] = true,
  is = true, it = true, its = true, me = true, my = true, ["not"] = true,
  of = true, on = true, ["or"] = true, our = true, she = true, so = true,
  that = true, the = true, their = true, them = true, they = true,
  this = true, to = true, was = true, we = true, were = true, with = true,
  you = true, your = true
}

-- Bytes 128-255 count as word characters so non-ASCII letters work, which
-- would include common UTF-8 punctuation: curly quotes, en/em dashes, the
-- ellipsis and no-break space. Those become spaces of the same byte length,
-- so word positions still match the text.
local function mask_punctuation(text)
  return (text:gsub("\226\128[\147\148\152\153\156\157\166]", "   "):gsub("\194\160", "  "))
end

-- Words are held as parallel arrays (key[i], text[i], first[i], last[i],
-- with n words in all) rather than one table per word: a table costs far
-- more than the few fields it would hold, and at one table per word that
-- overhead alone could run past the plugin memory limit on a document of
-- only a megabyte or two of prose.
local function words_in(text, offset)
  local words = { key = {}, text = {}, first = {}, last = {}, n = 0 }
  offset = offset or 0
  for first, word, after in mask_punctuation(text):gmatch("()([A-Za-z0-9_\128-\255]+)()") do
    local n = words.n + 1
    words.key[n] = word:lower()
    words.text[n] = word
    words.first[n] = offset + first
    words.last[n] = offset + after - 1
    words.n = n
  end
  return words
end

-- `words`, when given, is the whole text's words_in() result, already in
-- order: counting a sentence's share of it by position avoids rescanning
-- every sentence's text through words_in() a second time just to learn how
-- many words it has - wasted work that, over a large document, built and
-- discarded about as many word tables as the one list callers keep. Callers
-- that don't need a real count (show_readability finds its own words per
-- sentence afterwards) can leave `words` out; the sentence is then kept
-- whenever it has any word character at all, which costs only a find().
local function sentence_ranges(text, offset, words)
  local ranges, start, position, wordIndex = {}, 1, 1, 1
  offset = offset or 0
  local function take(finish)
    local sentence = text:sub(start, finish)
    local count
    if words then
      count = 0
      local last = offset + finish
      while wordIndex <= words.n and words.last[wordIndex] <= last do
        count = count + 1
        wordIndex = wordIndex + 1
      end
    else
      count = sentence:find("[A-Za-z0-9_\128-\255]") and 1 or 0
    end
    if count > 0 then
      local leading = sentence:match("^%s*") or ""
      ranges[#ranges + 1] = {
        first = offset + start + #leading,
        last = offset + finish,
        text = sentence:sub(#leading + 1),
        words = count
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
    take(finish)
    start, position = finish + 1, finish + 1
  end
  take(#text)
  return ranges
end

local function count_plain(text, needle)
  local count, position = 0, 1
  while true do
    local found = text:find(needle, position, true)
    if not found then return count end
    count, position = count + 1, found + #needle
  end
end

local function paragraph_count(text)
  text = text:gsub("\r\n", "\n"):gsub("\r", "\n")
  local count, in_paragraph = 0, false
  for line in (text .. "\n"):gmatch("(.-)\n") do
    if line:match("%S") then
      if not in_paragraph then count = count + 1 end
      in_paragraph = true
    else
      in_paragraph = false
    end
  end
  return count
end

local function line_count(text)
  if text == "" then return 0 end
  text = text:gsub("\r\n", "\n"):gsub("\r", "\n")
  local _, breaks = text:gsub("\n", "")
  return breaks + 1
end

local function classify_mood(stats)
  if stats.letters >= 10 and stats.uppercase / stats.letters >= 0.65 then
    return "LOUD", "Most letters are capitalised; this document has brought a megaphone."
  end
  if stats.exclamations >= math.max(2, stats.sentences / 3) then
    return "Enthusiastic", "Exclamation marks give it plenty of forward energy."
  end
  if stats.questions >= math.max(2, stats.sentences / 3) then
    return "Curious", "It asks more questions than most documents dare to."
  end
  if stats.ellipses >= 2 then
    return "Suspenseful", "Those ellipses keep leaving thoughts hanging..."
  end
  if stats.average_sentence >= 20 then
    return "Contemplative", "Long sentences let ideas wander before coming home."
  end
  if stats.average_sentence <= 8 then
    return "Punchy", "Short sentences keep the rhythm brisk."
  end
  if stats.words >= 20 and stats.unique / stats.words >= 0.65 then
    return "Eclectic", "Its vocabulary likes taking the scenic route."
  end
  return "Measured", "Its rhythm and punctuation are pleasantly balanced."
end

local function analyse(text)
  local words = words_in(text)
  local sentences = sentence_ranges(text, nil, words)
  if words.n == 0 then return nil end

  local frequencies, unique, favourite, favourite_count = {}, 0, nil, 0
  local letters, uppercase = 0, 0
  for index = 1, words.n do
    local key, value = words.key[index], words.text[index]
    if not frequencies[key] then unique = unique + 1 end
    frequencies[key] = (frequencies[key] or 0) + 1
    if not stop_words[key] and #key >= 3 then
      local count = frequencies[key]
      if count > favourite_count or
        (count == favourite_count and (not favourite or key < favourite)) then
        favourite, favourite_count = key, count
      end
    end
    for letter in value:gmatch("[A-Za-z]") do
      letters = letters + 1
      if letter:match("%u") then uppercase = uppercase + 1 end
    end
  end

  local longest, total_sentence_words = nil, 0
  for _, sentence in ipairs(sentences) do
    total_sentence_words = total_sentence_words + sentence.words
    if not longest or sentence.words > longest.words then longest = sentence end
  end

  local stats = {
    words = words.n,
    unique = unique,
    sentences = math.max(#sentences, 1),
    average_sentence = total_sentence_words / math.max(#sentences, 1),
    exclamations = count_plain(text, "!"),
    questions = count_plain(text, "?"),
    ellipses = count_plain(text, "..."),
    letters = letters,
    uppercase = uppercase
  }
  stats.mood, stats.reason = classify_mood(stats)
  stats.favourite, stats.favourite_count = favourite, favourite_count
  stats.longest = longest
  stats.lines = line_count(text)
  stats.paragraphs = paragraph_count(text)
  return stats
end

local function preview(text)
  text = text:gsub("%s+", " "):match("^%s*(.-)%s*$")
  if (utf8.len(text) or 0) > 64 then return text:sub(1, utf8.offset(text, 62) - 1) .. "..." end
  return text
end

-- Reports go to the output panel's Output tab, where earlier ones stay to
-- compare against, unless Reports in Output Panel is switched off.
local function show_lines(title, lines)
  if config.get("reports_in_output", true) then
    output.report(title, table.concat(lines, "\n"))
  else
    ui.message(table.concat(lines, "\n"), title)
  end
end

local function show_report()
  local selected = editor.get_selection()
  local text = selected ~= "" and selected or editor.get_text()
  local stats = analyse(text)
  if not stats then
    ui.message("There are no words to analyse.", nil, "warning")
    return
  end

  local reading_seconds = math.max(1, math.floor(stats.words / READING_WORDS_PER_MINUTE * 60 + 0.5))
  local reading_time = reading_seconds < 60 and reading_seconds .. " seconds"
    or math.ceil(reading_seconds / 60) .. " minutes"
  local favourite = stats.favourite and
    string.format("%s (%d times)", stats.favourite, stats.favourite_count) or "No clear favourite"
  local longest = stats.longest and
    string.format("%d words — %s", stats.longest.words, preview(stats.longest.text)) or "None"
  local scope = selected ~= "" and "Selection" or "Document"

  show_lines("Document Personality", {
    scope .. " mood: " .. stats.mood,
    stats.reason,
    "",
    string.format("Words: %d  |  Unique: %d  |  Sentences: %d", stats.words, stats.unique,
      stats.sentences),
    string.format("Paragraphs: %d  |  Lines: %d  |  Reading time: %s", stats.paragraphs,
      stats.lines, reading_time),
    string.format("Average sentence: %.1f words", stats.average_sentence),
    "Favourite word: " .. favourite,
    "Longest sentence: " .. longest
  })
end

-- ---- Readability ----
-- The standard English formulae. They count syllables, which are estimated
-- from groups of vowels, so treat the scores as a guide.

local LONG_SENTENCE_WORDS = 25

-- Estimated syllables in an English word, from groups of vowels after
-- dropping a silent ending ("-es", "-ed", "-e", but not "-le" as in table).
local function syllables(word)
  word = word:lower():gsub("[^a-z]", "")
  if #word <= 3 then return 1 end
  local trimmed = word:gsub("[^laeiouy]es$", "")
  if trimmed == word then trimmed = word:gsub("ed$", "") end
  if trimmed == word then trimmed = word:gsub("[^laeiouy]e$", "") end
  local count = 0
  for _ in trimmed:gsub("^y", ""):gmatch("[aeiouy][aeiouy]?") do count = count + 1 end
  return math.max(count, 1)
end

-- The prose of Markdown text as blocks, so that a heading or list item
-- without a full stop doesn't run into the next sentence. Fenced code,
-- inline code, link targets and HTML tags are left out.
local function prose_blocks(text)
  local blocks, current, fence = {}, {}, nil
  local function finish()
    if #current > 0 then blocks[#blocks + 1] = table.concat(current, " ") end
    current = {}
  end
  text = text:gsub("\r\n", "\n"):gsub("\r", "\n")
  for source_line in (text .. "\n"):gmatch("(.-)\n") do
    local line = source_line
    local marker = line:match("^ ? ? ?(```+)") or line:match("^ ? ? ?(~~~+)")
    if fence then
      if marker and marker:sub(1, 1) == fence:sub(1, 1) and #marker >= #fence then fence = nil end
    elseif marker then
      finish()
      fence = marker
    elseif line:match("^%s*$") then
      finish()
    else
      line = line:gsub("`+[^`]*`+", " "):gsub("%]%b()", "]"):gsub("<[^<>]*>", " ")
      local heading_or_item = line:match("^%s*#") or line:match("^%s*[-*+] ")
        or line:match("^%s*%d+[.)] ")
      if heading_or_item then finish() end
      current[#current + 1] = line
      if heading_or_item then finish() end
    end
  end
  finish()
  return blocks
end

local function reading_ease_band(score)
  if score >= 90 then return "very easy" end
  if score >= 80 then return "easy" end
  if score >= 70 then return "fairly easy" end
  if score >= 60 then return "plain English" end
  if score >= 50 then return "fairly difficult" end
  if score >= 30 then return "difficult" end
  return "very difficult"
end

local function show_readability()
  local selected = editor.get_selection()
  local words, sentences, syllable_count, complex, letters, long = 0, 0, 0, 0, 0, 0
  for _, block in ipairs(prose_blocks(selected ~= "" and selected or editor.get_text())) do
    for _, sentence in ipairs(sentence_ranges(block)) do
      local counted = 0
      local sentence_words = words_in(sentence.text)
      for index = 1, sentence_words.n do
        local value = sentence_words.text[index]
        local _, letter_count = value:gsub("%a", "")
        if letter_count > 0 then
          local count = syllables(value)
          counted, syllable_count, letters = counted + 1, syllable_count + count, letters + letter_count
          if count >= 3 then complex = complex + 1 end
        end
      end
      if counted > 0 then
        words, sentences = words + counted, sentences + 1
        if counted >= LONG_SENTENCE_WORDS then long = long + 1 end
      end
    end
  end
  if words == 0 then
    ui.message("There are no words to analyse.", nil, "warning")
    return
  end

  local per_sentence, per_word = words / sentences, syllable_count / words
  local ease = 206.835 - 1.015 * per_sentence - 84.6 * per_word
  local grade = 0.39 * per_sentence + 11.8 * per_word - 15.59
  local fog = 0.4 * (per_sentence + 100 * complex / words)
  local coleman_liau = 0.0588 * (100 * letters / words) - 0.296 * (100 * sentences / words) - 15.8
  show_lines((selected ~= "" and "Selection" or "Document") .. " Readability", {
    string.format("Flesch reading ease: %.0f (%s)", ease, reading_ease_band(ease)),
    string.format("Flesch-Kincaid grade: %.1f", grade),
    string.format("Gunning fog index: %.1f", fog),
    string.format("Coleman-Liau index: %.1f", coleman_liau),
    "",
    string.format("Words: %d  |  Sentences: %d  |  Syllables: %d", words, sentences, syllable_count),
    string.format("Average sentence: %.1f words  |  Average word: %.2f syllables",
      per_sentence, per_word),
    string.format("Words of three or more syllables: %d (%.0f%%)", complex, 100 * complex / words),
    string.format("Sentences of %d or more words: %d", LONG_SENTENCE_WORDS, long),
    "",
    "Reading ease runs from 0 (hardest) to 100 (easiest); the other scores",
    "are US school grades. All of them are for English text."
  })
end

local function positive_integer(label, default, maximum)
  local answer = ui.prompt(label, tostring(default))
  if answer == nil then return nil end
  local value = tonumber(answer)
  if not value or value % 1 ~= 0 or value < 2 or value > maximum then
    ui.message("Enter a whole number from 2 to " .. maximum .. ".", nil, "warning")
    return nil
  end
  return value
end

local function highlight_long_sentences()
  local minimum = positive_integer("Highlight sentences with at least this many words:", 25, 500)
  if not minimum then return end
  editor.clear_indicator(LONG_SENTENCE_INDICATOR)
  local count = 0
  local text = editor.get_text()
  for _, sentence in ipairs(sentence_ranges(text, nil, words_in(text))) do
    if sentence.words >= minimum then
      editor.indicator(sentence.first, sentence.last, LONG_SENTENCE_INDICATOR, 0xffa726)
      count = count + 1
    end
  end
  ui.status(string.format("Highlighted %d long sentence%s.", count, count == 1 and "" or "s"))
end

local function highlight_repeated_words()
  local minimum = positive_integer("Highlight words used at least this many times:", 3, 100)
  if not minimum then return end
  local words, frequencies = words_in(editor.get_text()), {}
  for index = 1, words.n do
    local key = words.key[index]
    if not stop_words[key] and #key >= 4 then
      frequencies[key] = (frequencies[key] or 0) + 1
    end
  end
  editor.clear_indicator(REPEATED_WORD_INDICATOR)
  local count, kinds = 0, {}
  for index = 1, words.n do
    local key = words.key[index]
    if (frequencies[key] or 0) >= minimum then
      editor.indicator(words.first[index], words.last[index], REPEATED_WORD_INDICATOR, 0xab47bc)
      count = count + 1
      kinds[key] = true
    end
  end
  local kind_count = 0
  for _ in pairs(kinds) do kind_count = kind_count + 1 end
  ui.status(string.format("Highlighted %d occurrence%s of %d repeated word%s.", count,
    count == 1 and "" or "s", kind_count, kind_count == 1 and "" or "s"))
end

menu.add("Document Personality/Analyse", show_report)
menu.add("Document Personality/Readability", show_readability)
menu.checkbox("Document Personality/Reports in Output Panel", "reports_in_output", true, function() end)
menu.separator("Document Personality")
menu.add("Document Personality/Highlight Long Sentences...", highlight_long_sentences)
menu.add("Document Personality/Highlight Repeated Words...", highlight_repeated_words)
menu.add("Document Personality/Clear Highlights", function()
  editor.clear_indicator(LONG_SENTENCE_INDICATOR)
  editor.clear_indicator(REPEATED_WORD_INDICATOR)
end)
