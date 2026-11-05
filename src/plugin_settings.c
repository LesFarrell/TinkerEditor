#include "plugin_settings.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "file_io.h"

#define PLUGIN_SETTINGS_MAX_BYTES (1024 * 1024)
#define PLUGIN_SETTINGS_MAX_ENTRIES 4096

typedef struct SettingEntry {
    char *plugin;
    char *key;
    char *value;
    struct SettingEntry *next;
} SettingEntry;

typedef struct {
    const char *at;
    const char *end;
    int failed;
} JsonReader;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} JsonWriter;

static char *copy_string(const char *value) {
    size_t length = strlen(value);
    char *copy = (char *)malloc(length + 1);
    if (copy) memcpy(copy, value, length + 1);
    return copy;
}

static void free_entries(SettingEntry *entry) {
    while (entry) {
        SettingEntry *next = entry->next;
        free(entry->plugin);
        free(entry->key);
        free(entry->value);
        free(entry);
        entry = next;
    }
}

static SettingEntry *find_entry(SettingEntry *entries, const char *plugin, const char *key) {
    for (SettingEntry *entry = entries; entry; entry = entry->next)
        if (strcmp(entry->plugin, plugin) == 0 && strcmp(entry->key, key) == 0) return entry;
    return NULL;
}

static int put_entry(SettingEntry **entries, const char *plugin, const char *key,
                     const char *value, int *count) {
    SettingEntry *existing = find_entry(*entries, plugin, key);
    char *copy = copy_string(value);
    if (!copy) return 0;
    if (existing) {
        free(existing->value);
        existing->value = copy;
        return 1;
    }
    if (*count >= PLUGIN_SETTINGS_MAX_ENTRIES) { free(copy); return 0; }
    SettingEntry *entry = (SettingEntry *)calloc(1, sizeof(*entry));
    if (!entry) { free(copy); return 0; }
    entry->plugin = copy_string(plugin);
    entry->key = copy_string(key);
    entry->value = copy;
    if (!entry->plugin || !entry->key) {
        free(entry->plugin); free(entry->key); free(entry->value); free(entry);
        return 0;
    }
    SettingEntry **tail = entries;
    while (*tail) tail = &(*tail)->next;
    *tail = entry;
    (*count)++;
    return 1;
}

static void skip_space(JsonReader *reader) {
    while (reader->at < reader->end &&
           (*reader->at == ' ' || *reader->at == '\t' ||
            *reader->at == '\r' || *reader->at == '\n')) reader->at++;
}

static int take(JsonReader *reader, char expected) {
    skip_space(reader);
    if (reader->at >= reader->end || *reader->at != expected) {
        reader->failed = 1;
        return 0;
    }
    reader->at++;
    return 1;
}

static int read_hex4(JsonReader *reader, uint32_t *value) {
    if (reader->end - reader->at < 4) return 0;
    uint32_t result = 0;
    for (int i = 0; i < 4; i++) {
        int digit = hex_digit_value((WCHAR)*reader->at++);
        if (digit < 0) return 0;
        result = result * 16u + (uint32_t)digit;
    }
    *value = result;
    return 1;
}

