#ifndef SETTINGS_IO_H
#define SETTINGS_IO_H

#include "settings_values.h"

/* Reading and writing the normal per-user settings, or portable-data when an
   executable-local portable.mode marker is present. */

WCHAR *get_settings_path(void);
/* Heap copy of an arbitrarily long string value (caller frees), or NULL
   when the key is absent or empty. */
WCHAR *read_profile_string(const WCHAR *section, const WCHAR *key, const WCHAR *path);
int read_setting_int(const WCHAR *section, const WCHAR *key, int defaultValue,
                     const WCHAR *path);
int write_setting_string(const WCHAR *section, const WCHAR *key, const WCHAR *value,
                         const WCHAR *path);
int clear_setting_section(const WCHAR *section, const WCHAR *path);

void load_font_settings(void);
void save_font_settings(void);
void load_theme_settings(void);
void save_theme_settings(void);
void load_layout_settings(void);
void save_layout_settings(void);
void load_find_settings(void);
void save_find_settings(void);
void load_compiler_settings(void);
void save_compiler_settings(void);
void load_git_settings(void);
void save_git_settings(void);

#endif
