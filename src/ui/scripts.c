#include "scripts.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    ScriptOpenFunc open_script;
    ScriptRenameFunc rename_script;
    gpointer user_data;
    gboolean autoexec;
} ScriptHandlers;

typedef struct {
    GtkListBoxRow parent;
    GFile *file;
    char *edit_name;
    GtkStack *stack;
    GtkImage *icon;
    GtkLabel *label;
    GtkEntry *entry;
    GtkLabel *error;
    GtkPopover *menu;
    ScriptHandlers handlers;
    gboolean editing;
} ScriptRow;

typedef GtkListBoxRowClass ScriptRowClass;

G_DEFINE_TYPE(ScriptRow, script_row, GTK_TYPE_LIST_BOX_ROW)

static void ask_delete(GFile *file, GtkWidget *anchor);
static void duplicate_file(GFile *file);

static void file_action_requested(GtkWidget *widget, const char *action, GVariant *parameter)
{
    (void)parameter;
    ScriptRow *row = (ScriptRow *)widget;
    if (row->editing)
        return;
    if (row->menu)
        gtk_popover_popdown(row->menu);
    if (g_str_equal(action, "script.open"))
        gtk_widget_activate(widget);
    else if (g_str_equal(action, "script.duplicate"))
        duplicate_file(row->file);
    else if (g_str_equal(action, "script.delete"))
        ask_delete(row->file, widget);
}

static void finish_rename(ScriptRow *row)
{
    row->editing = FALSE;
    gtk_stack_set_visible_child_name(row->stack, "name");
    gtk_widget_set_visible(GTK_WIDGET(row->error), FALSE);
    gtk_widget_remove_css_class(GTK_WIDGET(row->entry), "error");
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
}

static void rename_requested(GtkWidget *widget, const char *action, GVariant *parameter)
{
    (void)action;
    (void)parameter;
    ScriptRow *row = (ScriptRow *)widget;
    if (row->menu)
        gtk_popover_popdown(row->menu);
    gtk_editable_set_text(GTK_EDITABLE(row->entry), row->edit_name);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_stack_set_visible_child_name(row->stack, "rename");
    row->editing = TRUE;
    gtk_widget_grab_focus(GTK_WIDGET(row->entry));
    const char *extension = strrchr(row->edit_name, '.');
    int end = extension && extension != row->edit_name
        ? (int)g_utf8_pointer_to_offset(row->edit_name, extension) : -1;
    gtk_editable_select_region(GTK_EDITABLE(row->entry), 0, end);
}

static void rename_activated(GtkEntry *entry, ScriptRow *row)
{
    if (!row->editing)
        return;
    const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (g_str_equal(name, row->edit_name)) {
        finish_rename(row);
        gtk_widget_grab_focus(GTK_WIDGET(row));
        return;
    }
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = row->handlers.rename_script(
        row->file, name, &error, row->handlers.user_data);
    if (!file) {
        gtk_label_set_text(row->error, error ? error->message : "Could not rename script.");
        gtk_widget_set_visible(GTK_WIDGET(row->error), TRUE);
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
        return;
    }
    g_set_object(&row->file, file);
    g_free(row->edit_name);
    row->edit_name = g_strdup(name);
    gtk_label_set_text(row->label, name);
    g_autofree char *path = g_file_get_parse_name(file);
    gtk_widget_set_tooltip_text(GTK_WIDGET(row), path);
    finish_rename(row);
    gtk_widget_grab_focus(GTK_WIDGET(row));
}

static void rename_changed(GtkEditable *entry, ScriptRow *row)
{
    gtk_widget_set_visible(GTK_WIDGET(row->error), FALSE);
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
}

static void focus_left(GtkEventControllerFocus *controller, ScriptRow *row)
{
    (void)controller;
    if (row->editing)
        finish_rename(row);
}

