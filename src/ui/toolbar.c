#include "toolbar.h"

typedef struct {
    GWeakRef parent;
    GtkFileDialog *dialog;
    GCancellable *pending;
    GtkWidget *open;
    GtkWidget *execute;
    GtkLabel *execute_label;
    GtkWidget *status;
    GtkWidget *status_label;
    ToolbarHandlers handlers;
    gpointer user_data;
    gboolean stopped;
} Toolbar;

static Toolbar *toolbar_data(GtkWidget *toolbar)
{
    return g_object_get_data(G_OBJECT(toolbar), "toolbar");
}

void toolbar_set_execute_state(GtkWidget *toolbar, gboolean enabled, guint ready, guint selected)
{
    Toolbar *state = toolbar_data(toolbar);
    gtk_widget_set_sensitive(state->execute, enabled);
    g_autofree char *label = selected > 1 ? g_strdup_printf("Execute %u", ready) : g_strdup("Execute");
    gtk_label_set_text(state->execute_label, label);
    g_autofree char *hint = selected > 1
        ? g_strdup_printf("Execute on %u of %u selected clients that are ready.", ready, selected)
        : g_strdup(selected ? "Execute on the selected client" : "Select a client to execute scripts");
    gtk_widget_set_tooltip_text(state->execute, hint);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->execute), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

void toolbar_set_scripts_path(GtkWidget *toolbar, const char *path)
{
    Toolbar *state = toolbar_data(toolbar);
    g_autoptr(GFile) folder = g_file_new_for_path(path);
    gtk_file_dialog_set_initial_folder(state->dialog, folder);
}

void toolbar_set_status(GtkWidget *toolbar, const char *label, const char *description, gboolean ready)
{
    Toolbar *state = toolbar_data(toolbar);
    gtk_label_set_text(GTK_LABEL(state->status_label), label ? label : "");
    gtk_widget_set_visible(state->status_label, label && *label);
    gtk_widget_set_tooltip_text(state->status, description);
    if (ready)
        gtk_widget_add_css_class(state->status, "ready");
    else
        gtk_widget_remove_css_class(state->status, "ready");
}

static void stop_toolbar(Toolbar *toolbar)
{
    toolbar->stopped = TRUE;
    if (toolbar->pending)
        g_cancellable_cancel(toolbar->pending);
}

void toolbar_shutdown(GtkWidget *toolbar)
{
    stop_toolbar(toolbar_data(toolbar));
}

static void free_toolbar(gpointer user_data)
{
    Toolbar *toolbar = user_data;
    stop_toolbar(toolbar);
    g_weak_ref_clear(&toolbar->parent);
    g_clear_object(&toolbar->pending);
    g_object_unref(toolbar->dialog);
    g_free(toolbar);
}

static void file_chosen(GObject *dialog, GAsyncResult *result, gpointer user_data)
{
    GWeakRef *owner = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(dialog), result, &error);
    g_autoptr(GtkWidget) widget = g_weak_ref_get(owner);
    Toolbar *toolbar = widget ? toolbar_data(widget) : NULL;
    if (toolbar && !toolbar->stopped) {
        g_clear_object(&toolbar->pending);
        gtk_widget_set_sensitive(toolbar->open, TRUE);
        if (file) {
            g_autoptr(GFile) folder = g_file_get_parent(file);
            if (folder)
                gtk_file_dialog_set_initial_folder(toolbar->dialog, folder);
            g_autofree char *basename = g_file_get_basename(file);
            g_autofree char *name = basename ? g_filename_display_name(basename) : g_strdup("script.luau");
            if (toolbar->handlers.open_script)
                toolbar->handlers.open_script(file, name, toolbar->user_data);
        } else if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
                   !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
                   !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_printerr("Could not open file picker: %s\n", error->message);
            g_autoptr(GtkWindow) parent = g_weak_ref_get(&toolbar->parent);
            if (parent) {
                g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not open file picker");
                gtk_alert_dialog_set_detail(alert, error->message);
                gtk_alert_dialog_show(alert, parent);
            }
        }
    }
    g_weak_ref_clear(owner);
    g_free(owner);
}

