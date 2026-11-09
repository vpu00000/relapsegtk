#pragma once

#include <gtk/gtk.h>

typedef void (*PickerFinished)(GFile *file, const GError *error, gpointer user_data);

void picker_open_file(GtkWindow *parent, const char *title, const char *accept_label,
                      GFile *initial_folder, GCancellable *cancellable,
                      PickerFinished callback, gpointer user_data);
void picker_select_folder(GtkWindow *parent, const char *title, const char *accept_label,
                          GFile *initial_folder, GCancellable *cancellable,
                          PickerFinished callback, gpointer user_data);
