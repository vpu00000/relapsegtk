#pragma once

#include <gtk/gtk.h>

GtkWindow *configuration_dialog_new(GtkWindow *parent, const char *profile, GtkWidget *actions);
