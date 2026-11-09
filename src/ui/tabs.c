#include "tabs.h"

#include "editor.h"
#include "tab_strip.h"
#include "search.h"
#include "state/session.h"
#include <string.h>

enum { MaxScriptBytes = 8 * 1024 * 1024 };

typedef struct {
    GtkWidget *strip;
    TabsChangedFunc changed;
    gpointer user_data;
    GFile *scripts_dir;
    EditorSettings editor_settings;
    guint next_untitled;
    SessionWriter *session_writer;
    guint session_timer;
    gint64 session_dirty_since;
    gboolean session_enabled;
    gboolean restoring;
    gboolean stopped;
} Tabs;

typedef struct PendingSave PendingSave;

typedef struct {
    GtkNotebook *notebook;
    GtkWidget *page;
    GtkWidget *editor;
    GtkWidget *search;
    GtkWidget *tab;
    GtkImage *icon;
    GtkLabel *label;
    GtkLabel *message;
    GFile *file;
    char *name;
    GCancellable *load;
    GCancellable *save;
    GFile *save_file;
    PendingSave *saving;
    GCancellable *close_prompt;
    gboolean ready;
    char *recovery_text;
    int restore_cursor;
} Script;

typedef struct {
    GWeakRef notebook;
    GWeakRef page;
} PendingPage;

struct PendingSave {
    PendingPage *page;
    GBytes *contents;
    GBytes *next_contents;
    GApplication *application;
};

static void schedule_session(GtkNotebook *notebook, gboolean immediate);

static GtkNotebook *notebook_for(GtkWidget *widget)
{
    return GTK_IS_NOTEBOOK(widget) ? GTK_NOTEBOOK(widget) :
        g_object_get_data(G_OBJECT(widget), "notebook");
}

static PendingPage *pending_page_new(Script *script)
{
    PendingPage *pending = g_new0(PendingPage, 1);
    g_weak_ref_init(&pending->notebook, script->notebook);
    g_weak_ref_init(&pending->page, script->page);
    return pending;
}

static void pending_page_free(PendingPage *pending)
{
    g_weak_ref_clear(&pending->notebook);
    g_weak_ref_clear(&pending->page);
    g_free(pending);
}

static Script *find_script(GtkNotebook *notebook, GtkWidget *page)
{
    if (!notebook || !page)
        return NULL;
    Tabs *tabs = g_object_get_data(G_OBJECT(notebook), "tabs");
    return !tabs->stopped && gtk_notebook_page_num(notebook, page) >= 0
        ? g_object_get_data(G_OBJECT(page), "script") : NULL;
}

static Script *active_script(GtkWidget *notebook)
{
    GtkNotebook *book = notebook_for(notebook);
    return find_script(book, gtk_notebook_get_nth_page(book, gtk_notebook_get_current_page(book)));
}

static void notify_changed(GtkNotebook *notebook)
{
    Tabs *tabs = g_object_get_data(G_OBJECT(notebook), "tabs");
    if (tabs->restoring)
        return;
    schedule_session(notebook, TRUE);
    if (!tabs->stopped && tabs->changed)
        tabs->changed(GTK_WIDGET(notebook), tabs->user_data);
}

GtkTextBuffer *tabs_get_buffer(GtkWidget *tabs)
{
    Script *script = active_script(tabs);
    return script && script->ready ? gtk_text_view_get_buffer(GTK_TEXT_VIEW(script->editor)) : NULL;
}

const char *tabs_get_name(GtkWidget *tabs)
{
    Script *script = active_script(tabs);
    return script ? script->name : NULL;
}

void tabs_find(GtkWidget *tabs)
{
    Script *script = active_script(tabs);
    if (script && script->ready)
        search_show(script->search);
}

void tabs_find_next(GtkWidget *tabs, gboolean backward)
{
    Script *script = active_script(tabs);
    if (script && script->ready)
        search_step(script->search, backward);
}

GFile *tabs_get_current_file(GtkWidget *tabs)
{
    Script *script = active_script(tabs);
    if (!script) return NULL;
    return script->file ? script->file : script->save_file;
}

void tabs_close_current(GtkWidget *tabs)
{
    GtkNotebook *book = notebook_for(tabs);
    int page = gtk_notebook_get_current_page(book);
    if (page >= 0) {
        gtk_notebook_remove_page(book, page);
        notify_changed(book);
    }
}

static void update_origin_icon(Script *script)
{
    GFile *file = script->file ? script->file : script->save_file;
    g_autofree char *directory = g_build_filename(g_get_home_dir(), ".config", "relapse", "autoexec", NULL);
    g_autoptr(GFile) autoexec = g_file_new_for_path(directory);
    gboolean is_autoexec = file && g_file_has_prefix(file, autoexec);
    if (is_autoexec)
        gtk_image_set_from_resource(script->icon, "/org/relapse/RelapseGtk/icons/autoexec.svg");
    gtk_widget_set_visible(GTK_WIDGET(script->icon), is_autoexec);
}

