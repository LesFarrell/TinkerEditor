static int line_has_list_marker(const char *line, Sci_Position length) {
    Sci_Position i = 0;
    while (i < length && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i + 1 < length && (line[i] == '-' || line[i] == '*' || line[i] == '+') &&
        (line[i + 1] == ' ' || line[i + 1] == '\t')) return 1;
    Sci_Position digits = i;
    while (i < length && line[i] >= '0' && line[i] <= '9') i++;
    return i > digits && i + 1 < length && (line[i] == '.' || line[i] == ')') &&
        (line[i + 1] == ' ' || line[i + 1] == '\t');
}

/* Preserve real tabs for indentation, but use spaces for alignment after
   ordinary text. A collapsed caret anywhere in a Markdown list item indents
   the entire item, which makes nesting work without first moving the caret. */
static int editor_tab_width(HWND hEdit) {
    int tabWidth = (int)SendMessageA(hEdit, SCI_GETTABWIDTH, 0, 0);
    return tabWidth < 1 ? 4 : tabWidth;
}

static int smart_tab(HWND hEdit, int reverse) {
    if (g_largeFile) return 0; /* copies the whole line, see smart_newline() */
    Sci_Position start = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position end = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (start != end) return 0; /* retain Scintilla's block-selection behavior */

    Sci_Position lineNumber = (Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)start, 0);
    Sci_Position lineStart = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)lineNumber, 0);
    Sci_Position lineEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETLINEENDPOSITION, (WPARAM)lineNumber, 0);
    Sci_Position lineLength = lineEnd - lineStart;
    char *line = get_text_range(hEdit, lineStart, lineEnd);
    if (!line) return 0;

    int leadingLength = 0;
    while (leadingLength < lineLength &&
           (line[leadingLength] == ' ' || line[leadingLength] == '\t')) leadingLength++;
    int listItem = line_has_list_marker(line, lineLength);

    if (reverse && leadingLength > 0) {
        int remove = 0;
        int tabWidth = editor_tab_width(hEdit);
        if (line[0] == '\t') remove = 1;
        else while (remove < leadingLength && remove < tabWidth && line[remove] == ' ') remove++;
        SendMessageA(hEdit, SCI_SETTARGETSTART, (WPARAM)lineStart, 0);
        SendMessageA(hEdit, SCI_SETTARGETEND, (WPARAM)(lineStart + remove), 0);
        SendMessageA(hEdit, SCI_REPLACETARGET, 0, (LPARAM)"");
        free(line);
        return 1;
    }

    if (!reverse && listItem) {
        SendMessageA(hEdit, SCI_INSERTTEXT, (WPARAM)lineStart, (LPARAM)"\t");
        SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(start + 1), (LPARAM)(start + 1));
        free(line);
        return 1;
    }

    free(line);
    if (reverse || start <= lineStart + leadingLength) return 0;

    int tabWidth = editor_tab_width(hEdit);
    Sci_Position column = (Sci_Position)SendMessageA(hEdit, SCI_GETCOLUMN, (WPARAM)start, 0);
    int spaces = tabWidth - column % tabWidth;
    char padding[32];
    if (spaces >= (int)sizeof(padding)) spaces = (int)sizeof(padding) - 1;
    memset(padding, ' ', (size_t)spaces);
    padding[spaces] = 0;
    SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)padding);
    return 1;
}

static int smart_markdown_newline(HWND hEdit) {
    Sci_Position selectionStart = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selectionEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (selectionStart != selectionEnd) return 0;
    Sci_Position caret = selectionEnd;
    if (caret > 0 && (int)SendMessageA(hEdit, SCI_GETSTYLEAT, (WPARAM)(caret - 1), 0) == SCE_MARKDOWN_CODEBK)
        return 0;
    Sci_Position line = (Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)caret, 0);
    Sci_Position lineStart = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
    Sci_Position beforeLength = caret - lineStart;
    if (beforeLength <= 0) return 0;
    char *before = get_text_range(hEdit, lineStart, caret);
    if (!before) return 0;

    int i = 0;
    while (i < beforeLength && (before[i] == ' ' || before[i] == '\t')) i++;
    int indentEnd = i;
    while (i < beforeLength && before[i] == '>') {
        i++;
        if (i < beforeLength && before[i] == ' ') i++;
    }
    int quoteEnd = i;
    int markerStart = i, markerEnd = i, ordered = 0, nextNumber = 0;
    char punctuation = 0;
    if (i + 1 < beforeLength && (before[i] == '-' || before[i] == '*' || before[i] == '+') &&
        (before[i + 1] == ' ' || before[i + 1] == '\t')) {
        i += 2;
        markerEnd = i;
        if (i + 3 < beforeLength && before[i] == '[' &&
            (before[i + 1] == ' ' || before[i + 1] == 'x' || before[i + 1] == 'X') &&
            before[i + 2] == ']' && (before[i + 3] == ' ' || before[i + 3] == '\t')) {
            i += 4;
            markerEnd = i;
        }
    } else {
        int digits = i;
        unsigned long number = 0;
        while (i < beforeLength && before[i] >= '0' && before[i] <= '9') {
            if (number < 9999999) number = number * 10 + (unsigned)(before[i] - '0');
            i++;
        }
        if (i > digits && i + 1 < beforeLength && (before[i] == '.' || before[i] == ')') &&
            (before[i + 1] == ' ' || before[i + 1] == '\t')) {
            punctuation = before[i];
            i += 2;
            markerEnd = i;
            ordered = 1;
            nextNumber = (int)number + 1;
        }
    }
    int hasMarker = markerEnd > markerStart;
    int hasQuote = quoteEnd > indentEnd;
    if (!hasMarker && !hasQuote) { free(before); return 0; }
    int contentStart = hasMarker ? markerEnd : quoteEnd;
    int contentEmpty = 1;
    for (Sci_Position p = contentStart; p < beforeLength; p++) {
        if (before[p] != ' ' && before[p] != '\t') { contentEmpty = 0; break; }
    }

    SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
    if (contentEmpty) {
        /* An empty list item or quote terminates that construct. Nested quote
           prefixes remain when terminating a list nested inside a quote. */
        int keep = hasMarker ? quoteEnd : indentEnd;
        SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(lineStart + keep), (LPARAM)caret);
        SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)"");
    } else {
        const char *eol = editor_eol_sequence();
        char orderedMarker[48] = "";
        const char *markerText = NULL;
        int prefixLength = contentStart;
        if (ordered) {
            snprintf(orderedMarker, sizeof(orderedMarker), "%d%c ", nextNumber, punctuation);
            markerText = orderedMarker;
            prefixLength = markerStart;
        }
        size_t insertLength = strlen(eol) + (size_t)prefixLength +
            (markerText ? strlen(markerText) : 0);
        char *insert = (char *)malloc(insertLength + 1);
        if (insert) {
            strcpy(insert, eol);
            memcpy(insert + strlen(eol), before, (size_t)prefixLength);
            size_t used = strlen(eol) + (size_t)prefixLength;
            if (markerText) {
                strcpy(insert + used, markerText);
            } else if (hasMarker && markerEnd - markerStart >= 5 &&
                       before[markerStart + 2] == '[') {
                /* Continue task lists unchecked, even when the previous item was checked. */
                memcpy(insert + strlen(eol) + markerStart, before + markerStart,
                       (size_t)(markerEnd - markerStart));
                insert[strlen(eol) + markerStart + 3] = ' ';
                insert[insertLength] = 0;
            } else {
                insert[insertLength] = 0;
            }
            SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)insert);
            free(insert);
        }
    }
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(before);
    return 1;
}