static void show_menu(ScriptRow *row, double x, double y)
{
    if (row->editing)
        return;
    GtkListBox *list = GTK_LIST_BOX(gtk_widget_get_parent(GTK_WIDGET(row)));
    gtk_list_box_select_row(list, GTK_LIST_BOX_ROW(row));
    gtk_widget_grab_focus(GTK_WIDGET(row));
    if (!row->menu) {
        row->menu = GTK_POPOVER(gtk_popover_new());
        gtk_popover_set_has_arrow(row->menu, FALSE);
        gtk_widget_set_parent(GTK_WIDGET(row->menu), GTK_WIDGET(row));
        gtk_widget_add_css_class(GTK_WIDGET(row->menu), "script-menu");
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        const struct {
            const char *label;
            const char *action;
        } items[] = {
            {"Open", "script.open"},
            {"Rename", "script.rename"},
            {"Duplicate", "script.duplicate"},
            {"Delete", "script.delete"}
        };
        for (guint i = 0; i < G_N_ELEMENTS(items); ++i) {
            GtkWidget *button = gtk_button_new_with_label(items[i].label);
            gtk_widget_add_css_class(button, "flat");
            gtk_widget_add_css_class(button, "menu-item");
            gtk_widget_set_halign(button, GTK_ALIGN_FILL);
            gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
            gtk_label_set_xalign(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(button))), 0);
            gtk_actionable_set_action_name(GTK_ACTIONABLE(button), items[i].action);
            gtk_box_append(GTK_BOX(box), button);
        }
        gtk_popover_set_child(row->menu, box);
    }
    GdkRectangle point = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(row->menu, &point);
    gtk_popover_popup(row->menu);
}

static void right_clicked(GtkGestureClick *gesture, int presses, double x, double y, ScriptRow *row)
{
    (void)presses;
    if (!row->editing) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        show_menu(row, x, y);
    }
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
                            GdkModifierType modifiers, ScriptRow *row)
{
    (void)controller;
    (void)keycode;
    if (row->editing) {
        if (keyval != GDK_KEY_Escape)
            return FALSE;
        finish_rename(row);
        gtk_widget_grab_focus(GTK_WIDGET(row));
        return TRUE;
    }
    if (keyval == GDK_KEY_Menu || (keyval == GDK_KEY_F10 && (modifiers & GDK_SHIFT_MASK))) {
        show_menu(row, 8, gtk_widget_get_height(GTK_WIDGET(row)));
        return TRUE;
    }
    if (keyval == GDK_KEY_F2) {
        gtk_widget_activate_action(GTK_WIDGET(row), "script.rename", NULL);
        return TRUE;
    }
    return FALSE;
}

static void script_row_size_allocate(GtkWidget *widget, int width, int height, int baseline)
{
    GTK_WIDGET_CLASS(script_row_parent_class)->size_allocate(widget, width, height, baseline);
    ScriptRow *row = (ScriptRow *)widget;
    if (row->menu)
        gtk_popover_present(row->menu);
}

static void script_row_dispose(GObject *object)
{
    ScriptRow *row = (ScriptRow *)object;
    row->editing = FALSE;
    if (row->menu) {
        gtk_widget_unparent(GTK_WIDGET(row->menu));
        row->menu = NULL;
    }
    g_clear_object(&row->file);
    g_clear_pointer(&row->edit_name, g_free);
    G_OBJECT_CLASS(script_row_parent_class)->dispose(object);
}

static void script_row_class_init(ScriptRowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = script_row_dispose;
    GTK_WIDGET_CLASS(klass)->size_allocate = script_row_size_allocate;
    gtk_widget_class_install_action(GTK_WIDGET_CLASS(klass), "script.rename", NULL, rename_requested);
    gtk_widget_class_install_action(GTK_WIDGET_CLASS(klass), "script.open", NULL, file_action_requested);
    gtk_widget_class_install_action(GTK_WIDGET_CLASS(klass), "script.duplicate", NULL, file_action_requested);
    gtk_widget_class_install_action(GTK_WIDGET_CLASS(klass), "script.delete", NULL, file_action_requested);
}