static void update_label(GtkTextBuffer *buffer, GtkWidget *page)
{
    Script *script = g_object_get_data(G_OBJECT(page), "script");
    update_origin_icon(script);
    g_autofree char *label = g_strdup_printf("%s%s", script->name,
        script->load ? " …" : gtk_text_buffer_get_modified(buffer) ? " •" : "");
    gtk_label_set_text(script->label, label);
    schedule_session(script->notebook, FALSE);
}

static void buffer_changed(GtkTextBuffer *buffer, GtkWidget *page)
{
    (void)buffer;
    Script *script = g_object_get_data(G_OBJECT(page), "script");
    schedule_session(script->notebook, FALSE);
}

static void cursor_changed(GObject *buffer, GParamSpec *property, GtkWidget *page)
{
    (void)property;
    buffer_changed(GTK_TEXT_BUFFER(buffer), page);
}

static Session *snapshot(GtkNotebook *notebook)
{
    Tabs *tabs = g_object_get_data(G_OBJECT(notebook), "tabs");
    Session *session = session_new();
    session->next_untitled = tabs->next_untitled;
    session->active_tab = MAX(0, gtk_notebook_get_current_page(notebook));
    for (int i = 0; i < gtk_notebook_get_n_pages(notebook); ++i) {
        Script *script = g_object_get_data(G_OBJECT(gtk_notebook_get_nth_page(notebook, i)), "script");
        SessionTab *tab = g_new0(SessionTab, 1);
        tab->name = g_strdup(script->name);
        GFile *file = script->file ? script->file : script->save_file;
        if (file)
            tab->uri = g_file_get_uri(file);
        if (!script->file && script->save_file) {
            g_autofree char *name = g_file_get_basename(script->save_file);
            if (name) {
                g_free(tab->name);
                tab->name = g_filename_display_name(name);
            }
        }
        if (script->ready) {
            GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(script->editor));
            GtkTextIter start, end, cursor;
            gtk_text_buffer_get_bounds(buffer, &start, &end);
            tab->text = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
            tab->modified = script->saving || gtk_text_buffer_get_modified(buffer);
            gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
            tab->cursor = gtk_text_iter_get_offset(&cursor);
        } else {
            tab->text = g_strdup(script->recovery_text);
            tab->cursor = script->restore_cursor;
        }
        g_ptr_array_add(session->tabs, tab);
    }
    return session;
}

static gboolean write_session(gpointer user_data)
{
    GtkNotebook *notebook = user_data;
    Tabs *tabs = g_object_get_data(G_OBJECT(notebook), "tabs");
    tabs->session_timer = 0;
    tabs->session_dirty_since = 0;
    session_writer_queue(tabs->session_writer, snapshot(notebook));
    return G_SOURCE_REMOVE;
}

static void schedule_session(GtkNotebook *notebook, gboolean immediate)
{
    Tabs *tabs = g_object_get_data(G_OBJECT(notebook), "tabs");
    if (!tabs->session_enabled || tabs->stopped || tabs->restoring)
        return;
    gint64 now = g_get_monotonic_time();
    if (!tabs->session_dirty_since)
        tabs->session_dirty_since = now;
    guint delay = immediate ? 50 : 600;
    gint64 remaining = 5000 - (now - tabs->session_dirty_since) / 1000;
    delay = MIN(delay, MAX(1, remaining));
    if (tabs->session_timer)
        g_source_remove(tabs->session_timer);
    tabs->session_timer = g_timeout_add(delay, write_session, notebook);
}

static void session_error(const char *detail, gpointer user_data)
{
    GtkWidget *notebook = user_data;
    GtkRoot *root = gtk_widget_get_root(notebook);
    if (GTK_IS_WINDOW(root)) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not save tab recovery data");
        gtk_alert_dialog_set_detail(alert, detail);
        gtk_alert_dialog_show(alert, GTK_WINDOW(root));
    }
}

void tabs_set_session_enabled(GtkWidget *widget, gboolean enabled)
{
    Tabs *tabs = g_object_get_data(G_OBJECT(widget), "tabs");
    if (tabs->stopped)
        return;
    tabs->session_enabled = enabled;
    if (enabled) {
        if (!tabs->session_writer) {
            GtkRoot *root = gtk_widget_get_root(widget);
            GtkApplication *app = GTK_IS_WINDOW(root) ? gtk_window_get_application(GTK_WINDOW(root)) : NULL;
            tabs->session_writer = session_writer_new(app ? G_APPLICATION(app) : NULL, session_error, widget);
        }
        schedule_session(notebook_for(widget), TRUE);
    } else {
        if (tabs->session_timer)
            g_source_remove(tabs->session_timer);
        tabs->session_timer = 0;
        tabs->session_dirty_since = 0;
    }
}

