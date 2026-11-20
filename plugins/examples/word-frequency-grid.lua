-- Word Frequency Grid: a tiny, self-contained demo of output.table() - no
-- API beyond editor/output/menu. Counts how often each word appears in the
-- current document and shows the result as a sortable grid on the output
-- panel's Table tab: click "Count" to sort by number, click "Word" to sort
-- alphabetically, click either again to reverse.

menu.add("/Tools/Word Frequency Grid", function()
  local text = editor.get_text()
  local counts = {}
  local order = {}
  for word in text:gmatch("%a+") do
    word = word:lower()
    if not counts[word] then
      counts[word] = 0
      order[#order + 1] = word
    end
    counts[word] = counts[word] + 1
  end

  if #order == 0 then
    ui.message("No words found in this document.", "Word Frequency Grid")
    return
  end

  local rows = {}
  for _, word in ipairs(order) do
    rows[#rows + 1] = { Word = word, Count = counts[word] }
  end

  output.table({ "Word", "Count" }, rows)
end)
