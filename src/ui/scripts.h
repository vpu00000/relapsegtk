#pragma once

#include <gtk/gtk.h>

typedef void (*ScriptOpenFunc)(GFile *file, const char *display_name, gpointer user_data);
typedef GFile *(*ScriptRenameFunc)(GFile *file, const char *name, GError **error, gpointer user_data);

GtkWidget *scripts_new(const char *path, ScriptOpenFunc open_script,
                       ScriptRenameFunc rename_script, gpointer user_data);
void scripts_set_directory(GtkWidget *scripts, const char *path);