static void stop_script(Script *script)
{
    if (script->load)
        g_cancellable_cancel(script->load);
    if (script->save && !script->save_file)
        g_cancellable_cancel(script->save);
    if (script->close_prompt)
        g_cancellable_cancel(script->close_prompt);
}

static void free_script(gpointer data)
{
    Script *script = data;
    stop_script(script);
    g_clear_object(&script->load);
    g_clear_object(&script->save);
    g_clear_object(&script->save_file);
    g_clear_object(&script->close_prompt);
    g_clear_object(&script->file);
    g_clear_object(&script->tab);
    g_free(script->name);
    g_free(script->recovery_text);
    g_free(script);
}

static void save_error(Script *script, const char *detail)
{
    g_printerr("Could not save %s: %s\n", script->name, detail);
    GtkRoot *root = gtk_widget_get_root(script->page);
    if (GTK_IS_WINDOW(root)) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not save %s", script->name);
        gtk_alert_dialog_set_detail(alert, detail);
        gtk_alert_dialog_show(alert, GTK_WINDOW(root));
    }
}

static void save_to_file(Script *script, GFile *file);

static GBytes *script_contents(Script *script)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(script->editor));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
    return g_bytes_new_take(text, strlen(text));
}

static void script_saved(GObject *file, GAsyncResult *result, gpointer user_data)
{
    PendingSave *pending = user_data;
    g_autoptr(GError) error = NULL;
    gboolean saved = g_file_replace_contents_finish(G_FILE(file), result, NULL, &error);
    g_autoptr(GtkNotebook) notebook = g_weak_ref_get(&pending->page->notebook);
    g_autoptr(GtkWidget) page = g_weak_ref_get(&pending->page->page);
    Script *script = find_script(notebook, page);
    if (script) {
        if (saved) {
            g_set_object(&script->file, G_FILE(file));
            g_autofree char *basename = g_file_get_basename(script->file);
            g_free(script->name);
            script->name = basename ? g_filename_display_name(basename) : g_strdup("script.luau");
            g_autofree char *path = g_file_get_parse_name(script->file);
            gtk_widget_set_tooltip_text(script->tab, path);

            GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(script->editor));
            g_autoptr(GBytes) contents = script_contents(script);
            gtk_text_buffer_set_modified(buffer, !g_bytes_equal(contents, pending->contents));
            update_label(buffer, page);
            notify_changed(notebook);
        } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            save_error(script, error->message);
        }
    } else if (!saved) {
        g_autofree char *path = g_file_get_parse_name(G_FILE(file));
        g_printerr("Could not save %s: %s\n", path, error->message);
    }
    if (saved && pending->next_contents && !g_bytes_equal(pending->contents, pending->next_contents)) {
        g_bytes_unref(pending->contents);
        pending->contents = g_steal_pointer(&pending->next_contents);
        g_file_replace_contents_bytes_async(G_FILE(file), pending->contents, NULL, FALSE,
            G_FILE_CREATE_NONE, NULL, script_saved, pending);
        return;
    }
    if (script) {
        script->saving = NULL;
        g_clear_object(&script->save);
        g_clear_object(&script->save_file);
    }
    pending_page_free(pending->page);
    g_bytes_unref(pending->contents);
    g_clear_pointer(&pending->next_contents, g_bytes_unref);
    if (pending->application) {
        g_application_release(pending->application);
        g_object_unref(pending->application);
    }
    g_free(pending);
}

static void save_to_file(Script *script, GFile *file)
{
    for (int i = 0; i < gtk_notebook_get_n_pages(script->notebook); ++i) {
        GtkWidget *page = gtk_notebook_get_nth_page(script->notebook, i);
        Script *other = g_object_get_data(G_OBJECT(page), "script");
        if (other != script &&
            ((other->file && g_file_equal(other->file, file)) ||
             (other->save_file && g_file_equal(other->save_file, file)))) {
            g_clear_object(&script->save);
            save_error(script, "This file is already open in another tab. Choose a different filename or save that tab.");
            return;
        }
    }
    PendingSave *pending = g_new0(PendingSave, 1);
    pending->page = pending_page_new(script);
    pending->contents = script_contents(script);
    GtkRoot *root = gtk_widget_get_root(script->page);
    GtkApplication *application = GTK_IS_WINDOW(root)
        ? gtk_window_get_application(GTK_WINDOW(root)) : NULL;
    if (application) {
        pending->application = g_object_ref(G_APPLICATION(application));
        g_application_hold(pending->application);
    }
    if (!script->save)
        script->save = g_cancellable_new();
    script->save_file = g_object_ref(file);
    script->saving = pending;
    g_file_replace_contents_bytes_async(file, pending->contents, NULL, FALSE,
        G_FILE_CREATE_NONE, script->save, script_saved, pending);
}

