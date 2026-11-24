#include "search.h"
#include "editor_wrap.h"

#include <gtksourceview/gtksource.h>

typedef struct {
    GtkTextView *editor;
    GtkSourceSearchContext *context;
    GtkEntry *entry;
    GtkLabel *count;
    GtkWidget *previous;
    GtkWidget *next;
    GtkWidget *replace_row;
    GtkWidget *replacement;
    GtkWidget *replace_one;
    GtkWidget *replace_all;
} Search;

static Search *search_data(GtkWidget *widget)
{
    return g_object_get_data(G_OBJECT(widget), "search");
}

static void update_count(GtkWidget *widget)
{
    Search *search = search_data(widget);
    GtkSourceSearchSettings *settings = gtk_source_search_context_get_settings(search->context);
    const char *query = gtk_source_search_settings_get_search_text(settings);
    int count = gtk_source_search_context_get_occurrences_count(search->context);
    int position = 0;
    GtkTextIter start, end;
    GtkTextBuffer *buffer = editor_document(search->editor);
    if (count > 0 && gtk_text_buffer_get_selection_bounds(buffer, &start, &end))
        position = gtk_source_search_context_get_occurrence_position(search->context, &start, &end);
    g_autofree char *label = !query || !*query ? g_strdup("") :
        count < 0 ? g_strdup("Searching…") :
        count == 0 ? g_strdup("No matches") :
        position > 0 ? g_strdup_printf("%d / %d", position, count) :
        g_strdup_printf("%d match%s", count, count == 1 ? "" : "es");
    gtk_label_set_text(search->count, label);
    gboolean have_matches = query && *query && count != 0;
    gtk_widget_set_sensitive(search->previous, have_matches);
    gtk_widget_set_sensitive(search->next, have_matches);
    gtk_widget_set_sensitive(search->replace_one, have_matches);
    gtk_widget_set_sensitive(search->replace_all, have_matches);
    if (query && *query && count == 0)
        gtk_widget_add_css_class(GTK_WIDGET(search->entry), "no-matches");
    else
        gtk_widget_remove_css_class(GTK_WIDGET(search->entry), "no-matches");
}

static void find_match(GtkWidget *widget, gboolean backward, gboolean include_selection)
{
    Search *search = search_data(widget);
    GtkSourceSearchSettings *settings = gtk_source_search_context_get_settings(search->context);
    const char *query = gtk_source_search_settings_get_search_text(settings);
    if (!query || !*query)
        return;
    GtkTextBuffer *buffer = editor_document(search->editor);
    GtkTextIter start, end, from;
    gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
    from = backward || include_selection ? start : end;
    gboolean found = backward
        ? gtk_source_search_context_backward(search->context, &from, &start, &end, NULL)
        : gtk_source_search_context_forward(search->context, &from, &start, &end, NULL);
    if (found) {
        gtk_text_buffer_select_range(buffer, &start, &end);
        editor_reveal_cursor(search->editor);
    }
    update_count(widget);
}

static void query_changed(GtkWidget *widget)
{
    if (!gtk_widget_get_visible(widget))
        return;
    Search *search = search_data(widget);
    GtkSourceSearchSettings *settings = gtk_source_search_context_get_settings(search->context);
    const char *query = gtk_editable_get_text(GTK_EDITABLE(search->entry));
    const char *clear_icon = *query ? "edit-clear-symbolic" : NULL;
    if (g_strcmp0(gtk_entry_get_icon_name(search->entry, GTK_ENTRY_ICON_SECONDARY), clear_icon) != 0)
        gtk_entry_set_icon_from_icon_name(search->entry, GTK_ENTRY_ICON_SECONDARY, clear_icon);
    gtk_source_search_settings_set_search_text(settings, query);
    find_match(widget, FALSE, TRUE);
    update_count(widget);
}

static void clear_query(GtkEntry *entry, GtkEntryIconPosition position, gpointer data)
{
    (void)position;
    (void)data;
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    gtk_widget_grab_focus(GTK_WIDGET(entry));
}

void search_step(GtkWidget *widget, gboolean backward)
{
    Search *search = search_data(widget);
    if (!gtk_widget_get_visible(widget)) {
        gtk_widget_set_visible(widget, TRUE);
        gtk_source_search_context_set_highlight(search->context, TRUE);
        gtk_source_search_settings_set_search_text(
            gtk_source_search_context_get_settings(search->context),
            gtk_editable_get_text(GTK_EDITABLE(search->entry)));
    }
    if (!*gtk_editable_get_text(GTK_EDITABLE(search->entry))) {
        gtk_widget_grab_focus(GTK_WIDGET(search->entry));
        return;
    }
    find_match(widget, backward, FALSE);
}

static void next_match(GtkWidget *widget)
{
    search_step(widget, FALSE);
}