static int ends_with_word(const char *text, Sci_Position length, const char *word) {
    int wordLength = (int)strlen(word);
    if (length < wordLength || memcmp(text + length - wordLength, word,
                                      (size_t)wordLength) != 0) return 0;
    return length == wordLength || !(isalnum((unsigned char)text[length - wordLength - 1]) ||
                                      text[length - wordLength - 1] == '_');
}

static int style_allows_indent(HWND hEdit, Sci_Position position) {
    int style = (int)SendMessageA(hEdit, SCI_GETSTYLEAT, (WPARAM)position, 0);
    switch (g_activeLanguage) {
        case LANG_CPP:
            return !(style == SCE_C_COMMENT || style == SCE_C_COMMENTLINE ||
                     style == SCE_C_COMMENTDOC || style == SCE_C_COMMENTLINEDOC ||
                     style == SCE_C_STRING || style == SCE_C_CHARACTER ||
                     style == SCE_C_STRINGEOL || style == SCE_C_VERBATIM ||
                     style == SCE_C_REGEX || style == SCE_C_STRINGRAW ||
                     style == SCE_C_TRIPLEVERBATIM || style == SCE_C_HASHQUOTEDSTRING);
        case LANG_JSON:
            return !(style == SCE_JSON_STRING || style == SCE_JSON_STRINGEOL ||
                     style == SCE_JSON_LINECOMMENT || style == SCE_JSON_BLOCKCOMMENT);
        case LANG_PYTHON:
            return !(style == SCE_P_COMMENTLINE || style == SCE_P_COMMENTBLOCK ||
                     style == SCE_P_STRING || style == SCE_P_CHARACTER ||
                     style == SCE_P_TRIPLE || style == SCE_P_TRIPLEDOUBLE ||
                     style == SCE_P_STRINGEOL || style == SCE_P_FSTRING ||
                     style == SCE_P_FCHARACTER || style == SCE_P_FTRIPLE ||
                     style == SCE_P_FTRIPLEDOUBLE);
        case LANG_YAML:
            return style != SCE_YAML_COMMENT;
        case LANG_BASH:
            return !(style == SCE_SH_COMMENTLINE || style == SCE_SH_STRING ||
                     style == SCE_SH_CHARACTER || style == SCE_SH_HERE_DELIM ||
                     style == SCE_SH_HERE_Q);
        case LANG_POWERSHELL:
            return !(style == SCE_POWERSHELL_COMMENT ||
                     style == SCE_POWERSHELL_COMMENTSTREAM ||
                     style == SCE_POWERSHELL_COMMENTDOCKEYWORD ||
                     style == SCE_POWERSHELL_STRING ||
                     style == SCE_POWERSHELL_CHARACTER ||
                     style == SCE_POWERSHELL_HERE_STRING ||
                     style == SCE_POWERSHELL_HERE_CHARACTER);
        default:
            return 1;
    }
}

static int line_opens_indent(HWND hEdit, const char *before, Sci_Position contentStart,
                             Sci_Position trimmedEnd, Sci_Position lineStart, char *closingOut) {
    if (closingOut) *closingOut = 0;
    if (trimmedEnd <= contentStart || before[contentStart] == '#') return 0;
    if (!style_allows_indent(hEdit, lineStart + trimmedEnd - 1)) return 0;
    char last = before[trimmedEnd - 1];
    switch (g_activeLanguage) {
        case LANG_CPP:
        case LANG_POWERSHELL:
            if (last == '{') { if (closingOut) *closingOut = '}'; return 1; }
            break;
        case LANG_JSON:
            if (last == '{') { if (closingOut) *closingOut = '}'; return 1; }
            if (last == '[') { if (closingOut) *closingOut = ']'; return 1; }
            break;
        case LANG_PYTHON:
        case LANG_YAML:
            return last == ':';
        case LANG_BASH:
            if (last == '{') { if (closingOut) *closingOut = '}'; return 1; }
            return ends_with_word(before + contentStart, trimmedEnd - contentStart, "then") ||
                   ends_with_word(before + contentStart, trimmedEnd - contentStart, "do") ||
                   ends_with_word(before + contentStart, trimmedEnd - contentStart, "in");
        default:
            break;
    }
    return 0;
}

/* Carry the current line's leading whitespace onto the next line and add one
   real tab after language-specific block openers. When the caret is between a
   paired opener and closer, expand both onto properly aligned lines at once. */
static int smart_indented_newline(HWND hEdit) {
    Sci_Position selectionStart = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selectionEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (selectionStart != selectionEnd) return 0;
    Sci_Position caret = selectionEnd;
    Sci_Position lineNumber = (Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)caret, 0);
    Sci_Position lineStart = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)lineNumber, 0);
    Sci_Position lineEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETLINEENDPOSITION, (WPARAM)lineNumber, 0);
    Sci_Position beforeLength = caret - lineStart;
    if (beforeLength < 0) return 0;
    char *before = get_text_range(hEdit, lineStart, caret);
    if (!before) return 0;

    int indentLength = 0;
    while (indentLength < beforeLength &&
           (before[indentLength] == ' ' || before[indentLength] == '\t')) indentLength++;
    Sci_Position trimmedEnd = beforeLength;
    while (trimmedEnd > indentLength &&
           (before[trimmedEnd - 1] == ' ' || before[trimmedEnd - 1] == '\t')) trimmedEnd--;
    char closing = 0;
    int addIndent = line_opens_indent(hEdit, before, indentLength, trimmedEnd, lineStart, &closing);

    Sci_Position after = caret;
    while (after < lineEnd) {
        char character = (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)after, 0);
        if (character != ' ' && character != '\t') break;
        after++;
    }
    int pairedClose = addIndent && closing && after < lineEnd &&
        (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)after, 0) == closing;
    if (pairedClose) {
        Sci_Position rest = after + 1;
        while (rest < lineEnd) {
            char character = (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)rest, 0);
            if (character != ' ' && character != '\t') { pairedClose = 0; break; }
            rest++;
        }
    }

    if (!indentLength && !addIndent) { free(before); return 0; }
    const char *eol = editor_eol_sequence();
    size_t eolLength = strlen(eol);
    size_t firstLength = eolLength + (size_t)indentLength + (addIndent ? 1 : 0);
    size_t insertLength = firstLength + (pairedClose ? eolLength + (size_t)indentLength : 0);
    char *insert = (char *)malloc(insertLength + 1);
    if (!insert) { free(before); return 0; }
    size_t used = 0;
    memcpy(insert + used, eol, eolLength); used += eolLength;
    memcpy(insert + used, before, (size_t)indentLength); used += (size_t)indentLength;
    if (addIndent) insert[used++] = '\t';
    if (pairedClose) {
        memcpy(insert + used, eol, eolLength); used += eolLength;
        memcpy(insert + used, before, (size_t)indentLength); used += (size_t)indentLength;
    }
    insert[used] = 0;
    SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
    if (pairedClose && after > caret)
        SendMessageA(hEdit, SCI_SETSEL, (WPARAM)caret, (LPARAM)after);
    SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)insert);
    if (pairedClose)
        SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(caret + (int)firstLength),
                     (LPARAM)(caret + (int)firstLength));
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(insert);
    free(before);
    return 1;
}

