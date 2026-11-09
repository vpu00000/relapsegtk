#pragma once

#include <gtk/gtk.h>
#include "settings/editor.h"

typedef void (*TabsChangedFunc)(GtkWidget *tabs, gpointer user_data);

GtkWidget *tabs_new(const char *scripts_path, TabsChangedFunc changed, gpointer user_data);
void tabs_open(GtkWidget *tabs, GFile *file, const char *display_name);
GFile *tabs_rename(GtkWidget *tabs, GFile *file, const char *name, GError **error);
void tabs_save(GtkWidget *tabs);
void tabs_find(GtkWidget *tabs);
void tabs_find_next(GtkWidget *tabs, gboolean backward);
void tabs_set_editor_settings(GtkWidget *tabs, const EditorSettings *settings);
void tabs_set_scripts_path(GtkWidget *tabs, const char *path);
gboolean tabs_restore_session(GtkWidget *tabs, GError **error);
void tabs_set_session_enabled(GtkWidget *tabs, gboolean enabled);

GtkTextBuffer *tabs_get_buffer(GtkWidget *tabs);
const char *tabs_get_name(GtkWidget *tabs);
GFile *tabs_get_current_file(GtkWidget *tabs);
void tabs_close_current(GtkWidget *tabs);

void tabs_shutdown(GtkWidget *tabs);