static void save_file_chosen(GObject *dialog, GAsyncResult *result, gpointer user_data)
{
    PendingPage *pending = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(dialog), result, &error);
    g_autoptr(GtkNotebook) notebook = g_weak_ref_get(&pending->notebook);
    g_autoptr(GtkWidget) page = g_weak_ref_get(&pending->page);
    Script *script = find_script(notebook, page);
    if (script) {
        if (file) {
            save_to_file(script, file);
        } else {
            g_clear_object(&script->save);
            if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
                !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
                !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                save_error(script, error->message);
        }
    }
    pending_page_free(pending);
}

void tabs_save(GtkWidget *tabs)
{
    Script *script = active_script(tabs);
    if (!script || !script->ready || script->close_prompt)
        return;
    if (script->save) {
        if (script->saving) {
            g_clear_pointer(&script->saving->next_contents, g_bytes_unref);
            script->saving->next_contents = script_contents(script);
        }
        return;
    }
    if (script->file) {
        save_to_file(script, script->file);
        return;
    }
    GtkRoot *root = gtk_widget_get_root(script->page);
    if (!GTK_IS_WINDOW(root))
        return;
    Tabs *state = g_object_get_data(G_OBJECT(tabs), "tabs");
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save script");
    gtk_file_dialog_set_accept_label(dialog, "Save");
    gtk_file_dialog_set_modal(dialog, TRUE);
    gtk_file_dialog_set_initial_folder(dialog, state->scripts_dir);
    g_autofree char *name = g_strconcat(script->name, ".luau", NULL);
    gtk_file_dialog_set_initial_name(dialog, name);
    g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_autoptr(GtkFileFilter) scripts = gtk_file_filter_new();
    gtk_file_filter_set_name(scripts, "Scripts (*.lua, *.luau, *.txt)");
    gtk_file_filter_add_suffix(scripts, "lua");
    gtk_file_filter_add_suffix(scripts, "luau");
    gtk_file_filter_add_suffix(scripts, "txt");
    g_list_store_append(filters, scripts);
    g_autoptr(GtkFileFilter) all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, all);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, scripts);
    script->save = g_cancellable_new();
    gtk_file_dialog_save(dialog, GTK_WINDOW(root), script->save, save_file_chosen, pending_page_new(script));
}

static Script *new_script(GtkNotebook *notebook, GFile *file, const char *name, const char *text);

static void new_tab(GtkButton *button, GtkNotebook *notebook)
{
    (void)button;
    Tabs *tabs = g_object_get_data(G_OBJECT(notebook), "tabs");
    g_autofree char *name = g_strdup_printf("Untitled %u", ++tabs->next_untitled);
    new_script(notebook, NULL, name, "");
}

static void remove_script(Script *script)
{
    GtkNotebook *notebook = script->notebook;
    stop_script(script);
    gtk_notebook_remove_page(notebook, gtk_notebook_page_num(notebook, script->page));
    if (gtk_notebook_get_n_pages(notebook) == 0)
        new_tab(NULL, notebook);
    notify_changed(notebook);
}

static void confirm_close(GObject *dialog, GAsyncResult *result, gpointer user_data)
{
    PendingPage *pending = user_data;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dialog), result, NULL);
    g_autoptr(GtkNotebook) notebook = g_weak_ref_get(&pending->notebook);
    g_autoptr(GtkWidget) page = g_weak_ref_get(&pending->page);
    Script *script = find_script(notebook, page);
    if (script) {
        g_clear_object(&script->close_prompt);
        if (choice == 1)
            remove_script(script);
    }
    pending_page_free(pending);
}

static void close_tab(GtkButton *button, GtkWidget *page)
{
    (void)button;
    Script *script = g_object_get_data(G_OBJECT(page), "script");
    if (script->close_prompt)
        return;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(script->editor));
    if (!gtk_text_buffer_get_modified(buffer)) {
        remove_script(script);
        return;
    }
    g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new("Discard changes to %s?", script->name);
    gtk_alert_dialog_set_detail(dialog, "This tab's edits have not been saved to disk.");
    const char *buttons[] = {"Keep editing", "Discard", NULL};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);
    script->close_prompt = g_cancellable_new();
    gtk_alert_dialog_choose(dialog, GTK_WINDOW(gtk_widget_get_root(page)), script->close_prompt,
                            confirm_close, pending_page_new(script));
}