static int smart_newline(HWND hEdit) {
    /* Both copy the line up to the caret - in a large file with few line
       breaks, that can be most of the document on every Enter. */
    if (g_largeFile) return 0;
    if (g_activeLanguage == LANG_MARKDOWN && smart_markdown_newline(hEdit)) return 1;
    return smart_indented_newline(hEdit);
}

/* Typing a closing brace on an otherwise blank indented line brings it back
   one indentation level. Existing auto-paired closers are still skipped by
   smart_pair_character() instead of being replaced. */
static int smart_closing_indent(HWND hEdit, char typed) {
    int valid = typed == '}' && (g_activeLanguage == LANG_CPP ||
        g_activeLanguage == LANG_BASH || g_activeLanguage == LANG_POWERSHELL ||
        g_activeLanguage == LANG_JSON);
    if (typed == ']' && g_activeLanguage == LANG_JSON) valid = 1;
    if (!valid) return 0;
    Sci_Position start = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position end = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (start != end || (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)end, 0) == typed)
        return 0;
    Sci_Position line = (Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)start, 0);
    Sci_Position lineStart = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
    for (Sci_Position position = lineStart; position < start; position++) {
        char character = (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)position, 0);
        if (character != ' ' && character != '\t') return 0;
    }
    if (start == lineStart) return 0;
    Sci_Position removeStart = start;
    if ((char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)(start - 1), 0) == '\t') {
        removeStart--;
    } else {
        int tabWidth = editor_tab_width(hEdit);
        while (removeStart > lineStart && start - removeStart < tabWidth &&
               (char)SendMessageA(hEdit, SCI_GETCHARAT,
                                  (WPARAM)(removeStart - 1), 0) == ' ') removeStart--;
    }
    char replacement[2] = { typed, 0 };
    SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)removeStart, (LPARAM)start);
    SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)replacement);
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
    return 1;
}

static int smart_pair_character(HWND hEdit, char typed) {
    char closing = typed == '(' ? ')' : typed == '[' ? ']' : typed == '{' ? '}' : typed == '`' ? '`' : 0;
    Sci_Position start = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position end = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (typed == '`' && start == end) {
        if ((char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)end, 0) == '`') {
            SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(end + 1), (LPARAM)(end + 1));
            return 1;
        }
        if (start >= 2 && (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)(start - 1), 0) == '`' &&
            (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)(start - 2), 0) == '`') return 0;
    }
    if (!closing) {
        if ((typed == ')' || typed == ']' || typed == '}') && start == end &&
            (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)end, 0) == typed) {
            SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(end + 1), (LPARAM)(end + 1));
            return 1;
        }
        return 0;
    }
    Sci_Position length = end - start;
    char *selected = get_text_range(hEdit, start, end);
    if (!selected) return 0;
    size_t cap = (size_t)length + 3;
    char *replacement = (char *)malloc(cap);
    if (!replacement) { free(selected); return 0; }
    replacement[0] = typed;
    if (length > 0) memcpy(replacement + 1, selected, (size_t)length);
    replacement[length + 1] = closing;
    replacement[length + 2] = 0;
    SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)replacement);
    if (length > 0) SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(start + 1), (LPARAM)(start + 1 + length));
    else SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(start + 1), (LPARAM)(start + 1));
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(replacement);
    free(selected);
    return 1;
}

static int smart_pair_backspace(HWND hEdit) {
    Sci_Position start = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position end = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (start != end || start <= 0) return 0;
    char before = (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)(start - 1), 0);
    char after = (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)start, 0);
    if (!((before == '(' && after == ')') || (before == '[' && after == ']') ||
          (before == '{' && after == '}') || (before == '`' && after == '`'))) return 0;
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(start - 1), (LPARAM)(start + 1));
    SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)"");
    return 1;
}

#define EDITOR_CONTEXT_MENU_PLUGIN_ITEM_MAX 64
#define EDITOR_CONTEXT_MENU_PLUGIN_FIRST 100

/* Adds a snapshot of the registered plugin items. Stable registry ids are
   kept separately from the popup's small, temporary Win32 command ids: a
   deferred callback can add or remove registrations while TrackPopupMenu's
   modal loop is open. */
static int append_plugin_context_menu_items(HMENU menu, int *itemIds, HWND hEdit) {
    int available = plugin_host_context_menu_item_count();
    int count = available < EDITOR_CONTEXT_MENU_PLUGIN_ITEM_MAX
        ? available : EDITOR_CONTEXT_MENU_PLUGIN_ITEM_MAX;
    if (count && GetMenuItemCount(menu) > 0) AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    int appended = 0;
    for (int i = 0; i < count; i++) {
        WCHAR *label = to_wide(plugin_host_context_menu_item_label(i));
        if (!label) continue;
        int itemId = plugin_host_context_menu_item_id(i);
        int checked = plugin_host_context_menu_item_is_checked(i);
        WCHAR *parts[PLUGIN_MAX_CONTEXT_DEPTH];
        int partCount = split_plugin_label(label, parts);
        HMENU destination = menu;
        for (int part = 0; destination && part < partCount - 1; part++)
            destination = plugin_submenu(destination, parts[part], NULL);
        UINT flags = MF_STRING;
        int enabled = 0;
        int oldPluginDocument = g_pluginCommandDocument;
        g_pluginCommandDocument = document_index_for_view(hEdit);
        char *error = plugin_host_context_menu_item_enabled(i, hEdit, &enabled);
        g_pluginCommandDocument = oldPluginDocument;
        if (error) show_plugin_error(error);
        free(error);
        if (!enabled) flags |= MF_GRAYED;
        if (checked) flags |= MF_CHECKED;
        if (destination && partCount &&
            AppendMenuW(destination, flags,
                        EDITOR_CONTEXT_MENU_PLUGIN_FIRST + appended, parts[partCount - 1])) {
            itemIds[appended] = itemId;
            appended++;
        }
        free(label);
    }
    return appended;
}

static int run_plugin_context_menu_command(UINT command, const int *itemIds,
                                           int itemCount, HWND hEdit) {
    int index = (int)command - EDITOR_CONTEXT_MENU_PLUGIN_FIRST;
    if (index < 0 || index >= itemCount) return 0;
    run_plugin_context_menu_item(itemIds[index], hEdit);
    return 1;
}