static int append_utf8(char *output, size_t capacity, size_t *length, uint32_t codepoint) {
    unsigned char bytes[4];
    size_t count;
    if (codepoint <= 0x7F) { bytes[0] = (unsigned char)codepoint; count = 1; }
    else if (codepoint <= 0x7FF) {
        bytes[0] = (unsigned char)(0xC0 | (codepoint >> 6));
        bytes[1] = (unsigned char)(0x80 | (codepoint & 0x3F)); count = 2;
    } else if (codepoint <= 0xFFFF) {
        bytes[0] = (unsigned char)(0xE0 | (codepoint >> 12));
        bytes[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | (codepoint & 0x3F)); count = 3;
    } else if (codepoint <= 0x10FFFF) {
        bytes[0] = (unsigned char)(0xF0 | (codepoint >> 18));
        bytes[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[3] = (unsigned char)(0x80 | (codepoint & 0x3F)); count = 4;
    } else return 0;
    if (*length + count >= capacity) return 0;
    memcpy(output + *length, bytes, count);
    *length += count;
    return 1;
}

static char *read_json_string(JsonReader *reader) {
    skip_space(reader);
    if (reader->at >= reader->end || *reader->at++ != '"') { reader->failed = 1; return NULL; }
    size_t capacity = (size_t)(reader->end - reader->at) + 1;
    char *output = (char *)malloc(capacity);
    if (!output) { reader->failed = 1; return NULL; }
    size_t length = 0;
    while (reader->at < reader->end) {
        unsigned char character = (unsigned char)*reader->at++;
        if (character == '"') { output[length] = 0; return output; }
        if (character < 0x20) break;
        if (character != '\\') { output[length++] = (char)character; continue; }
        if (reader->at >= reader->end) break;
        character = (unsigned char)*reader->at++;
        if (character == '"' || character == '\\' || character == '/') output[length++] = (char)character;
        else if (character == 'b') output[length++] = '\b';
        else if (character == 'f') output[length++] = '\f';
        else if (character == 'n') output[length++] = '\n';
        else if (character == 'r') output[length++] = '\r';
        else if (character == 't') output[length++] = '\t';
        else if (character == 'u') {
            uint32_t codepoint;
            if (!read_hex4(reader, &codepoint)) break;
            if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                uint32_t low;
                if (reader->end - reader->at < 6 || reader->at[0] != '\\' || reader->at[1] != 'u') break;
                reader->at += 2;
                if (!read_hex4(reader, &low) || low < 0xDC00 || low > 0xDFFF) break;
                codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
            } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) break;
            if (!append_utf8(output, capacity, &length, codepoint)) break;
        } else break;
    }
    free(output);
    reader->failed = 1;
    return NULL;
}

static SettingEntry *parse_settings(const char *json, size_t length, int *valid) {
    JsonReader reader = { json, json + length, 0 };
    SettingEntry *entries = NULL;
    int count = 0;
    if (!take(&reader, '{')) goto done;
    skip_space(&reader);
    if (reader.at < reader.end && *reader.at == '}') reader.at++;
    else for (;;) {
        char *plugin = read_json_string(&reader);
        if (!plugin || !take(&reader, ':') || !take(&reader, '{')) { free(plugin); goto done; }
        skip_space(&reader);
        if (reader.at < reader.end && *reader.at == '}') reader.at++;
        else for (;;) {
            char *key = read_json_string(&reader);
            if (!key || !take(&reader, ':')) { free(key); free(plugin); goto done; }
            char *value = read_json_string(&reader);
            if (!value || !put_entry(&entries, plugin, key, value, &count)) {
                free(key); free(value); free(plugin); reader.failed = 1; goto done;
            }
            free(key); free(value);
            skip_space(&reader);
            if (reader.at < reader.end && *reader.at == '}') { reader.at++; break; }
            if (!take(&reader, ',')) { free(plugin); goto done; }
        }
        free(plugin);
        skip_space(&reader);
        if (reader.at < reader.end && *reader.at == '}') { reader.at++; break; }
        if (!take(&reader, ',')) goto done;
    }
    skip_space(&reader);
    if (reader.at != reader.end) reader.failed = 1;
done:
    *valid = !reader.failed;
    if (reader.failed) { free_entries(entries); return NULL; }
    return entries;
}

static SettingEntry *load_settings(const WCHAR *path, int *valid) {
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        *valid = GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
        return NULL;
    }
    size_t length = 0;
    char *json = read_utf8_text_file_length(path, PLUGIN_SETTINGS_MAX_BYTES, &length);
    if (!json) { *valid = 0; return NULL; }
    SettingEntry *entries = parse_settings(json, length, valid);
    free(json);
    return entries;
}

static void writer_reserve(JsonWriter *writer, size_t extra) {
    if (writer->failed || extra > SIZE_MAX - writer->length - 1) { writer->failed = 1; return; }
    size_t needed = writer->length + extra + 1;
    if (needed <= writer->capacity) return;
    size_t capacity = writer->capacity ? writer->capacity : 256;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) { writer->failed = 1; return; }
        capacity *= 2;
    }
    char *grown = (char *)realloc(writer->data, capacity);
    if (!grown) { writer->failed = 1; return; }
    writer->data = grown;
    writer->capacity = capacity;
}

