-- Playful, dependency-free ciphers and encodings for selected text.
-- These are text toys, not encryption and should not be used for secrets.

local function selection_or_warn()
  local text = editor.get_selection()
  if text == "" then
    ui.message("Select some text first.", nil, "warning")
    return nil
  end
  return text
end

local function replace_with(transform)
  local text = selection_or_warn()
  if not text then return end
  local result, message = transform(text)
  if not result then
    ui.message(message, nil, "warning")
    return
  end
  editor.replace_selection(result)
end

local function shift_letter(byte, first, shift)
  return string.char((byte - first + shift) % 26 + first)
end

local function caesar(text, shift)
  shift = shift % 26
  return (text:gsub("[A-Za-z]", function(letter)
    local byte = letter:byte()
    return shift_letter(byte, byte >= 97 and 97 or 65, shift)
  end))
end

local function atbash(text)
  return (text:gsub("[A-Za-z]", function(letter)
    local byte = letter:byte()
    local first = byte >= 97 and 97 or 65
    return string.char(first + 25 - (byte - first))
  end))
end

local morse_codes = {
  A = ".-", B = "-...", C = "-.-.", D = "-..", E = ".", F = "..-.",
  G = "--.", H = "....", I = "..", J = ".---", K = "-.-", L = ".-..",
  M = "--", N = "-.", O = "---", P = ".--.", Q = "--.-", R = ".-.",
  S = "...", T = "-", U = "..-", V = "...-", W = ".--", X = "-..-",
  Y = "-.--", Z = "--..",
  ["0"] = "-----", ["1"] = ".----", ["2"] = "..---", ["3"] = "...--",
  ["4"] = "....-", ["5"] = ".....", ["6"] = "-....", ["7"] = "--...",
  ["8"] = "---..", ["9"] = "----.",
  ["."] = ".-.-.-", [","] = "--..--", ["?"] = "..--..", ["'"] = ".----.",
  ["!"] = "-.-.--", ["/"] = "-..-.", ["("] = "-.--.", [")"] = "-.--.-",
  ["&"] = ".-...", [":"] = "---...", [";"] = "-.-.-.", ["="] = "-...-",
  ["+"] = ".-.-.", ["-"] = "-....-", ["_"] = "..--.-", ['"'] = ".-..-.",
  ["$"] = "...-..-", ["@"] = ".--.-."
}

local morse_characters = {}
for character, code in pairs(morse_codes) do morse_characters[code] = character end