static POINT editor_context_menu_point(HWND hwnd, LPARAM lParam) {
    POINT point;
    if ((int)GET_X_LPARAM(lParam) == -1 && (int)GET_Y_LPARAM(lParam) == -1) {
        if (!GetCaretPos(&point)) point = (POINT){0, 0};
        ClientToScreen(hwnd, &point);
    } else {
        point.x = GET_X_LPARAM(lParam);
        point.y = GET_Y_LPARAM(lParam);
    }
    return point;
}

static void show_editor_context_menu(HWND hwnd, LPARAM lParam) {
    enum {
        EDIT_CONTEXT_UNDO = 1, EDIT_CONTEXT_REDO, EDIT_CONTEXT_CUT,
        EDIT_CONTEXT_COPY, EDIT_CONTEXT_PASTE, EDIT_CONTEXT_DELETE,
        EDIT_CONTEXT_SELECT_ALL
    };
    Sci_Position selectionStart = (Sci_Position)SendMessageA(hwnd, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selectionEnd = (Sci_Position)SendMessageA(hwnd, SCI_GETSELECTIONEND, 0, 0);
    int hasSelection = selectionStart != selectionEnd;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | (SendMessageA(hwnd, SCI_CANUNDO, 0, 0) ? 0 : MF_GRAYED),
                EDIT_CONTEXT_UNDO, L"Undo");
    AppendMenuW(menu, MF_STRING | (SendMessageA(hwnd, SCI_CANREDO, 0, 0) ? 0 : MF_GRAYED),
                EDIT_CONTEXT_REDO, L"Redo");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), EDIT_CONTEXT_CUT, L"Cut");
    AppendMenuW(menu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), EDIT_CONTEXT_COPY, L"Copy");
    AppendMenuW(menu, MF_STRING | (SendMessageA(hwnd, SCI_CANPASTE, 0, 0) ? 0 : MF_GRAYED),
                EDIT_CONTEXT_PASTE, L"Paste");
    AppendMenuW(menu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), EDIT_CONTEXT_DELETE, L"Delete");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, EDIT_CONTEXT_SELECT_ALL, L"Select All");

    int itemIds[EDITOR_CONTEXT_MENU_PLUGIN_ITEM_MAX];
    int itemCount = append_plugin_context_menu_items(menu, itemIds, hwnd);
    POINT point = editor_context_menu_point(hwnd, lParam);
    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  point.x, point.y, 0, g_hMainWnd, NULL);
    DestroyMenu(menu);
    if (run_plugin_context_menu_command(command, itemIds, itemCount, hwnd)) return;
    if (command == EDIT_CONTEXT_UNDO) SendMessageA(hwnd, SCI_UNDO, 0, 0);
    else if (command == EDIT_CONTEXT_REDO) SendMessageA(hwnd, SCI_REDO, 0, 0);
    else if (command == EDIT_CONTEXT_CUT) SendMessageA(hwnd, SCI_CUT, 0, 0);
    else if (command == EDIT_CONTEXT_COPY) SendMessageA(hwnd, SCI_COPY, 0, 0);
    else if (command == EDIT_CONTEXT_PASTE) SendMessageA(hwnd, SCI_PASTE, 0, 0);
    else if (command == EDIT_CONTEXT_DELETE) SendMessageA(hwnd, SCI_REPLACESEL, 0, (LPARAM)"");
    else if (command == EDIT_CONTEXT_SELECT_ALL) SendMessageA(hwnd, SCI_SELECTALL, 0, 0);
}

static int show_spelling_context_menu(HWND hwnd, LPARAM lParam) {
    int documentIndex = document_index_for_view(hwnd);
    const Document *doc = documentIndex >= 0 ? &g_documentStore.documents[documentIndex] : NULL;
    Language language = language_for_path(doc ? doc->path : NULL)->id;
    if (!g_spellCheckEnabled || !g_spellChecker || !doc || doc->largeFile ||
        (language != LANG_MARKDOWN && language != LANG_TEXT))
        return 0;

    POINT screenPoint;
    Sci_Position position;
    if ((int)GET_X_LPARAM(lParam) == -1 && (int)GET_Y_LPARAM(lParam) == -1) {
        position = (Sci_Position)SendMessageA(hwnd, SCI_GETCURRENTPOS, 0, 0);
        if (!GetCaretPos(&screenPoint)) screenPoint = (POINT){0, 0};
        ClientToScreen(hwnd, &screenPoint);
    } else {
        screenPoint.x = GET_X_LPARAM(lParam);
        screenPoint.y = GET_Y_LPARAM(lParam);
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd, &clientPoint);
        position = (Sci_Position)SendMessageA(hwnd, SCI_POSITIONFROMPOINTCLOSE,
                                     (WPARAM)clientPoint.x, (LPARAM)clientPoint.y);
    }
    if (position < 0) return 0;

    SendMessageA(hwnd, SCI_SETINDICATORCURRENT, INDICATOR_SPELLING, 0);
    if (!SendMessageA(hwnd, SCI_INDICATORVALUEAT, INDICATOR_SPELLING, position) && position > 0)
        position--;
    if (!SendMessageA(hwnd, SCI_INDICATORVALUEAT, INDICATOR_SPELLING, position)) return 0;

    Sci_Position start = (Sci_Position)SendMessageA(hwnd, SCI_INDICATORSTART, INDICATOR_SPELLING, position);
    Sci_Position end = (Sci_Position)SendMessageA(hwnd, SCI_INDICATOREND, INDICATOR_SPELLING, position);
    if (start < 0 || end <= start) return 0;
    char *utf8Word = get_text_range(hwnd, start, end);
    if (!utf8Word) return 1;
    WCHAR *word = to_wide(utf8Word);
    free(utf8Word);
    if (!word) return 1;

    enum {
        SPELL_SUGGESTION_FIRST = 1, SPELL_SUGGESTION_LAST = 8,
        SPELL_IGNORE = 20, SPELL_ADD = 21,
        SPELL_UNDO = 30, SPELL_CUT = 31, SPELL_COPY = 32,
        SPELL_PASTE = 33, SPELL_SELECT_ALL = 34
    };
    WCHAR *suggestions[SPELL_SUGGESTION_LAST] = {0};
    int suggestionCount = 0;
    IEnumString *suggestionEnum = NULL;
    if (SUCCEEDED(ISpellChecker_Suggest(g_spellChecker, word, &suggestionEnum)) && suggestionEnum) {
        while (suggestionCount < SPELL_SUGGESTION_LAST) {
            LPOLESTR suggestion = NULL;
            ULONG fetched = 0;
            if (IEnumString_Next(suggestionEnum, 1, &suggestion, &fetched) != S_OK || !suggestion)
                break;
            suggestions[suggestionCount++] = suggestion;
        }
        IEnumString_Release(suggestionEnum);
    }

    HMENU menu = CreatePopupMenu();
    for (int i = 0; i < suggestionCount; i++)
        AppendMenuW(menu, MF_STRING, SPELL_SUGGESTION_FIRST + i, suggestions[i]);
    if (suggestionCount == 0)
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"(No suggestions)");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, SPELL_IGNORE, L"Ignore Word");
    AppendMenuW(menu, MF_STRING, SPELL_ADD, L"Add to Dictionary");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (SendMessageA(hwnd, SCI_CANUNDO, 0, 0) ? 0 : MF_GRAYED),
                SPELL_UNDO, L"Undo");
    AppendMenuW(menu, MF_STRING, SPELL_CUT, L"Cut");
    AppendMenuW(menu, MF_STRING, SPELL_COPY, L"Copy");
    AppendMenuW(menu, MF_STRING, SPELL_PASTE, L"Paste");
    AppendMenuW(menu, MF_STRING, SPELL_SELECT_ALL, L"Select All");

    int itemIds[EDITOR_CONTEXT_MENU_PLUGIN_ITEM_MAX];
    int itemCount = append_plugin_context_menu_items(menu, itemIds, hwnd);

    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  screenPoint.x, screenPoint.y, 0, g_hMainWnd, NULL);
    DestroyMenu(menu);
    if (run_plugin_context_menu_command(command, itemIds, itemCount, hwnd)) {
        /* handled by the plugin */
    } else if (command >= SPELL_SUGGESTION_FIRST && command <= (UINT)suggestionCount) {
        char *replacement = to_utf8(suggestions[command - SPELL_SUGGESTION_FIRST]);
        if (replacement) {
            SendMessageA(hwnd, SCI_BEGINUNDOACTION, 0, 0);
            SendMessageA(hwnd, SCI_SETTARGETSTART, (WPARAM)start, 0);
            SendMessageA(hwnd, SCI_SETTARGETEND, (WPARAM)end, 0);
            Sci_Position inserted = (Sci_Position)SendMessageA(hwnd, SCI_REPLACETARGET,
                                              (WPARAM)-1, (LPARAM)replacement);
            SendMessageA(hwnd, SCI_SETSEL, (WPARAM)(start + inserted),
                         (LPARAM)(start + inserted));
            SendMessageA(hwnd, SCI_ENDUNDOACTION, 0, 0);
            free(replacement);
            schedule_spell_check();
        }
    } else if (command == SPELL_IGNORE) {
        ISpellChecker_Ignore(g_spellChecker, word);
        schedule_spell_check();
    } else if (command == SPELL_ADD) {
        ISpellChecker_Add(g_spellChecker, word);
        schedule_spell_check();
    } else if (command == SPELL_UNDO) SendMessageA(hwnd, SCI_UNDO, 0, 0);
    else if (command == SPELL_CUT) SendMessageA(hwnd, SCI_CUT, 0, 0);
    else if (command == SPELL_COPY) SendMessageA(hwnd, SCI_COPY, 0, 0);
    else if (command == SPELL_PASTE) SendMessageA(hwnd, SCI_PASTE, 0, 0);
    else if (command == SPELL_SELECT_ALL) SendMessageA(hwnd, SCI_SELECTALL, 0, 0);

    for (int i = 0; i < suggestionCount; i++) CoTaskMemFree(suggestions[i]);
    free(word);
    return 1;
}