static void select_script(GtkButton *button, gpointer data)
{
    (void)button;
    Script *script = data;
    gtk_notebook_set_current_page(script->notebook, gtk_notebook_page_num(script->notebook, script->page));
}

static GdkContentProvider *drag_tab(GtkDragSource *source, double x, double y, gpointer data)
{
    (void)source;
    (void)x;
    (void)y;
    Script *script = data;
    return gdk_content_provider_new_typed(GTK_TYPE_WIDGET, script->page);
}

static gboolean drop_tab(GtkDropTarget *target, const GValue *value, double x, double y, gpointer data)
{
    (void)target;
    (void)y;
    Script *script = data;
    GtkWidget *page = g_value_get_object(value);
    if (!find_script(script->notebook, page)) return FALSE;
    int from = gtk_notebook_page_num(script->notebook, page);
    int to = gtk_notebook_page_num(script->notebook, script->page);
    if (x > gtk_widget_get_width(script->tab) / 2.0) ++to;
    if (from < to) --to;
    gtk_notebook_reorder_child(script->notebook, page, to);
    return TRUE;
}

static Script *new_script(GtkNotebook *notebook, GFile *file, const char *name, const char *text)
{
    Script *script = g_new0(Script, 1);
    script->notebook = notebook;
    script->name = g_strdup(name);
    script->file = file ? g_object_ref(file) : NULL;
    script->ready = text != NULL;
    script->page = gtk_stack_new();
    g_object_set_data_full(G_OBJECT(script->page), "script", script, free_script);
    Tabs *tabs = g_object_get_data(G_OBJECT(notebook), "tabs");
    script->editor = editor_new(&tabs->editor_settings);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(script->editor));
    gtk_text_buffer_set_text(buffer, text ? text : "", -1);
    gtk_text_buffer_set_modified(buffer, FALSE);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroll, "editor");
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll), FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), script->editor);
    GtkWidget *editor_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    script->search = search_new(script->editor);
    gtk_box_append(GTK_BOX(editor_box), script->search);
    gtk_box_append(GTK_BOX(editor_box), scroll);
    gtk_stack_add_named(GTK_STACK(script->page), editor_box, "editor");
    script->message = GTK_LABEL(gtk_label_new("Opening script…"));
    gtk_label_set_wrap(script->message, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(script->message), "script-message");
    gtk_stack_add_named(GTK_STACK(script->page), GTK_WIDGET(script->message), "message");
    gtk_stack_set_visible_child_name(GTK_STACK(script->page), text ? "editor" : "message");

    GtkWidget *tab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    script->tab = g_object_ref_sink(tab);
    gtk_widget_add_css_class(tab, "script-tab");
    g_object_set_data(G_OBJECT(tab), "page", script->page);
    GtkWidget *select = gtk_button_new();
    gtk_widget_add_css_class(select, "tab-select");
    gtk_box_append(GTK_BOX(tab), select);
    g_signal_connect(select, "clicked", G_CALLBACK(select_script), script);
    GtkDragSource *drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
    g_signal_connect(drag, "prepare", G_CALLBACK(drag_tab), script);
    gtk_widget_add_controller(select, GTK_EVENT_CONTROLLER(drag));
    GtkDropTarget *drop = gtk_drop_target_new(GTK_TYPE_WIDGET, GDK_ACTION_MOVE);
    g_signal_connect(drop, "drop", G_CALLBACK(drop_tab), script);
    gtk_widget_add_controller(tab, GTK_EVENT_CONTROLLER(drop));
    script->label = GTK_LABEL(gtk_label_new(name));
    gtk_label_set_xalign(script->label, 0);
    gtk_label_set_ellipsize(script->label, PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(script->label, 12);
    gtk_label_set_max_width_chars(script->label, 22);
    script->icon = GTK_IMAGE(gtk_image_new());
    gtk_image_set_pixel_size(script->icon, 16);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(script->icon));
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(script->label));
    gtk_button_set_child(GTK_BUTTON(select), content);
    update_origin_icon(script);
    GtkWidget *close = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(close, "tab-close");
    gtk_widget_set_tooltip_text(close, "Close tab");
    gtk_accessible_update_property(GTK_ACCESSIBLE(close), GTK_ACCESSIBLE_PROPERTY_LABEL, "Close tab", -1);
    gtk_box_append(GTK_BOX(tab), close);
    g_signal_connect(close, "clicked", G_CALLBACK(close_tab), script->page);
    g_autofree char *path = file ? g_file_get_parse_name(file) : g_strdup(name);
    gtk_widget_set_tooltip_text(tab, path);
    g_signal_connect_object(buffer, "modified-changed", G_CALLBACK(update_label), script->page, 0);
    g_signal_connect_object(buffer, "changed", G_CALLBACK(buffer_changed), script->page, 0);
    g_signal_connect_object(buffer, "notify::cursor-position", G_CALLBACK(cursor_changed), script->page, 0);
    gtk_box_append(tab_strip_content(tabs->strip), tab);
    int index = gtk_notebook_append_page(notebook, script->page, NULL);
    gtk_notebook_set_current_page(notebook, index);
    if (script->ready)
        gtk_widget_grab_focus(script->editor);
    return script;
}

