#pragma once

#include <gtksourceview/gtksource.h>

void editor_wrap_set_preference(GtkTextView *view, GtkWrapMode mode);
void editor_wrap_enable(GtkTextView *view);
void editor_wrap_disable(GtkTextView *view);
GtkTextBuffer *editor_document(GtkTextView *view);
void editor_reveal_cursor(GtkTextView *view);