/* Ctrl+Alt+click jumps to the next occurrence of the word under the pointer -
   a lightweight stand-in for "go to definition" without a real per-language
   symbol index: for an identifier this typically lands on its declaration,
   and repeated Ctrl+Alt+clicks cycle through every other use in the file.
   (Plain Ctrl+click is already Scintilla's native gesture for adding a
   multi-cursor selection - see SCI_SETMULTIPLESELECTION in configure_scintilla()
   - so this needs its own modifier combination instead.)
   Returns 0 (unhandled, so the caller falls back to a normal click) when
   there's no word under the pointer to jump from. */
static int ctrl_click_jump_to_occurrence(HWND hEdit, int x, int y) {
    Sci_Position position = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMPOINTCLOSE, (WPARAM)x, (LPARAM)y);
    if (position < 0) return 0;

    Sci_Position wordStart = (Sci_Position)SendMessageA(hEdit, SCI_WORDSTARTPOSITION, (WPARAM)position, (LPARAM)1);
    Sci_Position wordEnd = (Sci_Position)SendMessageA(hEdit, SCI_WORDENDPOSITION, (WPARAM)position, (LPARAM)1);
    if (wordEnd <= wordStart) return 0;

    Sci_Position wordLength = wordEnd - wordStart;
    char *word = get_text_range(hEdit, wordStart, wordEnd);
    if (!word) return 1;

    Sci_Position docLength = (Sci_Position)SendMessageA(hEdit, SCI_GETLENGTH, 0, 0);
    SendMessageA(hEdit, SCI_SETSEARCHFLAGS, SCFIND_WHOLEWORD | SCFIND_MATCHCASE, 0);

    /* Search past the clicked word first, wrapping back to the start of the
       document if nothing turns up after it - the same wrap-around shape
       Find Next uses (search_find() in text_search.c). */
    SendMessageA(hEdit, SCI_SETTARGETSTART, (WPARAM)wordEnd, 0);
    SendMessageA(hEdit, SCI_SETTARGETEND, (WPARAM)docLength, 0);
    Sci_Position foundAt = (Sci_Position)SendMessageA(hEdit, SCI_SEARCHINTARGET, (WPARAM)wordLength, (LPARAM)word);
    if (foundAt < 0) {
        SendMessageA(hEdit, SCI_SETTARGETSTART, 0, 0);
        SendMessageA(hEdit, SCI_SETTARGETEND, (WPARAM)wordStart, 0);
        foundAt = (Sci_Position)SendMessageA(hEdit, SCI_SEARCHINTARGET, (WPARAM)wordLength, (LPARAM)word);
    }
    free(word);

    if (foundAt >= 0) {
        Sci_Position foundEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETTARGETEND, 0, 0);
        SendMessageA(hEdit, SCI_SETSEL, (WPARAM)foundAt, (LPARAM)foundEnd);
        SendMessageA(hEdit, SCI_SCROLLCARET, 0, 0);
    } else {
        MessageBeep(MB_ICONASTERISK); /* only occurrence in the file */
    }
    return 1;
}

/* The smart_* handlers read and edit only the main selection, so several
   carets or a rectangular selection keep Scintilla's own multi-caret typing. */
static int has_single_stream_selection(HWND hEdit) {
    return SendMessageA(hEdit, SCI_GETSELECTIONS, 0, 0) == 1 &&
        !SendMessageA(hEdit, SCI_SELECTIONISRECTANGLE, 0, 0);
}

