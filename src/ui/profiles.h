#pragma once

#include <gtk/gtk.h>

typedef void (*SocketSelectedFunc)(const char *const *paths, gpointer user_data);
typedef gboolean (*ProfileLaunchFunc)(const char *name, const char *place_id, const char *job_id,
                                       GError **error, gpointer user_data);
typedef gboolean (*ProfileRunningFunc)(const char *name, gpointer user_data);

GtkWidget *socket_picker_new(SocketSelectedFunc selected, gpointer user_data);
void socket_picker_update(GtkWidget *picker, const char *const *paths);
