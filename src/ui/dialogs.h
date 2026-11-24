#pragma once

#include <gtk/gtk.h>

typedef void (*DialogChoiceFunc)(gint64 id, guint choice, gpointer user_data);

GtkWindow *dialog_window_new(GtkWindow *parent, gint64 id, const char *title, const char *text,
                             const char *const *buttons, const char *icon, guint default_button,
                             DialogChoiceFunc choice, gpointer user_data);