static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR id, DWORD_PTR data) {
    (void)data;
    if (msg == WM_CHAR && has_single_stream_selection(hwnd)) {
        if (wParam == '\t' && smart_tab(hwnd, (GetKeyState(VK_SHIFT) & 0x8000) != 0)) return 0;
        if (wParam == '\r' && smart_newline(hwnd)) return 0;
        if (wParam == '\b' && smart_pair_backspace(hwnd)) return 0;
        if (wParam < 128 && smart_closing_indent(hwnd, (char)wParam)) return 0;
        if (wParam < 128 && smart_pair_character(hwnd, (char)wParam)) return 0;
    } else if (msg == WM_CONTEXTMENU) {
        if (show_spelling_context_menu(hwnd, lParam)) return 0;
        show_editor_context_menu(hwnd, lParam);
        return 0;
    } else if (msg == WM_LBUTTONDOWN) {
        if ((wParam & MK_CONTROL) && (GetKeyState(VK_MENU) & 0x8000) &&
            ctrl_click_jump_to_occurrence(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)))
            return 0;
    } else if (msg == WM_NCDESTROY) {
        /* This proc is shared by both g_hEdit (subclass id 2) and
           g_hSplitEdit (id 3) - remove whichever one this hwnd was actually
           registered under rather than a hardcoded id. */
        RemoveWindowSubclass(hwnd, EditSubclassProc, id);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void move_document_tab(int from, int to) {
    if (from < 0 || to < 0 || from >= g_documentStore.count || to >= g_documentStore.count || from == to) return;
    save_active_document_view();
    Document moving = g_documentStore.documents[from];
    if (from < to) {
        for (int i = from; i < to; i++) g_documentStore.documents[i] = g_documentStore.documents[i + 1];
    } else {
        for (int i = from; i > to; i--) g_documentStore.documents[i] = g_documentStore.documents[i - 1];
    }
    g_documentStore.documents[to] = moving;
    if (g_documentStore.active == from) g_documentStore.active = to;
    else if (from < g_documentStore.active && to >= g_documentStore.active) g_documentStore.active--;
    else if (from > g_documentStore.active && to <= g_documentStore.active) g_documentStore.active++;

    TabCtrl_DeleteItem(g_hTabs, from);
    TCITEMW item = {0};
    item.mask = TCIF_TEXT; item.pszText = L"";
    SendMessageW(g_hTabs, TCM_INSERTITEMW, (WPARAM)to, (LPARAM)&item);
    for (int i = 0; i < g_documentStore.count; i++) update_tab_label(i);
    TabCtrl_SetCurSel(g_hTabs, g_documentStore.active);
    update_title();
}

static void do_save_tab_at(int index) {
    if (index < 0 || index >= g_documentStore.count) return;
    int restoreSplitFocus = GetFocus() == g_hSplitEdit;
    int original = g_documentStore.active;
    if (index != original) switch_document(index);
    do_save();
    if (index != original && original < g_documentStore.count) switch_document(original);
    if (restoreSplitFocus && g_splitEditorVisible) SetFocus(g_hSplitEdit);
}

static void do_saveas_tab_at(int index) {
    if (index < 0 || index >= g_documentStore.count) return;
    int restoreSplitFocus = GetFocus() == g_hSplitEdit;
    int original = g_documentStore.active;
    if (index != original) switch_document(index);
    do_saveas();
    if (index != original && original < g_documentStore.count) switch_document(original);
    if (restoreSplitFocus && g_splitEditorVisible) SetFocus(g_hSplitEdit);
}

/* do_close_document_at() shifts every document after the one it removes down
   by one index, so these close multiple tabs from the highest index down -
   that way an index not yet visited never moves before its turn. */
static void do_close_other_tabs(int keep) {
    if (keep < 0 || keep >= g_documentStore.count) return;
    for (int i = g_documentStore.count - 1; i >= 0; i--) {
        if (i == keep || g_documentStore.documents[i].pinned) continue;
        do_close_document_at(i);
    }
}

static void do_close_tabs_to_right(int index) {
    for (int i = g_documentStore.count - 1; i > index; i--)
        if (!g_documentStore.documents[i].pinned) do_close_document_at(i);
}

static void do_close_all_tabs(void) {
    for (int i = g_documentStore.count - 1; i >= 0; i--)
        if (!g_documentStore.documents[i].pinned) do_close_document_at(i);
}

static void copy_text_to_clipboard(const WCHAR *text) {
    size_t bytes = (wcslen(text) + 1) * sizeof(WCHAR);
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!global) return;
    void *locked = GlobalLock(global);
    if (!locked) { GlobalFree(global); return; }
    memcpy(locked, text, bytes);
    GlobalUnlock(global);
    if (!OpenClipboard(g_hMainWnd)) { GlobalFree(global); return; }
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, global);
    CloseClipboard();
}

typedef struct CommandPaletteEntry {
    WORD command;
    int enabled;
    WCHAR *label;
} CommandPaletteEntry;

#define COMMAND_PALETTE_RECENT_MAX 16
static WORD g_commandPaletteRecent[COMMAND_PALETTE_RECENT_MAX];
static int g_commandPaletteRecentCount = 0;

static int command_palette_recent_rank(WORD command) {
    for (int i = 0; i < g_commandPaletteRecentCount; i++)
        if (g_commandPaletteRecent[i] == command) return i;
    return COMMAND_PALETTE_RECENT_MAX;
}

static void remember_palette_command(WORD command) {
    int found = command_palette_recent_rank(command);
    if (found >= g_commandPaletteRecentCount) {
        if (g_commandPaletteRecentCount < COMMAND_PALETTE_RECENT_MAX)
            g_commandPaletteRecentCount++;
        found = g_commandPaletteRecentCount - 1;
    }
    for (int i = found; i > 0; i--) g_commandPaletteRecent[i] = g_commandPaletteRecent[i - 1];
    g_commandPaletteRecent[0] = command;
}

static WCHAR *palette_clean_label(const char *utf8) {
    WCHAR *source = to_wide(utf8 ? utf8 : "");
    if (!source) return NULL;
    size_t length = wcslen(source);
    WCHAR *clean = (WCHAR *)malloc((length + 1) * sizeof(WCHAR));
    if (!clean) { free(source); return NULL; }
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        if (source[i] == L'&') {
            if (source[i + 1] == L'&') clean[out++] = source[i++];
            continue;
        }
        clean[out++] = source[i];
    }
    clean[out] = 0;
    free(source);
    return clean;
}

static WCHAR *palette_join(const WCHAR *prefix, const WCHAR *label) {
    size_t prefixLength = prefix ? wcslen(prefix) : 0;
    size_t labelLength = label ? wcslen(label) : 0;
    size_t capacity = prefixLength + labelLength + (prefixLength ? 4 : 1);
    WCHAR *result = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (result) {
        if (prefixLength) swprintf(result, capacity, L"%ls > %ls", prefix, label ? label : L"");
        else swprintf(result, capacity, L"%ls", label ? label : L"");
    }
    return result;
}