static gboolean read_more_script(const char *contents, goffset size, gpointer user_data)
{
    (void)contents;
    (void)user_data;
    return size <= MaxScriptBytes;
}

static void script_loaded(GObject *file, GAsyncResult *result, gpointer user_data)
{
    PendingPage *pending = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree char *contents = NULL;
    gsize size = 0;
    gboolean loaded = g_file_load_partial_contents_finish(G_FILE(file), result, &contents, &size, NULL, &error);
    g_autoptr(GtkNotebook) notebook = g_weak_ref_get(&pending->notebook);
    g_autoptr(GtkWidget) page = g_weak_ref_get(&pending->page);
    Script *script = find_script(notebook, page);
    if (script && !g_cancellable_is_cancelled(script->load)) {
        g_clear_object(&script->load);
        const char *failure = !loaded ? error->message :
            size > MaxScriptBytes ? "Script is larger than 8 MiB" :
            !g_utf8_validate(contents, size, NULL) || memchr(contents, '\0', size)
                ? "Script must be UTF-8 text" : NULL;
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(script->editor));
        if (failure) {
            g_printerr("Could not open %s: %s\n", script->name, failure);
            if (script->recovery_text) {
                gtk_text_buffer_set_text(buffer, script->recovery_text, -1);
                gtk_text_buffer_set_modified(buffer, TRUE);
                script->ready = TRUE;
                g_autofree char *path = g_file_get_parse_name(script->file);
                g_autofree char *tip = g_strdup_printf("%s\nRecovered session text: %s", path, failure);
                gtk_widget_set_tooltip_text(script->tab, tip);
            } else {
                gtk_label_set_text(script->message, failure);
            }
        } else {
            gtk_text_buffer_set_text(buffer, contents, (gint)size);
            gtk_text_buffer_set_modified(buffer, FALSE);
            script->ready = TRUE;
        }
        if (script->ready) {
            GtkTextIter cursor;
            gtk_text_buffer_get_iter_at_offset(buffer, &cursor,
                MIN(script->restore_cursor, gtk_text_buffer_get_char_count(buffer)));
            gtk_text_buffer_place_cursor(buffer, &cursor);
            gtk_stack_set_visible_child_name(GTK_STACK(page), "editor");
            if (script->restore_cursor > 0)
                gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(script->editor),
                    gtk_text_buffer_get_insert(buffer), 0.1, FALSE, 0, 0);
            if (active_script(GTK_WIDGET(notebook)) == script)
                gtk_widget_grab_focus(script->editor);
        }
        g_clear_pointer(&script->recovery_text, g_free);
        update_label(buffer, page);
        notify_changed(notebook);
    }
    pending_page_free(pending);
}

static void load_script(Script *script)
{
    script->load = g_cancellable_new();
    gtk_label_set_text(script->message, "Opening script…");
    update_label(gtk_text_view_get_buffer(GTK_TEXT_VIEW(script->editor)), script->page);
    notify_changed(script->notebook);
    g_file_load_partial_contents_async(script->file, script->load, read_more_script, script_loaded,
                                      pending_page_new(script));
}

gboolean tabs_restore_session(GtkWidget *widget, GError **error)
{
    g_autoptr(Session) session = NULL;
    if (!session_load(&session, error))
        return FALSE;
    if (!session || !session->tabs->len)
        return TRUE;
    GtkNotebook *notebook = notebook_for(widget);
    Tabs *tabs = g_object_get_data(G_OBJECT(notebook), "tabs");
    tabs->restoring = TRUE;
    while (gtk_notebook_get_n_pages(notebook)) {
        GtkWidget *page = gtk_notebook_get_nth_page(notebook, 0);
        stop_script(g_object_get_data(G_OBJECT(page), "script"));
        gtk_notebook_remove_page(notebook, 0);
    }
    tabs->next_untitled = session->next_untitled;
    for (guint i = 0; i < session->tabs->len; ++i) {
        SessionTab *tab = g_ptr_array_index(session->tabs, i);
        g_autoptr(GFile) file = tab->uri ? g_file_new_for_uri(tab->uri) : NULL;
        gboolean reload = file && !tab->modified;
        Script *script = new_script(notebook, file, tab->name, reload ? NULL : tab->text);
        script->restore_cursor = tab->cursor;
        if (reload) {
            script->recovery_text = g_strdup(tab->text);
            load_script(script);
        } else {
            GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(script->editor));
            gtk_text_buffer_set_modified(buffer, tab->modified);
            GtkTextIter cursor;
            gtk_text_buffer_get_iter_at_offset(buffer, &cursor,
                MIN(tab->cursor, gtk_text_buffer_get_char_count(buffer)));
            gtk_text_buffer_place_cursor(buffer, &cursor);
            gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(script->editor),
                gtk_text_buffer_get_insert(buffer), 0.1, FALSE, 0, 0);
            update_label(buffer, script->page);
        }
    }
    gtk_notebook_set_current_page(notebook, session->active_tab);
    tabs->restoring = FALSE;
    notify_changed(notebook);
    return TRUE;
}