static void open_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkWidget *widget = user_data;
    Toolbar *toolbar = toolbar_data(widget);
    if (toolbar->stopped || toolbar->pending)
        return;
    g_autoptr(GtkWindow) parent = g_weak_ref_get(&toolbar->parent);
    if (!parent)
        return;
    toolbar->pending = g_cancellable_new();
    gtk_widget_set_sensitive(toolbar->open, FALSE);
    GWeakRef *owner = g_new0(GWeakRef, 1);
    g_weak_ref_init(owner, widget);
    gtk_file_dialog_open(toolbar->dialog, parent, toolbar->pending, file_chosen, owner);
}

static void execute_clicked(GtkButton *button, gpointer user_data)
{
    Toolbar *toolbar = user_data;
    if (!toolbar->stopped && gtk_widget_get_sensitive(GTK_WIDGET(button)) && toolbar->handlers.execute)
        toolbar->handlers.execute(toolbar->user_data);
}

static void add_tool(GtkWidget *toolbar, GtkWidget *button, const char *icon, const char *hint)
{
    GtkWidget *image = gtk_image_new_from_resource(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(image), 16);
    gtk_button_set_child(GTK_BUTTON(button), image);
    gtk_widget_add_css_class(button, "editor-tool");
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(button, hint);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, hint, -1);
    gtk_box_append(GTK_BOX(toolbar), button);
}

GtkWidget *toolbar_new(GtkWindow *parent, const char *scripts_path, GtkWidget *socket_picker,
                       const ToolbarHandlers *handlers, gpointer user_data)
{
    Toolbar *state = g_new0(Toolbar, 1);
    g_weak_ref_init(&state->parent, parent);
    if (handlers)
        state->handlers = *handlers;
    state->user_data = user_data;
    state->dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(state->dialog, "Open script");
    gtk_file_dialog_set_accept_label(state->dialog, "Open");
    gtk_file_dialog_set_modal(state->dialog, TRUE);
    g_autoptr(GFile) folder = g_file_new_for_path(scripts_path);
    gtk_file_dialog_set_initial_folder(state->dialog, folder);

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
    gtk_file_dialog_set_filters(state->dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(state->dialog, scripts);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_widget_add_css_class(toolbar, "editor-toolbar");
    g_object_set_data_full(G_OBJECT(toolbar), "toolbar", state, free_toolbar);
    state->open = gtk_button_new();
    add_tool(toolbar, state->open, "/org/relapse/RelapseGtk/icons/folder-open.svg", "Open script");
    g_signal_connect(state->open, "clicked", G_CALLBACK(open_clicked), toolbar);

    GtkWidget *save = gtk_button_new();
    add_tool(toolbar, save, "/org/relapse/RelapseGtk/icons/save.svg", "Save script (Ctrl+S)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(save), "win.save");

    GtkWidget *output = gtk_toggle_button_new();
    add_tool(toolbar, output, "/org/relapse/RelapseGtk/icons/output.svg", "Show script output");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(output), "win.show-output");

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(toolbar), spacer);
    state->status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(state->status, "connection-status");
    gtk_widget_set_valign(state->status, GTK_ALIGN_CENTER);
    GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(dot, "status-dot");
    gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(state->status), dot);
    state->status_label = gtk_label_new(NULL);
    gtk_widget_add_css_class(state->status_label, "status-label");
    gtk_label_set_xalign(GTK_LABEL(state->status_label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(state->status_label), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(state->status), state->status_label);
    gtk_box_append(GTK_BOX(toolbar), state->status);
    toolbar_set_status(toolbar, "Disconnected", "Not connected. Retrying automatically.", FALSE);
    gtk_widget_set_valign(socket_picker, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(toolbar), socket_picker);

    state->execute = gtk_button_new();
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *play = gtk_image_new_from_resource("/org/relapse/RelapseGtk/icons/play.svg");
    gtk_image_set_pixel_size(GTK_IMAGE(play), 14);
    gtk_box_append(GTK_BOX(content), play);
    state->execute_label = GTK_LABEL(gtk_label_new("Execute"));
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(state->execute_label));
    gtk_button_set_child(GTK_BUTTON(state->execute), content);
    gtk_widget_set_valign(state->execute, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(state->execute, "execute-script-button");
    gtk_widget_set_sensitive(state->execute, FALSE);
    gtk_box_append(GTK_BOX(toolbar), state->execute);
    g_signal_connect(state->execute, "clicked", G_CALLBACK(execute_clicked), state);
    return toolbar;
}
