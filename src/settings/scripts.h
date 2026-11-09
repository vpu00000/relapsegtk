#pragma once

#include <glib.h>

char *scripts_directory_default(void);
char *scripts_settings_load(GError **error);
gboolean scripts_settings_save(const char *path, GError **error);