void tabs_open(GtkWidget *tabs, GFile *file, const char *display_name)
{
    GtkNotebook *notebook = notebook_for(tabs);
    Script *script = NULL;
    for (int i = 0; i < gtk_notebook_get_n_pages(notebook); ++i) {
        Script *candidate = find_script(notebook, gtk_notebook_get_nth_page(notebook, i));
        if ((candidate->file && g_file_equal(candidate->file, file)) ||
            (candidate->save_file && g_file_equal(candidate->save_file, file))) {
            gtk_notebook_set_current_page(notebook, i);
            if (candidate->ready) {
                gtk_widget_grab_focus(candidate->editor);
                return;
            }
            if (candidate->load)
                return;
            script = candidate;
            break;
        }
    }
    if (!script)
        script = new_script(notebook, file, display_name, NULL);
    load_script(script);
}

GFile *tabs_rename(GtkWidget *widget, GFile *file, const char *name, GError **error)
{
    if (!*name || g_str_equal(name, ".") || g_str_equal(name, "..") || strchr(name, '/')) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
            "Enter a filename without slashes.");
        return NULL;
    }
    GtkNotebook *notebook = notebook_for(widget);
    for (int i = 0; i < gtk_notebook_get_n_pages(notebook); ++i) {
        Script *script = find_script(notebook, gtk_notebook_get_nth_page(notebook, i));
        if (script && ((script->file && g_file_equal(script->file, file)) ||
                       (script->save_file && g_file_equal(script->save_file, file))) &&
            (script->load || script->save || script->close_prompt)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                "This script is busy. Try again when it finishes.");
            return NULL;
        }
    }
    GFile *renamed = g_file_set_display_name(file, name, NULL, error);
    if (!renamed)
        return NULL;
    g_autofree char *basename = g_file_get_basename(renamed);
    g_autofree char *display_name = g_filename_display_name(basename);
    g_autofree char *path = g_file_get_parse_name(renamed);
    for (int i = 0; i < gtk_notebook_get_n_pages(notebook); ++i) {
        GtkWidget *page = gtk_notebook_get_nth_page(notebook, i);
        Script *script = find_script(notebook, page);
        if (!script || !script->file || !g_file_equal(script->file, file))
            continue;
        g_set_object(&script->file, renamed);
        g_free(script->name);
        script->name = g_strdup(display_name);
        gtk_widget_set_tooltip_text(script->tab, path);
        update_label(gtk_text_view_get_buffer(GTK_TEXT_VIEW(script->editor)), page);
    }
    notify_changed(notebook);
    return renamed;
}

static void switched_page(GtkNotebook *notebook, GtkWidget *page, guint index, gpointer user_data)
{
    (void)index;
    (void)user_data;
    Tabs *tabs = g_object_get_data(G_OBJECT(notebook), "tabs");
    for (GtkWidget *tab = gtk_widget_get_first_child(GTK_WIDGET(tab_strip_content(tabs->strip)));
         tab; tab = gtk_widget_get_next_sibling(tab)) {
        if (g_object_get_data(G_OBJECT(tab), "page") == page) {
            gtk_widget_add_css_class(tab, "active");
            tab_strip_reveal(tabs->strip, tab);
        } else gtk_widget_remove_css_class(tab, "active");
    }
    Script *script = find_script(notebook, page);
    if (script && script->ready)
        gtk_widget_grab_focus(script->editor);
    notify_changed(notebook);
}

static void free_tabs(gpointer user_data)
{
    Tabs *tabs = user_data;
    if (tabs->session_timer)
        g_source_remove(tabs->session_timer);
    session_writer_free(tabs->session_writer);
    g_object_unref(tabs->scripts_dir);
    g_object_unref(tabs->strip);
    g_free(tabs);
}