static void previous_match(GtkWidget *widget)
{
    search_step(widget, TRUE);
}

static void replace_match(GtkWidget *widget)
{
    Search *search = search_data(widget);
    if (!gtk_widget_get_sensitive(search->replace_one)) return;
    GtkTextBuffer *buffer = editor_document(search->editor);
    GtkTextIter start, end;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(search->replacement));
    if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end) &&
        gtk_source_search_context_replace(search->context, &start, &end, text, -1, NULL))
        gtk_text_buffer_place_cursor(buffer, &end);
    find_match(widget, FALSE, FALSE);
}

static void replace_all_matches(GtkWidget *widget)
{
    Search *search = search_data(widget);
    const char *text = gtk_editable_get_text(GTK_EDITABLE(search->replacement));
    gtk_source_search_context_replace_all(search->context, text, -1, NULL);
    update_count(widget);
}

static void focus_field(GtkWidget *field)
{
    gtk_widget_grab_focus(field);
    gtk_editable_select_region(GTK_EDITABLE(field), 0, -1);
}

static void query_activated(GtkWidget *widget)
{
    Search *search = search_data(widget);
    if (gtk_widget_get_visible(search->replace_row))
        focus_field(search->replacement);
    else
        next_match(widget);
}

static void close_search(GtkWidget *widget)
{
    Search *search = search_data(widget);
    gtk_source_search_context_set_highlight(search->context, FALSE);
    gtk_source_search_settings_set_search_text(
        gtk_source_search_context_get_settings(search->context), NULL);
    gtk_widget_set_visible(widget, FALSE);
    gtk_widget_set_visible(search->replace_row, FALSE);
    gtk_widget_grab_focus(GTK_WIDGET(search->editor));
}

static void case_changed(GtkToggleButton *button, GtkWidget *widget)
{
    Search *search = search_data(widget);
    gtk_source_search_settings_set_case_sensitive(
        gtk_source_search_context_get_settings(search->context), gtk_toggle_button_get_active(button));
    find_match(widget, FALSE, TRUE);
    update_count(widget);
}

static gboolean search_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
                            GdkModifierType modifiers, GtkWidget *widget)
{
    (void)keycode;
    if (!gtk_widget_get_visible(widget))
        return FALSE;
    if (keyval == GDK_KEY_Escape) {
        close_search(widget);
        return TRUE;
    }
    if (gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)) == widget &&
        gdk_keyval_to_lower(keyval) == GDK_KEY_g &&
        (modifiers & gtk_accelerator_get_default_mod_mask() & ~GDK_SHIFT_MASK) == GDK_CONTROL_MASK) {
        search_step(widget, (modifiers & GDK_SHIFT_MASK) != 0);
        return TRUE;
    }
    if (gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)) == widget &&
        (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && (modifiers & GDK_SHIFT_MASK)) {
        previous_match(widget);
        return TRUE;
    }
    return FALSE;
}

void search_show(GtkWidget *widget, gboolean replace)
{
    Search *search = search_data(widget);
    GtkTextBuffer *buffer = editor_document(search->editor);
    GtkTextIter start, end;
    if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end) &&
        gtk_text_iter_get_line(&start) == gtk_text_iter_get_line(&end) &&
        gtk_text_iter_get_offset(&end) - gtk_text_iter_get_offset(&start) <= 256) {
        g_autofree char *selected = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        gtk_editable_set_text(GTK_EDITABLE(search->entry), selected);
    }
    gtk_widget_set_visible(widget, TRUE);
    gtk_widget_set_visible(search->replace_row, replace);
    gtk_source_search_context_set_highlight(search->context, TRUE);
    query_changed(widget);
    gboolean have_query = *gtk_editable_get_text(GTK_EDITABLE(search->entry)) != '\0';
    focus_field(replace && have_query ? search->replacement : GTK_WIDGET(search->entry));
}

static void free_search(gpointer data)
{
    Search *search = data;
    g_object_unref(search->context);
    g_object_unref(search->editor);
    g_free(search);
}

static GtkWidget *search_button(GtkWidget *row, const char *text, const char *label, GCallback callback, GtkWidget *bar)
{
    GtkWidget *button = gtk_button_new_with_label(text);
    gtk_widget_set_tooltip_text(button, label);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    g_signal_connect_object(button, "clicked", callback, bar, G_CONNECT_SWAPPED);
    gtk_box_append(GTK_BOX(row), button);
    return button;
}

static GtkWidget *search_field(GtkWidget *row, const char *placeholder, const char *label, GtkAccessibleRole role)
{
    GtkWidget *field = g_object_new(GTK_TYPE_ENTRY, "accessible-role", role, NULL);
    gtk_entry_set_placeholder_text(GTK_ENTRY(field), placeholder);
    gtk_editable_set_width_chars(GTK_EDITABLE(field), 8);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(field), 8);
    gtk_accessible_update_property(GTK_ACCESSIBLE(field), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    gtk_box_append(GTK_BOX(row), field);
    return field;
}