static WCHAR *palette_find_in_menu(const PluginMenuItem *items, int count,
                                   const char *name, const WCHAR *prefix) {
    for (int i = 0; i < count; i++) {
        const PluginMenuItem *item = &items[i];
        WCHAR *label = item->label ? palette_clean_label(item->label) : NULL;
        WCHAR *path = label ? palette_join(prefix, label) : NULL;
        free(label);
        if (item->kind == PLUGIN_MENU_COMMAND && item->name &&
            strcmp(item->name, name) == 0) return path;
        if (item->kind == PLUGIN_MENU_SUBMENU && path) {
            WCHAR *found = palette_find_in_menu(item->items, item->itemCount, name, path);
            if (found) { free(path); return found; }
        }
        free(path);
    }
    return NULL;
}

static WCHAR *palette_builtin_label(const MenuCommand *command) {
    int count = 0;
    const PluginMenuItem *menus = plugin_host_menu(&count);
    WCHAR *found = palette_find_in_menu(menus, count, command->name, NULL);
    if (found) return found;

    WCHAR *fallback = to_wide(command->name);
    if (!fallback) return NULL;
    int capitalize = 1;
    for (WCHAR *p = fallback; *p; p++) {
        if (*p == L'.') {
            *p = L':';
            capitalize = 1;
        } else if (*p == L'_') {
            *p = L' ';
            capitalize = 1;
        } else if (capitalize && iswalpha(*p)) {
            *p = towupper(*p);
            capitalize = 0;
        }
    }
    return fallback;
}

static WCHAR *palette_plugin_label(int index) {
    WCHAR *source = palette_clean_label(plugin_host_command_label(index));
    if (!source) return NULL;
    int menuBarPath = source[0] == L'/';
    size_t capacity = wcslen(source) * 3 + 1;
    WCHAR *label = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!label) { free(source); return NULL; }
    WCHAR *read = source + (menuBarPath ? 1 : 0), *write = label;
    while (*read) {
        if (*read == L'/') {
            memcpy(write, L" > ", 3 * sizeof(WCHAR));
            write += 3;
            read++;
        } else {
            *write++ = *read++;
        }
    }
    *write = 0;
    free(source);
    if (menuBarPath) return label;
    WCHAR *prefixed = palette_join(L"Plugins", label);
    free(label);
    return prefixed;
}

static WCHAR *palette_decorate_label(WCHAR *base, const char *shortcut,
                                     const CommandState *state) {
    WCHAR *wideShortcut = shortcut ? to_wide(shortcut) : NULL;
    size_t capacity = (base ? wcslen(base) : 0) +
        (wideShortcut ? wcslen(wideShortcut) : 0) + 48;
    WCHAR *label = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (label) swprintf(label, capacity, L"%ls%ls%ls%ls",
        state->checked ? L"✓ " : L"", base ? base : L"Command",
        wideShortcut ? L"    " : L"", wideShortcut ? wideShortcut : L"");
    if (label && !state->enabled) wcscat(label, L"    [unavailable]");
    free(wideShortcut);
    free(base);
    return label;
}

static void show_command_palette(void) {
    int pluginCount = plugin_host_command_count(), builtInCount = 0;
    while (menu_command_at(builtInCount)) builtInCount++;
    int capacity = builtInCount + pluginCount;
    CommandPaletteEntry *entries = (CommandPaletteEntry *)calloc(
        (size_t)capacity, sizeof(CommandPaletteEntry));
    WCHAR **labels = (WCHAR **)calloc((size_t)capacity, sizeof(WCHAR *));
    if (!entries || !labels) { free(entries); free(labels); return; }

    int count = 0;
    for (int i = 0; i < builtInCount; i++) {
        const MenuCommand *command = menu_command_at(i);
        CommandState state;
        command_state(command->id, &state);
        WCHAR *label = palette_decorate_label(palette_builtin_label(command),
                                               command->shortcut, &state);
        if (!label) continue;
        entries[count] = (CommandPaletteEntry){ command->id, state.enabled, label };
        labels[count++] = label;
    }
    for (int i = 0; i < pluginCount; i++) {
        if (plugin_host_command_is_separator(i) || plugin_host_command_overrides(i)) continue;
        CommandState state;
        command_state(IDM_PLUGIN_COMMAND_0 + (UINT)i, &state);
        WCHAR *label = palette_decorate_label(palette_plugin_label(i),
                                               plugin_host_command_shortcut(i), &state);
        if (!label) continue;
        entries[count] = (CommandPaletteEntry){ (WORD)(IDM_PLUGIN_COMMAND_0 + i),
                                                state.enabled, label };
        labels[count++] = label;
    }

    /* Commands chosen through the palette bubble to the top for the rest of
       the session; everything else retains registry/menu order. */
    for (int i = 1; i < count; i++) {
        CommandPaletteEntry entry = entries[i];
        int rank = command_palette_recent_rank(entry.command), at = i;
        while (at > 0 && command_palette_recent_rank(entries[at - 1].command) > rank) {
            entries[at] = entries[at - 1];
            at--;
        }
        entries[at] = entry;
    }
    for (int i = 0; i < count; i++) labels[i] = entries[i].label;

    HWND previousFocus = GetFocus();
    int selected = show_choice_dialog(L"Command Palette",
        L"Type a command, menu path, or shortcut:", labels, count, L"Run");
    WORD command = selected >= 0 && selected < count ? entries[selected].command : 0;
    int enabled = selected >= 0 && selected < count ? entries[selected].enabled : 0;
    for (int i = 0; i < count; i++) free(entries[i].label);
    free(labels);
    free(entries);
    if (command && enabled) {
        remember_palette_command(command);
        if (previousFocus && IsWindow(previousFocus)) SetFocus(previousFocus);
        handle_main_command(g_hMainWnd, command);
    } else if (command) {
        MessageBeep(MB_ICONASTERISK);
    }
}

