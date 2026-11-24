#pragma once

#include <gtk/gtk.h>
#include "settings/editor.h"

enum { EditorFileLimitBytes = 8 * 1024 * 1024 };

GtkWidget *editor_new(const EditorSettings *settings);
void editor_apply_settings(GtkWidget *editor, const EditorSettings *settings);
