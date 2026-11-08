#pragma once

#include <gtk/gtk.h>

typedef void (*ConsoleInputFunc)(gint64 id, const char *text, gpointer user_data);

GtkWindow *console_window_new(GtkWindow *parent, ConsoleInputFunc input, gpointer user_data);
void console_window_show(GtkWindow *window, gboolean visible);
void console_window_set_title(GtkWindow *window, const char *title);
void console_window_clear(GtkWindow *window);
void console_window_append(GtkWindow *window, const char *level, const char *text);
void console_window_request_input(GtkWindow *window, gint64 id);
