#pragma once

#include <gtk/gtk.h>

typedef void (*ScriptOpenFunc)(GFile *file, const char *display_name, gpointer user_data);
typedef gboolean (*ScriptMoveFunc)(GFile *file, GFile *destination, GError **error, gpointer user_data);
typedef gboolean (*ScriptDirectoryFunc)(const char *path, GError **error, gpointer user_data);

GtkWidget *scripts_new(const char *path, ScriptOpenFunc open_script,
                       ScriptMoveFunc move_script, ScriptDirectoryFunc directory_changed,
                       gpointer user_data);
void scripts_set_directory(GtkWidget *scripts, const char *path);
void scripts_set_active_file(GtkWidget *scripts, GFile *file);
void scripts_shutdown(GtkWidget *scripts);