static void script_row_init(ScriptRow *row)
{
    gtk_widget_add_css_class(GTK_WIDGET(row), "script-row");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    row->stack = GTK_STACK(gtk_stack_new());
    row->label = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_xalign(row->label, 0);
    gtk_label_set_ellipsize(row->label, PANGO_ELLIPSIZE_END);
    gtk_stack_add_named(row->stack, GTK_WIDGET(row->label), "name");
    row->entry = GTK_ENTRY(gtk_entry_new());
    gtk_widget_add_css_class(GTK_WIDGET(row->entry), "script-rename");
    gtk_editable_set_width_chars(GTK_EDITABLE(row->entry), 1);
    gtk_widget_set_hexpand(GTK_WIDGET(row->entry), TRUE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(row->entry),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Script filename", -1);
    gtk_stack_add_named(row->stack, GTK_WIDGET(row->entry), "rename");
    gtk_stack_set_hhomogeneous(row->stack, FALSE);
    gtk_stack_set_vhomogeneous(row->stack, FALSE);
    GtkWidget *title = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    row->icon = GTK_IMAGE(gtk_image_new());
    gtk_image_set_pixel_size(row->icon, 14);
    gtk_box_append(GTK_BOX(title), GTK_WIDGET(row->icon));
    gtk_widget_set_hexpand(GTK_WIDGET(row->stack), TRUE);
    gtk_box_append(GTK_BOX(title), GTK_WIDGET(row->stack));
    gtk_box_append(GTK_BOX(box), title);
    row->error = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(row->error), "script-rename-error");
    gtk_label_set_xalign(row->error, 0);
    gtk_label_set_wrap(row->error, TRUE);
    gtk_label_set_wrap_mode(row->error, PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(row->error, 18);
    gtk_widget_set_visible(GTK_WIDGET(row->error), FALSE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(row->error));
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_signal_connect(row->entry, "activate", G_CALLBACK(rename_activated), row);
    g_signal_connect(row->entry, "changed", G_CALLBACK(rename_changed), row);

    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(focus_left), row);
    gtk_widget_add_controller(GTK_WIDGET(row->entry), focus);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), row);
    gtk_widget_add_controller(GTK_WIDGET(row), keys);
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    g_signal_connect(click, "pressed", G_CALLBACK(right_clicked), row);
    gtk_widget_add_controller(GTK_WIDGET(row), GTK_EVENT_CONTROLLER(click));
}

static gboolean is_file(gpointer item, gpointer user_data)
{
    (void)user_data;
    return g_file_info_get_file_type(G_FILE_INFO(item)) == G_FILE_TYPE_REGULAR;
}

static gint compare_scripts(gconstpointer first, gconstpointer second, gpointer user_data)
{
    (void)user_data;
    g_autofree char *a = g_utf8_casefold(g_file_info_get_display_name(G_FILE_INFO((gpointer)first)), -1);
    g_autofree char *b = g_utf8_casefold(g_file_info_get_display_name(G_FILE_INFO((gpointer)second)), -1);
    return g_utf8_collate(a, b);
}

static void async_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    const char *what = user_data;
    g_autoptr(GError) error = NULL;
    gboolean ok = TRUE;
    if (g_strcmp0(what, "delete") == 0)
        ok = g_file_delete_finish(G_FILE(source), result, &error);
    else if (g_strcmp0(what, "copy") == 0)
        ok = g_file_copy_finish(G_FILE(source), result, &error);
    if (!ok && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning("%s failed: %s", what, error ? error->message : "unknown");
}

static void delete_confirmed(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GFile *file = user_data;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
    if (choice == 1)
        g_file_delete_async(file, G_PRIORITY_DEFAULT, NULL, async_done, (gpointer)"delete");
    g_object_unref(file);
}

static void ask_delete(GFile *file, GtkWidget *anchor)
{
    g_autofree char *basename = g_file_get_basename(file);
    g_autofree char *msg = g_strdup_printf("Delete %s?", basename ? basename : "this script");
    GtkAlertDialog *alert = gtk_alert_dialog_new("%s", msg);
    gtk_alert_dialog_set_detail(alert, "This will remove the file from disk.");
    const char *buttons[] = {"Cancel", "Delete", NULL};
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 0);
    gtk_alert_dialog_set_default_button(alert, 0);
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(anchor));
    gtk_alert_dialog_choose(alert, parent, NULL, delete_confirmed, g_object_ref(file));
    g_object_unref(alert);
}