local function morse_encode_line(line)
  local output, between_words = {}, false
  for position = 1, #line do
    local character = line:sub(position, position)
    if character:match("%s") then
      between_words = #output > 0
    else
      local code = morse_codes[character:upper()]
      if not code then
        return nil, "Morse code supports ASCII letters, digits, and common punctuation."
      end
      if between_words then output[#output + 1] = "/" end
      output[#output + 1] = code
      between_words = false
    end
  end
  return table.concat(output, " ")
end

local function transform_lines(text, transform)
  local output, position = {}, 1
  while position <= #text do
    local first, last = text:find("[\r\n]+", position)
    local line = text:sub(position, first and first - 1 or #text)
    local transformed, message = transform(line)
    if not transformed then return nil, message end
    output[#output + 1] = transformed
    if not first then break end
    output[#output + 1] = text:sub(first, last)
    position = last + 1
  end
  if text == "" then return "" end
  return table.concat(output)
end

local function morse_encode(text)
  return transform_lines(text, morse_encode_line)
end

local function morse_decode_line(line)
  local output, need_space = {}, false
  for token in line:gmatch("%S+") do
    if token == "/" then
      if #output == 0 or need_space then
        return nil, "The selection is not valid Morse code."
      end
      need_space = true
    else
      local character = morse_characters[token]
      if not character then return nil, "Unknown Morse code: " .. token end
      if need_space then output[#output + 1] = " " end
      output[#output + 1] = character
      need_space = false
    end
  end
  if need_space then return nil, "The selection is not valid Morse code." end
  return table.concat(output)
end

local function morse_decode(text)
  return transform_lines(text, morse_decode_line)
end

local nato_words = {
  A = "Alfa", B = "Bravo", C = "Charlie", D = "Delta", E = "Echo", F = "Foxtrot",
  G = "Golf", H = "Hotel", I = "India", J = "Juliett", K = "Kilo", L = "Lima",
  M = "Mike", N = "November", O = "Oscar", P = "Papa", Q = "Quebec", R = "Romeo",
  S = "Sierra", T = "Tango", U = "Uniform", V = "Victor", W = "Whiskey", X = "X-ray",
  Y = "Yankee", Z = "Zulu",
  ["0"] = "Zero", ["1"] = "One", ["2"] = "Two", ["3"] = "Three", ["4"] = "Four",
  ["5"] = "Five", ["6"] = "Six", ["7"] = "Seven", ["8"] = "Eight", ["9"] = "Nine"
}

local nato_characters = {}
for character, word in pairs(nato_words) do nato_characters[word:lower()] = character end

local function nato_encode_line(line)
  local output, between_words = {}, false
  for position = 1, #line do
    local character = line:sub(position, position)
    if character:match("%s") then
      between_words = #output > 0
    else
      local word = nato_words[character:upper()]
      if not word then
        return nil, "NATO phonetics supports ASCII letters and digits."
      end
      if between_words then output[#output + 1] = "/" end
      output[#output + 1] = word
      between_words = false
    end
  end
  return table.concat(output, " ")
end

local function nato_encode(text)
  return transform_lines(text, nato_encode_line)
end

local function nato_decode_line(line)
  local output, need_space = {}, false
  for token in line:gmatch("%S+") do
    if token == "/" then
      if #output == 0 or need_space then
        return nil, "The selection is not valid NATO phonetics."
      end
      need_space = true
    else
      local character = nato_characters[token:lower()]
      if not character then return nil, "Unknown NATO phonetic word: " .. token end
      if need_space then output[#output + 1] = " " end
      output[#output + 1] = character
      need_space = false
    end
  end
  if need_space then return nil, "The selection is not valid NATO phonetics." end
  return table.concat(output)
end

local function nato_decode(text)
  return transform_lines(text, nato_decode_line)
end

-- Every byte's eight bits, precomputed once rather than rebuilt per byte.
local BINARY_BITS = {}
for byte = 0, 255 do
  local bits = {}
  for bit = 7, 0, -1 do
    bits[#bits + 1] = math.floor(byte / 2 ^ bit) % 2 == 1 and "1" or "0"
  end
  BINARY_BITS[byte] = table.concat(bits)
end

-- Chunks of 1024 bytes' worth of bits are joined before going into `output`,
-- not one table entry per byte: a table entry costs much more than the
-- short string it would otherwise hold, and at one per byte that overhead
-- alone could exceed the plugin memory limit well before the document does.
local function binary_encode(text)
  local output, chunk = {}, {}
  for position = 1, #text do
    chunk[#chunk + 1] = BINARY_BITS[text:byte(position)]
    if #chunk == 1024 then
      output[#output + 1] = table.concat(chunk, " ")
      chunk = {}
    end
  end
  if #chunk > 0 then output[#output + 1] = table.concat(chunk, " ") end
  return table.concat(output, " ")
end

local function binary_decode(text)
  local compact = text:gsub("%s", "")
  if compact == "" or #compact % 8 ~= 0 or compact:find("[^01]") then
    return nil, "Binary input must contain groups of eight bits."
  end
  local output = {}
  for position = 1, #compact, 8 do
    output[#output + 1] = string.char(tonumber(compact:sub(position, position + 7), 2))
  end
  local result = table.concat(output)
  if not utf8.len(result) or result:find("\0", 1, true) then
    return nil, "Binary input does not decode to valid UTF-8 text."
  end
  return result
end

local function pig_latin_word(word)
  local upper = word == word:upper()
  local capitalized = not upper and word:sub(1, 1):match("%u") ~= nil
  local lower = word:lower()
  local transformed
  if lower:match("^[aeiou]") then
    transformed = lower .. "way"
  else
    local cluster, rest = lower:match("^([^aeiou]+)(.*)$")
    transformed = rest ~= "" and rest .. cluster .. "ay" or lower .. "ay"
  end
  if upper then return transformed:upper() end
  if capitalized then return transformed:sub(1, 1):upper() .. transformed:sub(2) end
  return transformed
end

local function pig_latin(text)
  return (text:gsub("[A-Za-z]+", pig_latin_word))
end

menu.add("Secret Message Lab/Caesar Shift...", function()
  local text = selection_or_warn()
  if not text then return end
  local answer = ui.prompt("Shift by (-25 to 25):", "3")
  if answer == nil then return end
  local shift = tonumber(answer)
  if not shift or shift % 1 ~= 0 or shift < -25 or shift > 25 then
    ui.message("Enter a whole number from -25 to 25.", nil, "warning")
    return
  end
  editor.replace_selection(caesar(text, shift))
end)

menu.add("Secret Message Lab/ROT13", function()
  replace_with(function(text) return caesar(text, 13) end)
end)

menu.add("Secret Message Lab/Atbash", function()
  replace_with(atbash)
end)

menu.separator("Secret Message Lab")

menu.add("Secret Message Lab/Morse Code/Encode", function()
  replace_with(morse_encode)
end)

menu.add("Secret Message Lab/Morse Code/Decode", function()
  replace_with(morse_decode)
end)

menu.add("Secret Message Lab/NATO Phonetics/Encode", function()
  replace_with(nato_encode)
end)

menu.add("Secret Message Lab/NATO Phonetics/Decode", function()
  replace_with(nato_decode)
end)

menu.add("Secret Message Lab/Binary/Encode UTF-8", function()
  replace_with(binary_encode)
end)

menu.add("Secret Message Lab/Binary/Decode UTF-8", function()
  replace_with(binary_decode)
end)

menu.separator("Secret Message Lab")

menu.add("Secret Message Lab/Pig Latin", function()
  replace_with(pig_latin)
end)
