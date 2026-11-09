#pragma once

#include <gtk/gtk.h>

typedef struct {
    void (*execute)(gpointer user_data);
    void (*open_script)(GFile *file, const char *display_name, gpointer user_data);
    void (*save_active)(gpointer user_data);
    void (*delete_active)(gpointer user_data);
} ToolbarHandlers;

GtkWidget *toolbar_new(GtkWindow *parent, const char *scripts_path, GtkWidget *socket_picker,
                       const ToolbarHandlers *handlers, gpointer user_data);
void toolbar_set_execute_enabled(GtkWidget *toolbar, gboolean enabled);
void toolbar_set_active_enabled(GtkWidget *toolbar, gboolean enabled);
void toolbar_set_status(GtkWidget *toolbar, const char *label, const char *description, gboolean ready);
void toolbar_set_scripts_path(GtkWidget *toolbar, const char *path);
void toolbar_shutdown(GtkWidget *toolbar);