static void duplicate_file(GFile *file)
{
    g_autofree char *base = g_file_get_basename(file);
    if (!base) return;
    const char *dot = strrchr(base, '.');
    g_autofree char *stem = dot ? g_strndup(base, (gsize)(dot - base)) : g_strdup(base);
    const char *ext = dot ? dot : "";
    g_autoptr(GFile) parent = g_file_get_parent(file);
    if (!parent) return;
    for (int i = 1; i < 100; ++i) {
        g_autofree char *name = i == 1
            ? g_strdup_printf("%s copy%s", stem, ext)
            : g_strdup_printf("%s copy %d%s", stem, i, ext);
        g_autoptr(GFile) target = g_file_get_child(parent, name);
        if (!g_file_query_exists(target, NULL)) {
            g_file_copy_async(file, target, G_FILE_COPY_NONE, G_PRIORITY_DEFAULT,
                              NULL, NULL, NULL, async_done, (gpointer)"copy");
            return;
        }
    }
}

static GtkWidget *create_row(gpointer item, gpointer user_data)
{
    GFileInfo *info = item;
    ScriptRow *row = g_object_new(script_row_get_type(), NULL);
    row->handlers = *(ScriptHandlers *)user_data;
    gtk_image_set_from_resource(row->icon, row->handlers.autoexec
        ? "/org/relapse/RelapseGtk/icons/autoexec.svg"
        : "/org/relapse/RelapseGtk/icons/editor.svg");
    row->edit_name = g_strdup(g_file_info_get_edit_name(info));
    gtk_label_set_text(row->label, g_file_info_get_display_name(info));
    GFile *file = G_FILE(g_file_info_get_attribute_object(info, "standard::file"));
    row->file = g_object_ref(file);
    g_autofree char *path = g_file_get_parse_name(file);
    gtk_widget_set_tooltip_text(GTK_WIDGET(row), path);
    return GTK_WIDGET(row);
}

static void row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    (void)list;
    ScriptHandlers *handler = user_data;
    ScriptRow *script = (ScriptRow *)row;
    if (!script->editing)
        handler->open_script(script->file, gtk_label_get_text(script->label), handler->user_data);
}

static void update_placeholder(GtkDirectoryList *directory, GParamSpec *property, GtkLabel *label)
{
    (void)property;
    const GError *error = gtk_directory_list_get_error(directory);
    if (error) {
        g_autofree char *message = g_strdup_printf("Could not read scripts:\n%s", error->message);
        gtk_label_set_text(label, message);
    } else {
        gtk_label_set_text(label, gtk_directory_list_is_loading(directory)
            ? "Loading scripts…" : "No files yet.");
    }
}

typedef struct {
    GtkDirectoryList *directory;
    GtkWidget *title;
    GtkWidget *source_icon;
    GtkLabel *placeholder;
    ScriptHandlers *handlers;
    char *scripts_path;
    char *autoexec_path;
    gboolean autoexec;
} Scripts;

static void free_scripts(gpointer user_data)
{
    Scripts *state = user_data;
    g_free(state->scripts_path);
    g_free(state->autoexec_path);
    g_free(state);
}

static void update_directory(Scripts *state)
{
    const char *path = state->autoexec ? state->autoexec_path : state->scripts_path;
    state->handlers->autoexec = state->autoexec;
    gtk_image_set_from_resource(GTK_IMAGE(state->source_icon), state->autoexec
        ? "/org/relapse/RelapseGtk/icons/autoexec.svg"
        : "/org/relapse/RelapseGtk/icons/editor.svg");
    g_autofree char *display_path = g_filename_display_name(path);
    gtk_widget_set_tooltip_text(state->title, display_path);
    if (g_mkdir_with_parents(path, 0700) != 0) {
        gtk_directory_list_set_file(state->directory, NULL);
        g_autofree char *message = g_strdup_printf("Could not create scripts folder:\n%s", g_strerror(errno));
        gtk_label_set_text(state->placeholder, message);
        return;
    }
    g_autoptr(GFile) folder = g_file_new_for_path(path);
    gtk_directory_list_set_file(state->directory, folder);
    update_placeholder(state->directory, NULL, state->placeholder);
}

void scripts_set_directory(GtkWidget *scripts, const char *path)
{
    Scripts *state = g_object_get_data(G_OBJECT(scripts), "scripts-state");
    g_free(state->scripts_path);
    state->scripts_path = g_strdup(path);
    update_directory(state);
}