GtkWidget *search_new(GtkWidget *editor)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkFlowBox *groups = GTK_FLOW_BOX(gtk_flow_box_new());
    gtk_flow_box_set_selection_mode(groups, GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(groups, 1);
    gtk_flow_box_set_max_children_per_line(groups, 2);
    gtk_flow_box_set_column_spacing(groups, 10);
    gtk_flow_box_set_row_spacing(groups, 3);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(groups));
    GtkWidget *find_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_flow_box_append(groups, find_row);
    gtk_widget_set_focusable(gtk_widget_get_parent(find_row), FALSE);
    gtk_widget_add_css_class(bar, "editor-search");
    gtk_widget_set_visible(bar, FALSE);
    Search *search = g_new0(Search, 1);
    search->editor = g_object_ref(GTK_TEXT_VIEW(editor));
    GtkTextBuffer *buffer = editor_document(search->editor);
    g_autoptr(GtkSourceSearchSettings) settings = gtk_source_search_settings_new();
    gtk_source_search_settings_set_wrap_around(settings, TRUE);
    search->context = gtk_source_search_context_new(GTK_SOURCE_BUFFER(buffer), settings);
    gtk_source_search_context_set_highlight(search->context, FALSE);
    g_object_set_data_full(G_OBJECT(bar), "search", search, free_search);
    search->entry = GTK_ENTRY(search_field(find_row, "Find…", "Find in script", GTK_ACCESSIBLE_ROLE_SEARCH_BOX));
    gtk_entry_set_icon_tooltip_text(search->entry, GTK_ENTRY_ICON_SECONDARY, "Clear search");
    search->count = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(search->count), "search-count");
    gtk_widget_set_hexpand(GTK_WIDGET(search->count), TRUE);
    gtk_label_set_xalign(search->count, 1);
    gtk_label_set_ellipsize(search->count, PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(search->count));
    GtkWidget *match_case = gtk_toggle_button_new_with_label("Aa");
    gtk_widget_set_tooltip_text(match_case, "Match case");
    gtk_accessible_update_property(GTK_ACCESSIBLE(match_case), GTK_ACCESSIBLE_PROPERTY_LABEL, "Match case", -1);
    g_signal_connect_object(match_case, "toggled", G_CALLBACK(case_changed), bar, 0);
    gtk_box_append(GTK_BOX(find_row), match_case);
    search->previous = search_button(find_row, "↑", "Previous match (Shift+F3)", G_CALLBACK(previous_match), bar);
    search->next = search_button(find_row, "↓", "Next match (F3)", G_CALLBACK(next_match), bar);
    GtkWidget *close = search_button(bar, "×", "Close search (Escape)", G_CALLBACK(close_search), bar);
    gtk_widget_set_valign(close, GTK_ALIGN_CENTER);
    GtkWidget *replace_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(replace_row, "replace-row");
    gtk_flow_box_append(groups, replace_row);
    search->replace_row = gtk_widget_get_parent(replace_row);
    gtk_widget_set_focusable(search->replace_row, FALSE);
    gtk_widget_set_visible(search->replace_row, FALSE);
    search->replacement = search_field(replace_row, "Replace with…", "Replace with", GTK_ACCESSIBLE_ROLE_TEXT_BOX);
    search->replace_one = search_button(replace_row, "Replace", "Replace current match (Enter)",
        G_CALLBACK(replace_match), bar);
    search->replace_all = search_button(replace_row, "Replace All", "Replace all matches in this tab",
        G_CALLBACK(replace_all_matches), bar);
    g_signal_connect_object(search->replacement, "activate", G_CALLBACK(replace_match), bar, G_CONNECT_SWAPPED);
    g_signal_connect_object(search->entry, "changed", G_CALLBACK(query_changed), bar, G_CONNECT_SWAPPED);
    g_signal_connect_object(search->entry, "activate", G_CALLBACK(query_activated), bar, G_CONNECT_SWAPPED);
    g_signal_connect(search->entry, "icon-release", G_CALLBACK(clear_query), NULL);
    g_signal_connect_object(search->context, "notify::occurrences-count", G_CALLBACK(update_count), bar, G_CONNECT_SWAPPED);
    g_signal_connect_object(buffer, "notify::cursor-position", G_CALLBACK(update_count), bar, G_CONNECT_SWAPPED);
    GtkWidget *targets[] = {bar, editor};
    for (guint i = 0; i < G_N_ELEMENTS(targets); ++i) {
        GtkEventController *keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect_object(keys, "key-pressed", G_CALLBACK(search_key), bar, 0);
        gtk_widget_add_controller(targets[i], keys);
    }
    update_count(bar);
    return bar;
}