static void writer_text(JsonWriter *writer, const char *text) {
    size_t length = strlen(text);
    writer_reserve(writer, length);
    if (!writer->failed) { memcpy(writer->data + writer->length, text, length); writer->length += length; }
}

static void writer_char(JsonWriter *writer, char character) {
    writer_reserve(writer, 1);
    if (!writer->failed) writer->data[writer->length++] = character;
}

static void writer_string(JsonWriter *writer, const char *value) {
    static const char hex[] = "0123456789abcdef";
    writer_char(writer, '"');
    for (const unsigned char *at = (const unsigned char *)value; *at; at++) {
        switch (*at) {
            case '"': writer_text(writer, "\\\""); break;
            case '\\': writer_text(writer, "\\\\"); break;
            case '\b': writer_text(writer, "\\b"); break;
            case '\f': writer_text(writer, "\\f"); break;
            case '\n': writer_text(writer, "\\n"); break;
            case '\r': writer_text(writer, "\\r"); break;
            case '\t': writer_text(writer, "\\t"); break;
            default:
                if (*at < 0x20) {
                    char escape[] = "\\u00xx";
                    escape[4] = hex[*at >> 4]; escape[5] = hex[*at & 15];
                    writer_text(writer, escape);
                } else writer_char(writer, (char)*at);
        }
    }
    writer_char(writer, '"');
}

static int plugin_seen_before(SettingEntry *entries, SettingEntry *target) {
    for (SettingEntry *entry = entries; entry && entry != target; entry = entry->next)
        if (strcmp(entry->plugin, target->plugin) == 0) return 1;
    return 0;
}

static int save_settings(const WCHAR *path, SettingEntry *entries) {
    JsonWriter writer = {0};
    writer_text(&writer, "{\n");
    int firstPlugin = 1;
    for (SettingEntry *plugin = entries; plugin; plugin = plugin->next) {
        if (plugin_seen_before(entries, plugin)) continue;
        if (!firstPlugin) writer_text(&writer, ",\n");
        writer_text(&writer, "  "); writer_string(&writer, plugin->plugin); writer_text(&writer, ": {\n");
        int firstKey = 1;
        for (SettingEntry *entry = entries; entry; entry = entry->next) {
            if (strcmp(entry->plugin, plugin->plugin) != 0) continue;
            if (!firstKey) writer_text(&writer, ",\n");
            writer_text(&writer, "    "); writer_string(&writer, entry->key);
            writer_text(&writer, ": "); writer_string(&writer, entry->value);
            firstKey = 0;
        }
        writer_text(&writer, "\n  }");
        firstPlugin = 0;
    }
    writer_text(&writer, "\n}\n");
    if (!writer.failed) writer.data[writer.length] = 0;
    int ok = !writer.failed && write_file_atomic(path, writer.data, writer.length,
                                                  TEXT_ENCODING_UTF8, 0);
    free(writer.data);
    return ok;
}

/* Open batch, or NULL. plugin_settings_set() checks this before touching
   disk; plugin_settings_get()/plugin_settings_clear_section() ignore it and
   always read the file directly, so they still see a batch's pending writes
   only once it's been committed. */
static WCHAR *s_batchPath;
static SettingEntry *s_batchEntries;
static int s_batchValid;

void plugin_settings_begin_batch(const WCHAR *path) {
    if (s_batchPath) plugin_settings_end_batch();
    if (!path) return;
    s_batchPath = _wcsdup(path);
    if (!s_batchPath) return;
    s_batchEntries = load_settings(s_batchPath, &s_batchValid);
}

void plugin_settings_end_batch(void) {
    if (!s_batchPath) return;
    if (s_batchValid) save_settings(s_batchPath, s_batchEntries);
    free_entries(s_batchEntries);
    s_batchEntries = NULL;
    free(s_batchPath);
    s_batchPath = NULL;
    s_batchValid = 0;
}