static void source_changed(GtkDropDown *source, GParamSpec *property, gpointer user_data)
{
    (void)property;
    GtkWidget *sidebar = user_data;
    Scripts *state = g_object_get_data(G_OBJECT(sidebar), "scripts-state");
    state->autoexec = gtk_drop_down_get_selected(source) == 1;
    update_directory(state);
}

GtkWidget *scripts_new(const char *path, ScriptOpenFunc open_script,
                       ScriptRenameFunc rename_script, gpointer user_data)
{
    g_return_val_if_fail(path != NULL && open_script != NULL && rename_script != NULL, NULL);
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(sidebar, "scripts-sidebar");
    gtk_widget_set_size_request(sidebar, 160, -1);

    g_autoptr(GtkStringList) sources = gtk_string_list_new(NULL);
    gtk_string_list_append(sources, "Scripts");
    gtk_string_list_append(sources, "Autoexec");
    GtkWidget *title = gtk_drop_down_new(G_LIST_MODEL(sources), NULL);
    gtk_widget_add_css_class(title, "scripts-title");
    gtk_widget_add_css_class(title, "socket-picker");
    gtk_drop_down_set_enable_search(GTK_DROP_DOWN(title), FALSE);
    GtkWidget *source_icon = gtk_image_new_from_resource("/org/relapse/RelapseGtk/icons/editor.svg");
    gtk_image_set_pixel_size(GTK_IMAGE(source_icon), 16);
    GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(heading), source_icon);
    gtk_box_append(GTK_BOX(heading), title);
    gtk_box_append(GTK_BOX(sidebar), heading);

    GtkWidget *list = gtk_list_box_new();
    gtk_widget_add_css_class(list, "scripts-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), TRUE);
    GtkWidget *placeholder = gtk_label_new(NULL);
    gtk_widget_add_css_class(placeholder, "scripts-empty");
    gtk_label_set_wrap(GTK_LABEL(placeholder), TRUE);
    gtk_label_set_xalign(GTK_LABEL(placeholder), 0);
    gtk_widget_set_valign(placeholder, GTK_ALIGN_START);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(list), placeholder);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll), FALSE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(sidebar), scroll);

    ScriptHandlers *handler = g_new(ScriptHandlers, 1);
    *handler = (ScriptHandlers){open_script, rename_script, user_data};
    g_object_set_data_full(G_OBJECT(list), "script-handlers", handler, g_free);
    g_signal_connect(list, "row-activated", G_CALLBACK(row_activated), handler);

    GtkDirectoryList *directory = gtk_directory_list_new(
        "standard::name,standard::display-name,standard::edit-name,standard::type", NULL);
    gtk_directory_list_set_monitored(directory, TRUE);
    g_signal_connect_object(directory, "notify::loading", G_CALLBACK(update_placeholder), placeholder, 0);
    g_signal_connect_object(directory, "notify::error", G_CALLBACK(update_placeholder), placeholder, 0);
    GtkFilterListModel *filtered = gtk_filter_list_model_new(G_LIST_MODEL(directory),
        GTK_FILTER(gtk_custom_filter_new(is_file, NULL, NULL)));
    GtkSortListModel *sorted = gtk_sort_list_model_new(G_LIST_MODEL(filtered),
        GTK_SORTER(gtk_custom_sorter_new(compare_scripts, NULL, NULL)));
    gtk_list_box_bind_model(GTK_LIST_BOX(list), G_LIST_MODEL(sorted), create_row, handler, NULL);
    g_object_unref(sorted);

    Scripts *state = g_new0(Scripts, 1);
    state->directory = directory;
    state->title = title;
    state->source_icon = source_icon;
    state->placeholder = GTK_LABEL(placeholder);
    state->handlers = handler;
    state->scripts_path = g_strdup(path);
    state->autoexec_path = g_build_filename(g_get_home_dir(), ".config", "relapse", "autoexec", NULL);
    g_object_set_data_full(G_OBJECT(sidebar), "scripts-state", state, free_scripts);
    update_directory(state);
    g_signal_connect(title, "notify::selected", G_CALLBACK(source_changed), sidebar);
    return sidebar;
}
