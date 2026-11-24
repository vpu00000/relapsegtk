#pragma once

#include <gtk/gtk.h>

GtkWidget *search_new(GtkWidget *editor);
void search_show(GtkWidget *search, gboolean replace);
void search_step(GtkWidget *search, gboolean backward);