char *plugin_settings_get(const WCHAR *path, const char *plugin, const char *key) {
    if (!path || !plugin || !key) return NULL;
    int valid = 0;
    SettingEntry *entries = load_settings(path, &valid);
    if (!valid) return NULL;
    SettingEntry *entry = find_entry(entries, plugin, key);
    char *value = entry ? copy_string(entry->value) : NULL;
    free_entries(entries);
    return value;
}

static int apply_setting(SettingEntry **entries, const char *plugin, const char *key,
                         const char *value, int *count) {
    int ok = 1;
    if (value) {
        ok = put_entry(entries, plugin, key, value, count);
    } else {
        SettingEntry **link = entries;
        while (*link && (strcmp((*link)->plugin, plugin) != 0 || strcmp((*link)->key, key) != 0))
            link = &(*link)->next;
        if (*link) {
            SettingEntry *removed = *link;
            *link = removed->next;
            removed->next = NULL;
            free_entries(removed);
        }
    }
    return ok;
}

int plugin_settings_set(const WCHAR *path, const char *plugin, const char *key,
                        const char *value) {
    if (!path || !plugin || !key) return 0;
    if (s_batchPath && wcscmp(path, s_batchPath) == 0) {
        if (!s_batchValid) return 0;
        int count = 0;
        for (SettingEntry *entry = s_batchEntries; entry; entry = entry->next) count++;
        return apply_setting(&s_batchEntries, plugin, key, value, &count);
    }
    int valid = 0, count = 0;
    SettingEntry *entries = load_settings(path, &valid);
    if (!valid) return 0;
    for (SettingEntry *entry = entries; entry; entry = entry->next) count++;
    int ok = apply_setting(&entries, plugin, key, value, &count);
    if (ok) ok = save_settings(path, entries);
    free_entries(entries);
    return ok;
}

int plugin_settings_clear_section(const WCHAR *path, const char *plugin) {
    if (!path || !plugin) return 0;
    int valid = 0;
    SettingEntry *entries = load_settings(path, &valid);
    if (!valid) return 0;
    SettingEntry **link = &entries;
    while (*link) {
        if (strcmp((*link)->plugin, plugin) == 0) {
            SettingEntry *removed = *link;
            *link = removed->next;
            removed->next = NULL;
            free_entries(removed);
        } else link = &(*link)->next;
    }
    int ok = save_settings(path, entries);
    free_entries(entries);
    return ok;
}

void plugin_settings_free_entries(PluginSettingEntry *entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; i++) {
        free(entries[i].section);
        free(entries[i].key);
        free(entries[i].value);
    }
    free(entries);
}

int plugin_settings_list(const WCHAR *path, const char *prefix,
                         PluginSettingEntry **result, int *resultCount) {
    if (!path || !prefix || !result || !resultCount) return 0;
    *result = NULL;
    *resultCount = 0;
    int valid = 0;
    SettingEntry *entries = load_settings(path, &valid);
    if (!valid) return 0;
    size_t prefixLength = strlen(prefix);
    int count = 0;
    for (SettingEntry *entry = entries; entry; entry = entry->next)
        if (strncmp(entry->plugin, prefix, prefixLength) == 0) count++;
    PluginSettingEntry *items = count
        ? (PluginSettingEntry *)calloc((size_t)count, sizeof(*items)) : NULL;
    if (count && !items) {
        free_entries(entries);
        return 0;
    }
    int index = 0, ok = 1;
    for (SettingEntry *entry = entries; entry && ok; entry = entry->next) {
        if (strncmp(entry->plugin, prefix, prefixLength) != 0) continue;
        items[index].section = copy_string(entry->plugin);
        items[index].key = copy_string(entry->key);
        items[index].value = copy_string(entry->value);
        ok = items[index].section && items[index].key && items[index].value;
        index++;
    }
    free_entries(entries);
    if (!ok) {
        plugin_settings_free_entries(items, count);
        return 0;
    }
    *result = items;
    *resultCount = count;
    return 1;
}