static void show_tab_context_menu(int index, POINT screenPoint) {
    enum {
        TAB_CONTEXT_SAVE = 1, TAB_CONTEXT_SAVEAS, TAB_CONTEXT_PIN, TAB_CONTEXT_CLOSE,
        TAB_CONTEXT_CLOSE_OTHERS, TAB_CONTEXT_CLOSE_RIGHT, TAB_CONTEXT_CLOSE_ALL,
        TAB_CONTEXT_COPY_PATH, TAB_CONTEXT_REVEAL, TAB_CONTEXT_SPLIT
    };
    if (index < 0 || index >= g_documentStore.count) return;
    const WCHAR *path = g_documentStore.documents[index].path;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, TAB_CONTEXT_SAVE, L"Save");
    AppendMenuW(menu, MF_STRING, TAB_CONTEXT_SAVEAS, L"Save As...");
    AppendMenuW(menu, MF_STRING, TAB_CONTEXT_PIN,
                g_documentStore.documents[index].pinned ? L"Unpin Tab" : L"Pin Tab");
    AppendMenuW(menu, MF_STRING |
                ((g_documentStore.documents[index].viewKind == DOCUMENT_VIEW_TEXT &&
                  !g_documentStore.documents[index].largeFile) ? 0 : MF_GRAYED),
                TAB_CONTEXT_SPLIT, L"Show in Editor Split");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_documentStore.documents[index].pinned ? MF_GRAYED : 0),
                TAB_CONTEXT_CLOSE, L"Close");
    AppendMenuW(menu, MF_STRING | (g_documentStore.count > 1 ? 0 : MF_GRAYED),
                TAB_CONTEXT_CLOSE_OTHERS, L"Close Others");
    AppendMenuW(menu, MF_STRING | (index < g_documentStore.count - 1 ? 0 : MF_GRAYED),
                TAB_CONTEXT_CLOSE_RIGHT, L"Close Tabs to the Right");
    AppendMenuW(menu, MF_STRING, TAB_CONTEXT_CLOSE_ALL, L"Close All");
    if (path) {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, TAB_CONTEXT_COPY_PATH, L"Copy Path");
        AppendMenuW(menu, MF_STRING, TAB_CONTEXT_REVEAL, L"Reveal in Explorer");
    }

    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  screenPoint.x, screenPoint.y, 0, g_hMainWnd, NULL);
    DestroyMenu(menu);

    switch (command) {
        case TAB_CONTEXT_SAVE: do_save_tab_at(index); break;
        case TAB_CONTEXT_SAVEAS: do_saveas_tab_at(index); break;
        case TAB_CONTEXT_PIN: toggle_document_pinned(index); break;
        case TAB_CONTEXT_SPLIT: set_split_document(index); break;
        case TAB_CONTEXT_CLOSE: do_close_document_at(index); break;
        case TAB_CONTEXT_CLOSE_OTHERS: do_close_other_tabs(index); break;
        case TAB_CONTEXT_CLOSE_RIGHT: do_close_tabs_to_right(index); break;
        case TAB_CONTEXT_CLOSE_ALL: do_close_all_tabs(); break;
        case TAB_CONTEXT_COPY_PATH:
            if (index < g_documentStore.count && g_documentStore.documents[index].path)
                copy_text_to_clipboard(g_documentStore.documents[index].path);
            break;
        case TAB_CONTEXT_REVEAL:
            if (index < g_documentStore.count && g_documentStore.documents[index].path)
                reveal_path_in_explorer(g_documentStore.documents[index].path);
            break;
    }
}

static LRESULT CALLBACK TabSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR subclassId, DWORD_PTR refData) {
    (void)subclassId; (void)refData;
    if (msg == WM_ERASEBKGND) {
        return 1; /* WM_PAINT fills the whole strip from a back buffer */
    } else if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        if (client.right <= 0 || client.bottom <= 0) {
            EndPaint(hwnd, &ps);
            return 0;
        }

        /* Draw the strip and every tab into a back buffer: the tabs are
           owner-drawn one at a time, which visibly flickers on-screen. */
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = buffer ? CreateCompatibleBitmap(dc, client.right, client.bottom) : NULL;
        if (!buffer || !bitmap) {
            if (bitmap) DeleteObject(bitmap);
            if (buffer) DeleteDC(buffer);
            EndPaint(hwnd, &ps);
            return 0;
        }
        HBITMAP oldBitmap = (HBITMAP)SelectObject(buffer, bitmap);

        /* The recessed strip behind the tabs, matching the editor's
           line-number gutter immediately below it. */
        fill_rect_color(buffer, &client, theme_palette()->subtleBg);

        int selected = TabCtrl_GetCurSel(hwnd);
        for (int i = 0; i < g_documentStore.count; i++) {
            RECT itemRect;
            if (!TabCtrl_GetItemRect(hwnd, i, &itemRect)) continue;
            DRAWITEMSTRUCT dis = {0};
            dis.CtlType = ODT_TAB;
            dis.CtlID = ID_TABS;
            dis.itemID = (UINT)i;
            dis.itemAction = ODA_DRAWENTIRE;
            dis.itemState = i == selected ? ODS_SELECTED : 0;
            dis.hwndItem = hwnd;
            dis.hDC = buffer;
            dis.rcItem = itemRect;
            draw_tab_item(&dis);
        }

        BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(hwnd, &ps);
        return 0;
    } else if (msg == WM_LBUTTONDOWN) {
        TCHITTESTINFO hit = {0};
        hit.pt.x = GET_X_LPARAM(lParam); hit.pt.y = GET_Y_LPARAM(lParam);
        g_tabDragIndex = TabCtrl_HitTest(hwnd, &hit);
        g_tabDragged = 0;
        if (g_tabDragIndex >= 0) SetCapture(hwnd);
    } else if (msg == WM_MOUSEMOVE && g_tabDragIndex >= 0 && (wParam & MK_LBUTTON)) {
        TCHITTESTINFO hit = {0};
        hit.pt.x = GET_X_LPARAM(lParam); hit.pt.y = GET_Y_LPARAM(lParam);
        int target = TabCtrl_HitTest(hwnd, &hit);
        if (target >= 0 && target != g_tabDragIndex) {
            move_document_tab(g_tabDragIndex, target);
            g_tabDragIndex = target;
            g_tabDragged = 1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    } else if (msg == WM_LBUTTONUP || msg == WM_MBUTTONUP) {
        if (msg == WM_LBUTTONUP && g_tabDragIndex >= 0) {
            ReleaseCapture();
            g_tabDragIndex = -1;
            if (g_tabDragged) { g_tabDragged = 0; return 0; }
        }
        TCHITTESTINFO hit = {0};
        hit.pt.x = GET_X_LPARAM(lParam);
        hit.pt.y = GET_Y_LPARAM(lParam);
        int index = TabCtrl_HitTest(hwnd, &hit);
        if (index >= 0) {
            RECT tabRect;
            TabCtrl_GetItemRect(hwnd, index, &tabRect);
            RECT close = tab_close_rectangle(&tabRect);
            POINT point = hit.pt;
            if ((msg == WM_MBUTTONUP || PtInRect(&close, point)) &&
                !g_documentStore.documents[index].pinned) {
                PostMessageW(g_hMainWnd, WM_APP_CLOSE_TAB, (WPARAM)index, 0);
                return 0;
            }
        }
    } else if (msg == WM_CONTEXTMENU) {
        POINT screenPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int index;
        if (screenPoint.x == -1 && screenPoint.y == -1) {
            /* Keyboard-invoked (Shift+F10 / context menu key): no cursor
               position, so anchor the menu under the selected tab instead. */
            index = TabCtrl_GetCurSel(hwnd);
            RECT itemRect;
            if (index >= 0 && TabCtrl_GetItemRect(hwnd, index, &itemRect)) {
                screenPoint.x = itemRect.left;
                screenPoint.y = itemRect.bottom;
                ClientToScreen(hwnd, &screenPoint);
            }
        } else {
            POINT clientPoint = screenPoint;
            ScreenToClient(hwnd, &clientPoint);
            TCHITTESTINFO hit = {0};
            hit.pt = clientPoint;
            index = TabCtrl_HitTest(hwnd, &hit);
        }
        if (index >= 0) show_tab_context_menu(index, screenPoint);
        return 0;
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, TabSubclassProc, 1);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

/* explorer_init()'s callback: file_explorer.c cannot call open_file_in_tab()
   by name since it's static to this translation unit. */