static void page_reordered(GtkNotebook *notebook, GtkWidget *page, guint index, gpointer user_data)
{
    (void)user_data;
    Tabs *tabs = g_object_get_data(G_OBJECT(notebook), "tabs");
    Script *script = find_script(notebook, page);
    Script *previous = index ? find_script(notebook, gtk_notebook_get_nth_page(notebook, index - 1)) : NULL;
    gtk_box_reorder_child_after(tab_strip_content(tabs->strip), script->tab, previous ? previous->tab : NULL);
    tab_strip_reveal(tabs->strip, script->tab);
    schedule_session(notebook, TRUE);
}

static void page_removed(GtkNotebook *notebook, GtkWidget *page, guint index, gpointer data)
{
    (void)index;
    (void)data;
    Tabs *tabs = g_object_get_data(G_OBJECT(notebook), "tabs");
    GtkBox *content = tab_strip_content(tabs->strip);
    for (GtkWidget *tab = gtk_widget_get_first_child(GTK_WIDGET(content));
         tab; tab = gtk_widget_get_next_sibling(tab)) {
        if (g_object_get_data(G_OBJECT(tab), "page") != page) continue;
        gtk_box_remove(content, tab);
        break;
    }
}

GtkWidget *tabs_new(const char *scripts_path, TabsChangedFunc changed, gpointer user_data)
{
    GtkWidget *pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *notebook = gtk_notebook_new();
    Tabs *tabs = g_new0(Tabs, 1);
    tabs->strip = g_object_ref_sink(tab_strip_new());
    tabs->changed = changed;
    tabs->user_data = user_data;
    tabs->scripts_dir = g_file_new_for_path(scripts_path);
    tabs->editor_settings = editor_settings_defaults();
    tabs->next_untitled = 1;
    g_object_set_data_full(G_OBJECT(notebook), "tabs", tabs, free_tabs);
    g_object_set_data(G_OBJECT(pane), "tabs", tabs);
    g_object_set_data(G_OBJECT(pane), "notebook", notebook);
    gtk_widget_add_css_class(pane, "script-tabs");
    gtk_widget_add_css_class(tabs->strip, "script-tab-strip");
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(header, "script-tab-header");
    gtk_box_append(GTK_BOX(header), tabs->strip);
    gtk_box_append(GTK_BOX(pane), header);
    gtk_box_append(GTK_BOX(pane), notebook);
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_widget_set_hexpand(notebook, TRUE);
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
    GtkWidget *add = gtk_button_new_with_label("+");
    gtk_widget_add_css_class(add, "tab-add");
    gtk_widget_set_tooltip_text(add, "New script");
    gtk_accessible_update_property(GTK_ACCESSIBLE(add), GTK_ACCESSIBLE_PROPERTY_LABEL, "New script", -1);
    gtk_box_append(GTK_BOX(header), add);
    g_signal_connect(add, "clicked", G_CALLBACK(new_tab), notebook);
    g_signal_connect_after(notebook, "switch-page", G_CALLBACK(switched_page), NULL);
    g_signal_connect(notebook, "page-reordered", G_CALLBACK(page_reordered), NULL);
    g_signal_connect(notebook, "page-removed", G_CALLBACK(page_removed), NULL);
    new_script(GTK_NOTEBOOK(notebook), NULL, "Untitled 1", "print(\"hello from relapsegtk\")");
    return pane;
}

void tabs_set_editor_settings(GtkWidget *tabs, const EditorSettings *settings)
{
    Tabs *state = g_object_get_data(G_OBJECT(tabs), "tabs");
    state->editor_settings = *settings;
    GtkNotebook *book = notebook_for(tabs);
    for (int i = 0; i < gtk_notebook_get_n_pages(book); ++i) {
        Script *script = g_object_get_data(G_OBJECT(gtk_notebook_get_nth_page(book, i)), "script");
        editor_apply_settings(script->editor, settings);
    }
}

void tabs_set_scripts_path(GtkWidget *tabs, const char *path)
{
    Tabs *state = g_object_get_data(G_OBJECT(tabs), "tabs");
    g_autoptr(GFile) folder = g_file_new_for_path(path);
    g_set_object(&state->scripts_dir, folder);
}

void tabs_shutdown(GtkWidget *tabs)
{
    Tabs *state = g_object_get_data(G_OBJECT(tabs), "tabs");
    if (state->stopped)
        return;
    if (state->session_timer)
        g_source_remove(state->session_timer);
    state->session_timer = 0;
    if (state->session_enabled)
        session_writer_queue(state->session_writer, snapshot(notebook_for(tabs)));
    g_clear_pointer(&state->session_writer, session_writer_free);
    state->stopped = TRUE;
    GtkNotebook *notebook = notebook_for(tabs);
    for (int i = 0; i < gtk_notebook_get_n_pages(notebook); ++i) {
        GtkWidget *page = gtk_notebook_get_nth_page(notebook, i);
        stop_script(g_object_get_data(G_OBJECT(page), "script"));
    }
}
