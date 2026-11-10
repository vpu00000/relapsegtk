#pragma once

#include <gtk/gtk.h>
#include "settings/editor.h"

typedef struct {
    void (*execute)(gpointer user_data);
    void (*open_script)(GFile *file, const char *display_name, gpointer user_data);
    void (*active_script_changed)(gpointer user_data);
    gboolean (*editor_settings_changed)(const EditorSettings *settings, GError **error, gpointer user_data);
    gboolean (*restore_tabs_changed)(gboolean enabled, GError **error, gpointer user_data);
    gboolean (*output_changed)(gboolean enabled, GError **error, gpointer user_data);
    gboolean (*launcher_changed)(const char *path, GError **error, gpointer user_data);
    gboolean (*scripts_changed)(const char *path, GError **error, gpointer user_data);
    void (*socket_selected)(const char *const *paths, gpointer user_data);
    gboolean (*launch_profile)(const char *name, const char *place_id, const char *job_id,
                               GError **error, gpointer user_data);
    gboolean (*profile_running)(const char *name, gpointer user_data);
} WindowHandlers;

GtkWindow *window_new(GtkApplication *application, const char *scripts_path,
                      const WindowHandlers *handlers, gpointer user_data);
void window_set_execute_state(GtkWindow *window, gboolean enabled, guint ready, guint selected);
void window_set_status(GtkWindow *window, const char *label, const char *description, gboolean ready);
void window_open_script(GtkWindow *window, GFile *file, const char *display_name);
void window_set_editor_settings(GtkWindow *window, const EditorSettings *settings);
void window_set_output_enabled(GtkWindow *window, gboolean enabled);
void window_set_output_status(GtkWindow *window, const char *path, const char *text, gboolean active);
void window_append_output(GtkWindow *window, const char *path, const char *level, const char *text);
void window_set_output_target(GtkWindow *window, const char *path);
void window_set_launcher(GtkWindow *window, const char *path);
void window_set_scripts_path(GtkWindow *window, const char *path);
void window_update_profiles(GtkWindow *window, const char *const *names,
                            const char *const *sockets);
void window_set_profile_launch_error(GtkWindow *window, const char *name, const char *message);
gboolean window_start_session(GtkWindow *window, gboolean enabled, GError **error);

GtkTextBuffer *window_get_buffer(GtkWindow *window);
const char *window_get_script_name(GtkWindow *window);
