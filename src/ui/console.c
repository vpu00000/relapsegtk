#include "console.h"

typedef struct {
    GtkTextBuffer *buffer;
    GtkTextView *view;
    GtkEntry *entry;
    GQueue inputs;
    ConsoleInputFunc input;
    gpointer user_data;
} Console;

static Console *console_data(GtkWindow *window)
{
    return g_object_get_data(G_OBJECT(window), "console");
}

static void update_input(Console *console)
{
    gboolean active = console->inputs.length != 0;
    gtk_widget_set_sensitive(GTK_WIDGET(console->entry), active);
    gtk_entry_set_placeholder_text(console->entry, active ? "Script input…" : "No input requested");
    if (active) gtk_widget_grab_focus(GTK_WIDGET(console->entry));
}

static void input_submitted(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    Console *console = user_data;
    if (!console->inputs.length) return;
    gint64 *id = g_queue_pop_head(&console->inputs);
    g_autofree char *text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(console->entry)));
    gtk_editable_set_text(GTK_EDITABLE(console->entry), "");
    update_input(console);
    if (console->input) console->input(*id, text, console->user_data);
    g_free(id);
}

static void cancel_inputs(Console *console)
{
    while (console->inputs.length) {
        gint64 *id = g_queue_pop_head(&console->inputs);
        if (console->input) console->input(*id, NULL, console->user_data);
        g_free(id);
    }
    update_input(console);
}

static gboolean close_requested(GtkWindow *window, gpointer user_data)
{
    Console *console = user_data;
    cancel_inputs(console);
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    return TRUE;
}

static void free_console(gpointer user_data)
{
    Console *console = user_data;
    g_queue_clear_full(&console->inputs, g_free);
    g_free(console);
}

void console_window_show(GtkWindow *window, gboolean visible)
{
    if (visible) gtk_window_present(window);
    else {
        cancel_inputs(console_data(window));
        gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    }
}

void console_window_set_title(GtkWindow *window, const char *title)
{
    gtk_window_set_title(window, title);
}

void console_window_clear(GtkWindow *window)
{
    gtk_text_buffer_set_text(console_data(window)->buffer, "", 0);
}

void console_window_append(GtkWindow *window, const char *level, const char *text)
{
    Console *console = console_data(window);
    g_autofree char *line = g_str_has_suffix(text, "\n") ? g_strdup(text) : g_strconcat(text, "\n", NULL);
    const char *tag = g_str_equal(level, "warn") ? "warn" : g_str_equal(level, "error") ? "error" :
                      g_str_equal(level, "info") ? "info" : "print";
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(console->buffer, &end);
    gtk_text_buffer_insert_with_tags_by_name(console->buffer, &end, line, -1, tag, NULL);
    int lines = gtk_text_buffer_get_line_count(console->buffer);
    int chars = gtk_text_buffer_get_char_count(console->buffer);
    if (lines > 2000 || chars > 262144) {
        GtkTextIter start, cutoff;
        gtk_text_buffer_get_start_iter(console->buffer, &start);
        gtk_text_buffer_get_iter_at_offset(console->buffer, &cutoff, MAX(0, chars - 262144));
        if (gtk_text_iter_get_line(&cutoff) < lines - 2000)
            gtk_text_buffer_get_iter_at_line(console->buffer, &cutoff, lines - 2000);
        else if (!gtk_text_iter_starts_line(&cutoff))
            gtk_text_iter_forward_line(&cutoff);
        gtk_text_buffer_delete(console->buffer, &start, &cutoff);
    }
    gtk_text_buffer_get_end_iter(console->buffer, &end);
    gtk_text_view_scroll_to_iter(console->view, &end, 0, FALSE, 0, 1);
}

void console_window_request_input(GtkWindow *window, gint64 id)
{
    Console *console = console_data(window);
    for (GList *item = console->inputs.head; item; item = item->next)
        if (*(gint64 *)item->data == id) {
            console_window_show(window, TRUE);
            return;
        }
    gint64 *pending = g_new(gint64, 1);
    *pending = id;
    g_queue_push_tail(&console->inputs, pending);
    update_input(console);
    console_window_show(window, TRUE);
}

GtkWindow *console_window_new(GtkWindow *parent, ConsoleInputFunc input, gpointer user_data)
{
    Console *console = g_new0(Console, 1);
    console->input = input;
    console->user_data = user_data;
    GtkWindow *window = GTK_WINDOW(gtk_window_new());
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(window), "relapse-ui");
    gtk_window_set_title(window, "Relapse Console");
    gtk_window_set_transient_for(window, parent);
    gtk_window_set_default_size(window, 640, 360);
    g_object_set_data_full(G_OBJECT(window), "console", console, free_console);
    g_signal_connect(window, "close-request", G_CALLBACK(close_requested), console);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);
    gtk_window_set_child(window, box);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroll, "editor");
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(box), scroll);
    console->buffer = gtk_text_buffer_new(NULL);
    gtk_text_buffer_create_tag(console->buffer, "print", "foreground", "#d8dbe0", NULL);
    gtk_text_buffer_create_tag(console->buffer, "info", "foreground", "#94b9e3", NULL);
    gtk_text_buffer_create_tag(console->buffer, "warn", "foreground", "#e3bd7a", NULL);
    gtk_text_buffer_create_tag(console->buffer, "error", "foreground", "#e28b8b", NULL);
    console->view = GTK_TEXT_VIEW(gtk_text_view_new_with_buffer(console->buffer));
    gtk_text_view_set_editable(console->view, FALSE);
    gtk_text_view_set_cursor_visible(console->view, FALSE);
    gtk_text_view_set_wrap_mode(console->view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(console->view, TRUE);
    gtk_text_view_set_left_margin(console->view, 10);
    gtk_text_view_set_right_margin(console->view, 8);
    gtk_text_view_set_top_margin(console->view, 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(console->view));
    gtk_accessible_update_property(GTK_ACCESSIBLE(console->view), GTK_ACCESSIBLE_PROPERTY_LABEL, "Script console", -1);

    console->entry = GTK_ENTRY(gtk_entry_new());
    gtk_widget_add_css_class(GTK_WIDGET(console->entry), "console-input");
    gtk_entry_set_max_length(console->entry, 4096);
    gtk_accessible_update_property(GTK_ACCESSIBLE(console->entry), GTK_ACCESSIBLE_PROPERTY_LABEL, "Console input", -1);
    g_signal_connect(console->entry, "activate", G_CALLBACK(input_submitted), console);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(console->entry));
    update_input(console);
    return window;
}
