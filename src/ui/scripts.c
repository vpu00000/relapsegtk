#include "scripts.h"
#include "picker.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    ScriptOpenFunc open_script;
    ScriptMoveFunc move_script;
    ScriptDirectoryFunc directory_changed;
    gpointer user_data;
} ScriptHandlers;

typedef struct {
    GtkDirectoryList *directory;
    GtkListBox *list;
    GFile *active_file;
    ScriptHandlers handlers;
    GtkWidget *title;
    GtkWidget *sidebar;
    GtkListBoxRow *drag_row;
    GtkWidget *drop_highlight;
    GtkAdjustment *scroll;
    guint scroll_timer;
    double drag_x, drag_y, scroll_step;
    GtkPopover *root_menu;
    GtkLabel *placeholder;
    char *scripts_path;
    GCancellable *picker;
    gboolean stopped;
} Scripts;

typedef struct {
    GtkListBoxRow parent;
    GFile *file;
    char *edit_name;
    GtkTreeExpander *expander;
    GtkLabel *label;
    GtkEntry *entry;
    GtkLabel *error;
    GtkPopover *menu;
    Scripts *scripts;
    gboolean editing;
} ScriptRow;

typedef GtkListBoxRowClass ScriptRowClass;

G_DEFINE_TYPE(ScriptRow, script_row, GTK_TYPE_LIST_BOX_ROW)

static void ask_remove(ScriptRow *row);
static void duplicate_file(GFile *file);
static void begin_rename(ScriptRow *row);

static void file_action_requested(GtkWidget *widget, const char *action, GVariant *parameter)
{
    (void)parameter;
    ScriptRow *row = (ScriptRow *)widget;
    if (row->editing)
        return;
    if (row->menu)
        gtk_popover_popdown(row->menu);
    if (row->expander) {
        if (g_str_equal(action, "script.trash"))
            ask_remove(row);
    } else if (g_str_equal(action, "script.open"))
        gtk_widget_activate(widget);
    else if (g_str_equal(action, "script.duplicate"))
        duplicate_file(row->file);
    else if (g_str_equal(action, "script.delete"))
        ask_remove(row);
}

static void set_row_content(ScriptRow *row, GtkWidget *child)
{
    if (row->expander)
        gtk_tree_expander_set_child(row->expander, child);
    else
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), child);
}

static void finish_rename(ScriptRow *row)
{
    row->editing = FALSE;
    set_row_content(row, GTK_WIDGET(row->label));
    row->entry = NULL;
    row->error = NULL;
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
}

static void rename_requested(GtkWidget *widget, const char *action, GVariant *parameter)
{
    (void)action;
    (void)parameter;
    ScriptRow *row = (ScriptRow *)widget;
    if (row->menu)
        gtk_popover_popdown(row->menu);
    if (row->editing) return;
    begin_rename(row);
    gtk_widget_grab_focus(GTK_WIDGET(row->entry));
    const char *extension = row->expander ? NULL : strrchr(row->edit_name, '.');
    int end = extension && extension != row->edit_name
        ? (int)g_utf8_pointer_to_offset(row->edit_name, extension) : -1;
    gtk_editable_select_region(GTK_EDITABLE(row->entry), 0, end);
}

static GFile *named_child(GFile *parent, const char *name, GError **error)
{
    if (!parent)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Choose a script folder first.");
    else if (!*name || g_str_equal(name, ".") || g_str_equal(name, "..") || strchr(name, '/'))
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME, "Enter a name without slashes.");
    else
        return g_file_get_child_for_display_name(parent, name, error);
    return NULL;
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
    ScriptHandlers *handlers = &row->scripts->handlers;
    g_autoptr(GFile) parent = g_file_get_parent(row->file);
    g_autoptr(GFile) file = named_child(parent, name, &error);
    if (!file || !handlers->move_script(row->file, file, &error, handlers->user_data)) {
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

static GtkWidget *menu_button(GtkWidget *box, const char *label)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(button, "menu-item");
    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
    gtk_label_set_xalign(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(button))), 0);
    gtk_box_append(GTK_BOX(box), button);
    return button;
}

static void create_item(GtkWidget *widget, gpointer user_data)
{
    gboolean directory = GPOINTER_TO_INT(user_data);
    GtkPopover *menu = GTK_POPOVER(gtk_widget_get_ancestor(widget, GTK_TYPE_POPOVER));
    GtkWidget *entry = gtk_widget_get_first_child(gtk_popover_get_child(menu));
    Scripts *scripts = g_object_get_data(G_OBJECT(menu), "scripts");
    GtkWidget *anchor = gtk_widget_get_parent(GTK_WIDGET(menu));
    ScriptRow *row = anchor == scripts->sidebar ? NULL : (ScriptRow *)anchor;
    GFile *parent = row ? row->file : gtk_directory_list_get_file(scripts->directory);
    g_autofree char *name = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) child = named_child(parent, name, &error);
    if (child) {
        if (directory) {
            g_file_make_directory(child, NULL, &error);
        } else {
            g_autoptr(GFileOutputStream) stream = g_file_create(child, G_FILE_CREATE_NONE, NULL, &error);
            if (stream) g_output_stream_close(G_OUTPUT_STREAM(stream), NULL, &error);
        }
    }
    if (error) {
        GtkWidget *message = gtk_widget_get_next_sibling(entry);
        gtk_label_set_text(GTK_LABEL(message), g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS)
            ? "That name is already in use." : error->message);
        gtk_widget_set_visible(message, TRUE);
        gtk_widget_grab_focus(entry);
        return;
    }
    gtk_popover_popdown(menu);
    if (row) gtk_tree_list_row_set_expanded(gtk_tree_expander_get_list_row(row->expander), TRUE);
    if (!directory) scripts->handlers.open_script(child, name, scripts->handlers.user_data);
}

static void name_new_item(GtkButton *button, gpointer user_data)
{
    gboolean directory = GPOINTER_TO_INT(user_data);
    GtkPopover *menu = GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER));
    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *entry = gtk_entry_new();
    gtk_widget_add_css_class(entry, "script-rename");
    gtk_editable_set_width_chars(GTK_EDITABLE(entry), 18);
    gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
        directory ? "New folder name" : "New script name", -1);
    gtk_editable_set_text(GTK_EDITABLE(entry), directory ? "New folder" : "Untitled.luau");
    gtk_box_append(GTK_BOX(form), entry);
    GtkWidget *message = gtk_label_new(NULL);
    gtk_widget_add_css_class(message, "script-rename-error");
    gtk_label_set_wrap(GTK_LABEL(message), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(message), 26);
    gtk_widget_set_visible(message, FALSE);
    gtk_box_append(GTK_BOX(form), message);
    GtkWidget *create = gtk_button_new_with_label(directory ? "Create folder" : "Create script");
    gtk_box_append(GTK_BOX(form), create);
    g_signal_connect(entry, "activate", G_CALLBACK(create_item), user_data);
    g_signal_connect(create, "clicked", G_CALLBACK(create_item), user_data);
    gtk_popover_set_child(menu, form);
    gtk_widget_grab_focus(entry);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, directory ? -1 : (int)strlen("Untitled"));
}

static void popup_menu(Scripts *scripts, ScriptRow *row, double x, double y)
{
    GtkPopover **slot = row ? &row->menu : &scripts->root_menu;
    if (!*slot) {
        *slot = GTK_POPOVER(gtk_popover_new());
        gtk_popover_set_has_arrow(*slot, FALSE);
        gtk_widget_set_parent(GTK_WIDGET(*slot), row ? GTK_WIDGET(row) : scripts->sidebar);
        gtk_widget_add_css_class(GTK_WIDGET(*slot), "script-menu");
        g_object_set_data(G_OBJECT(*slot), "scripts", scripts);
    }
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    if (!row || row->expander) {
        g_signal_connect(menu_button(box, "New Script…"), "clicked", G_CALLBACK(name_new_item), GINT_TO_POINTER(FALSE));
        g_signal_connect(menu_button(box, "New Folder…"), "clicked", G_CALLBACK(name_new_item), GINT_TO_POINTER(TRUE));
    }
    if (row) {
        if (!row->expander)
            gtk_actionable_set_action_name(GTK_ACTIONABLE(menu_button(box, "Open")), "script.open");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(menu_button(box, "Rename")), "script.rename");
        if (row->expander)
            gtk_actionable_set_action_name(GTK_ACTIONABLE(menu_button(box, "Move to Trash…")), "script.trash");
        else {
            gtk_actionable_set_action_name(GTK_ACTIONABLE(menu_button(box, "Duplicate")), "script.duplicate");
            gtk_actionable_set_action_name(GTK_ACTIONABLE(menu_button(box, "Delete")), "script.delete");
        }
    }
    gtk_popover_set_child(*slot, box);
    GdkRectangle point = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(*slot, &point);
    gtk_popover_popup(*slot);
}

static void show_menu(ScriptRow *row, double x, double y)
{
    if (row->editing) return;
    GtkListBox *list = GTK_LIST_BOX(gtk_widget_get_parent(GTK_WIDGET(row)));
    gtk_list_box_select_row(list, GTK_LIST_BOX_ROW(row));
    gtk_widget_grab_focus(GTK_WIDGET(row));
    popup_menu(row->scripts, row, x, y);
}

static void right_clicked(GtkGestureClick *gesture, int presses, double x, double y, ScriptRow *row)
{
    (void)presses;
    if (!row->editing) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        show_menu(row, x, y);
    }
}

static void root_right_clicked(GtkGestureClick *gesture, int presses, double x, double y, Scripts *scripts)
{
    (void)presses;
    GtkWidget *hit = gtk_widget_pick(scripts->sidebar, x, y, GTK_PICK_DEFAULT);
    if (!hit || gtk_widget_get_ancestor(hit, script_row_get_type()) ||
        gtk_widget_get_ancestor(hit, GTK_TYPE_POPOVER) || gtk_widget_get_ancestor(hit, GTK_TYPE_SCROLLBAR))
        return;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    popup_menu(scripts, NULL, x, y);
}

static gboolean menu_key(guint keyval, GdkModifierType modifiers)
{
    return keyval == GDK_KEY_Menu || (keyval == GDK_KEY_F10 && (modifiers & GDK_SHIFT_MASK));
}

static gboolean root_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
                                 GdkModifierType modifiers, Scripts *scripts)
{
    (void)controller;
    (void)keycode;
    if (!menu_key(keyval, modifiers)) return FALSE;
    GtkWidget *header = gtk_widget_get_parent(scripts->title);
    popup_menu(scripts, NULL, 8, gtk_widget_get_height(header));
    return TRUE;
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
    if (menu_key(keyval, modifiers)) {
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
    g_clear_object(&row->label);
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
    gtk_widget_class_install_action(GTK_WIDGET_CLASS(klass), "script.trash", NULL, file_action_requested);
}

static void begin_rename(ScriptRow *row)
{
    row->editing = TRUE;
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_margin_start(box, gtk_widget_get_margin_start(GTK_WIDGET(row->label)));
    row->entry = GTK_ENTRY(gtk_entry_new());
    gtk_widget_add_css_class(GTK_WIDGET(row->entry), "script-rename");
    gtk_editable_set_width_chars(GTK_EDITABLE(row->entry), 1);
    gtk_widget_set_hexpand(GTK_WIDGET(row->entry), TRUE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(row->entry),
        GTK_ACCESSIBLE_PROPERTY_LABEL, row->expander ? "Folder name" : "Script filename", -1);
    gtk_editable_set_text(GTK_EDITABLE(row->entry), row->edit_name);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(row->entry));
    row->error = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(row->error), "script-rename-error");
    gtk_label_set_xalign(row->error, 0);
    gtk_label_set_wrap(row->error, TRUE);
    gtk_label_set_wrap_mode(row->error, PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(row->error, 18);
    gtk_widget_set_visible(GTK_WIDGET(row->error), FALSE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(row->error));
    set_row_content(row, box);
    g_signal_connect(row->entry, "activate", G_CALLBACK(rename_activated), row);
    g_signal_connect(row->entry, "changed", G_CALLBACK(rename_changed), row);

    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(focus_left), row);
    gtk_widget_add_controller(GTK_WIDGET(row->entry), focus);
}

static void script_row_init(ScriptRow *row)
{
    gtk_widget_add_css_class(GTK_WIDGET(row), "script-row");
    row->label = GTK_LABEL(g_object_ref_sink(gtk_label_new(NULL)));
    gtk_widget_add_css_class(GTK_WIDGET(row->label), "script-filename");
    gtk_label_set_xalign(row->label, 0);
    gtk_label_set_ellipsize(row->label, PANGO_ELLIPSIZE_END);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), row);
    gtk_widget_add_controller(GTK_WIDGET(row), keys);
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    g_signal_connect(click, "pressed", G_CALLBACK(right_clicked), row);
    gtk_widget_add_controller(GTK_WIDGET(row), GTK_EVENT_CONTROLLER(click));
}

static const char *file_attributes =
    "standard::name,standard::display-name,standard::edit-name,standard::type,standard::is-symlink";

static gboolean is_library_item(gpointer item, gpointer user_data)
{
    (void)user_data;
    GFileInfo *info = G_FILE_INFO(item);
    GFileType type = g_file_info_get_file_type(info);
    return type == G_FILE_TYPE_REGULAR ||
        (type == G_FILE_TYPE_DIRECTORY && !g_file_info_get_is_symlink(info));
}

static gint compare_scripts(gconstpointer first, gconstpointer second, gpointer user_data)
{
    (void)user_data;
    GFileInfo *a = (GFileInfo *)first;
    GFileInfo *b = (GFileInfo *)second;
    gboolean folder_a = g_file_info_get_file_type(a) == G_FILE_TYPE_DIRECTORY;
    gboolean folder_b = g_file_info_get_file_type(b) == G_FILE_TYPE_DIRECTORY;
    if (folder_a != folder_b) return folder_a ? -1 : 1;
    g_autofree char *left = g_utf8_casefold(g_file_info_get_display_name(a), -1);
    g_autofree char *right = g_utf8_casefold(g_file_info_get_display_name(b), -1);
    return g_utf8_collate(left, right);
}

static GListModel *sorted_entries(GtkDirectoryList *directory)
{
    GtkFilterListModel *filtered = gtk_filter_list_model_new(G_LIST_MODEL(directory),
        GTK_FILTER(gtk_custom_filter_new(is_library_item, NULL, NULL)));
    return G_LIST_MODEL(gtk_sort_list_model_new(G_LIST_MODEL(filtered),
        GTK_SORTER(gtk_custom_sorter_new(compare_scripts, NULL, NULL))));
}

static GListModel *folder_children(gpointer item, gpointer user_data)
{
    (void)user_data;
    GFileInfo *info = G_FILE_INFO(item);
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) return NULL;
    GFile *folder = G_FILE(g_file_info_get_attribute_object(info, "standard::file"));
    return sorted_entries(gtk_directory_list_new(file_attributes, folder));
}

static void copy_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)user_data;
    g_autoptr(GError) error = NULL;
    if (!g_file_copy_finish(G_FILE(source), result, &error) && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning("copy failed: %s", error->message);
}

typedef struct {
    GFile *file;
    GtkWindow *parent;
    gboolean trash;
} Removal;

static void removal_free(Removal *removal)
{
    g_object_unref(removal->file);
    g_object_unref(removal->parent);
    g_free(removal);
}

static void removal_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Removal *removal = user_data;
    g_autoptr(GError) error = NULL;
    gboolean ok = removal->trash ? g_file_trash_finish(G_FILE(source), result, &error)
                                 : g_file_delete_finish(G_FILE(source), result, &error);
    if (!ok && gtk_widget_get_visible(GTK_WIDGET(removal->parent))) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("%s",
            removal->trash ? "Could not move folder to Trash" : "Could not delete script");
        gtk_alert_dialog_set_detail(alert, error->message);
        gtk_alert_dialog_show(alert, removal->parent);
    }
    removal_free(removal);
}

static void removal_confirmed(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Removal *removal = user_data;
    if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL) != 1)
        removal_free(removal);
    else if (removal->trash)
        g_file_trash_async(removal->file, G_PRIORITY_DEFAULT, NULL, removal_finished, removal);
    else
        g_file_delete_async(removal->file, G_PRIORITY_DEFAULT, NULL, removal_finished, removal);
}

static void ask_remove(ScriptRow *row)
{
    Removal *removal = g_new0(Removal, 1);
    removal->file = g_object_ref(row->file);
    removal->parent = g_object_ref(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(row))));
    removal->trash = row->expander != NULL;
    g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new(
        removal->trash ? "Move %s to Trash?" : "Delete %s?", gtk_label_get_text(row->label));
    gtk_alert_dialog_set_detail(alert, removal->trash ? "The folder and its contents will be moved to Trash."
                                                      : "This will remove the file from disk.");
    const char *buttons[] = {"Cancel", removal->trash ? "Move to Trash" : "Delete", NULL};
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 0);
    gtk_alert_dialog_set_default_button(alert, 0);
    gtk_alert_dialog_choose(alert, removal->parent, NULL, removal_confirmed, removal);
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
                              NULL, NULL, NULL, copy_finished, NULL);
            return;
        }
    }
}

static GtkWidget *create_row(gpointer item, gpointer user_data)
{
    Scripts *state = user_data;
    GtkTreeListRow *node = GTK_TREE_LIST_ROW(item);
    g_autoptr(GFileInfo) info = gtk_tree_list_row_get_item(node);
    ScriptRow *row = g_object_new(script_row_get_type(), NULL);
    row->scripts = state;
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        row->expander = GTK_TREE_EXPANDER(gtk_tree_expander_new());
        gtk_tree_expander_set_list_row(row->expander, node);
        gtk_tree_expander_set_child(row->expander, GTK_WIDGET(row->label));
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), GTK_WIDGET(row->expander));
        gtk_widget_add_css_class(GTK_WIDGET(row), "script-directory");
    } else {
        guint depth = gtk_tree_list_row_get_depth(node);
        gtk_widget_set_margin_start(GTK_WIDGET(row->label), 14 + 12 * depth);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), GTK_WIDGET(row->label));
    }
    row->edit_name = g_strdup(g_file_info_get_edit_name(info));
    gtk_label_set_text(row->label, g_file_info_get_display_name(info));
    GFile *file = G_FILE(g_file_info_get_attribute_object(info, "standard::file"));
    row->file = g_object_ref(file);
    if (state->active_file && g_file_equal(file, state->active_file))
        gtk_widget_add_css_class(GTK_WIDGET(row), "current-script");
    g_autofree char *path = g_file_get_parse_name(file);
    gtk_widget_set_tooltip_text(GTK_WIDGET(row), path);
    return GTK_WIDGET(row);
}

static void row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    (void)list;
    ScriptHandlers *handler = user_data;
    ScriptRow *script = (ScriptRow *)row;
    if (script->expander) {
        GtkTreeListRow *node = gtk_tree_expander_get_list_row(script->expander);
        gtk_tree_list_row_set_expanded(node, !gtk_tree_list_row_get_expanded(node));
    } else if (!script->editing)
        handler->open_script(script->file, gtk_label_get_text(script->label), handler->user_data);
}

static GdkContentProvider *prepare_drag(GtkDragSource *source, double x, double y, Scripts *state)
{
    (void)source;
    GtkWidget *hit = gtk_widget_pick(GTK_WIDGET(state->list), x, y, GTK_PICK_DEFAULT);
    if (state->stopped || !hit || gtk_widget_get_ancestor(hit, GTK_TYPE_POPOVER)) return NULL;
    ScriptRow *row = (ScriptRow *)gtk_widget_get_ancestor(hit, script_row_get_type());
    if (!row || row->editing) return NULL;
    g_set_object(&state->drag_row, GTK_LIST_BOX_ROW(row));
    return gdk_content_provider_new_typed(script_row_get_type(), row);
}

static void drag_began(GtkDragSource *source, GdkDrag *drag, Scripts *state)
{
    (void)source;
    ScriptRow *row = (ScriptRow *)state->drag_row;
    GtkWidget *label = gtk_label_new(gtk_label_get_text(row->label));
    gtk_label_set_max_width_chars(GTK_LABEL(label), 32);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_add_css_class(label, "script-drag-label");
    GtkWidget *icon = gtk_drag_icon_get_for_drag(drag);
    gtk_widget_add_css_class(icon, "relapse-ui");
    gtk_drag_icon_set_child(GTK_DRAG_ICON(icon), label);
    gtk_widget_add_css_class(GTK_WIDGET(row), "dragging");
}

static void highlight_folder(Scripts *state, GtkWidget *widget)
{
    if (state->drop_highlight == widget) return;
    if (state->drop_highlight) gtk_widget_remove_css_class(state->drop_highlight, "drop-folder");
    g_set_object(&state->drop_highlight, widget);
    if (widget) gtk_widget_add_css_class(widget, "drop-folder");
}

static void clear_drop(Scripts *state)
{
    g_clear_handle_id(&state->scroll_timer, g_source_remove);
    highlight_folder(state, NULL);
}

static void drag_ended(GtkDragSource *source, GdkDrag *drag, gboolean delete_data, GtkWidget *sidebar)
{
    (void)source;
    (void)drag;
    (void)delete_data;
    Scripts *state = g_object_get_data(G_OBJECT(sidebar), "scripts-state");
    if (state->drag_row) gtk_widget_remove_css_class(GTK_WIDGET(state->drag_row), "dragging");
    g_clear_object(&state->drag_row);
    clear_drop(state);
}

/* Empty list space and the heading both lead back to the library root. */
static GFile *folder_at(Scripts *state, double x, double y, GtkWidget **highlight)
{
    ScriptRow *source = (ScriptRow *)state->drag_row;
    if (!source || state->stopped || gtk_widget_get_parent(GTK_WIDGET(source)) != GTK_WIDGET(state->list))
        return NULL;
    GFile *root = gtk_directory_list_get_file(state->directory);
    if (!root || !g_file_has_prefix(source->file, root)) return NULL;
    GtkWidget *hit = gtk_widget_pick(state->sidebar, x, y, GTK_PICK_DEFAULT);
    if (!hit || gtk_widget_get_ancestor(hit, GTK_TYPE_SCROLLBAR) || gtk_widget_get_ancestor(hit, GTK_TYPE_POPOVER))
        return NULL;
    ScriptRow *row = (ScriptRow *)gtk_widget_get_ancestor(hit, script_row_get_type());
    if (row && (!row->expander || row->editing)) return NULL;
    GFile *folder = row ? row->file : root;
    if (g_file_has_parent(source->file, folder) || g_file_equal(source->file, folder) ||
        g_file_has_prefix(folder, source->file))
        return NULL;
    *highlight = row ? GTK_WIDGET(row) : gtk_widget_get_parent(state->title);
    return folder;
}

static GdkDragAction update_drop(Scripts *state, double x, double y)
{
    GtkWidget *highlight = NULL;
    GFile *folder = folder_at(state, x, y, &highlight);
    highlight_folder(state, highlight);
    return folder ? GDK_ACTION_MOVE : 0;
}

static gboolean scroll_drag(gpointer data)
{
    GtkDropTarget *target = GTK_DROP_TARGET(data);
    GtkWidget *sidebar = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    Scripts *state = g_object_get_data(G_OBJECT(sidebar), "scripts-state");
    GdkDrop *drop = gtk_drop_target_get_current_drop(target);
    double value = gtk_adjustment_get_value(state->scroll);
    if (drop) {
        GdkDragAction action = update_drop(state, state->drag_x, state->drag_y);
        gdk_drop_status(drop, action, action);
        gtk_adjustment_set_value(state->scroll, value + state->scroll_step);
        if (gtk_adjustment_get_value(state->scroll) != value) return G_SOURCE_CONTINUE;
    }
    state->scroll_timer = 0;
    return G_SOURCE_REMOVE;
}

static GdkDragAction drag_motion(GtkDropTarget *target, double x, double y, Scripts *state)
{
    const double edge = 28;
    double top = gtk_widget_get_height(gtk_widget_get_parent(state->title));
    double bottom = gtk_widget_get_height(state->sidebar);
    state->drag_x = x;
    state->drag_y = y;
    state->scroll_step = 0;
    if (state->drag_row && y >= top && !state->stopped) {
        if (y < top + edge) state->scroll_step = (y - top - edge) * 0.55;
        else if (y > bottom - edge) state->scroll_step = (y - bottom + edge) * 0.55;
    }
    if (state->scroll_step == 0)
        g_clear_handle_id(&state->scroll_timer, g_source_remove);
    else if (!state->scroll_timer)
        state->scroll_timer = g_timeout_add(32, scroll_drag, target);
    return update_drop(state, x, y);
}

static void drag_left(GtkDropTarget *target, Scripts *state)
{
    (void)target;
    clear_drop(state);
}

static gboolean drop_item(GtkDropTarget *target, const GValue *value, double x, double y, Scripts *state)
{
    (void)target;
    clear_drop(state);
    ScriptRow *source = g_value_get_object(value);
    if (source != (ScriptRow *)state->drag_row) return FALSE;
    GtkWidget *highlight = NULL;
    GFile *folder = folder_at(state, x, y, &highlight);
    if (!folder) return FALSE;
    g_autofree char *name = g_file_get_basename(source->file);
    g_autoptr(GFile) destination = g_file_get_child(folder, name);
    g_autoptr(GError) error = NULL;
    if (!state->handlers.move_script(source->file, destination, &error, state->handlers.user_data)) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not move “%s”", gtk_label_get_text(source->label));
        gtk_alert_dialog_set_detail(alert, error->message);
        gtk_alert_dialog_show(alert, GTK_WINDOW(gtk_widget_get_root(state->sidebar)));
        return FALSE;
    }
    if (G_TYPE_CHECK_INSTANCE_TYPE(highlight, script_row_get_type())) {
        ScriptRow *row = (ScriptRow *)highlight;
        gtk_tree_list_row_set_expanded(gtk_tree_expander_get_list_row(row->expander), TRUE);
    }
    return TRUE;
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

void scripts_set_active_file(GtkWidget *sidebar, GFile *file)
{
    Scripts *state = g_object_get_data(G_OBJECT(sidebar), "scripts-state");
    if (state->active_file == file || (file && state->active_file && g_file_equal(file, state->active_file)))
        return;
    g_set_object(&state->active_file, file);
    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(state->list)); child;
         child = gtk_widget_get_next_sibling(child)) {
        if (!G_TYPE_CHECK_INSTANCE_TYPE(child, script_row_get_type())) continue;
        ScriptRow *row = (ScriptRow *)child;
        if (file && g_file_equal(row->file, file))
            gtk_widget_add_css_class(child, "current-script");
        else
            gtk_widget_remove_css_class(child, "current-script");
    }
}

void scripts_shutdown(GtkWidget *scripts)
{
    Scripts *state = g_object_get_data(G_OBJECT(scripts), "scripts-state");
    state->stopped = TRUE;
    clear_drop(state);
    if (state->picker) g_cancellable_cancel(state->picker);
}

static void free_scripts(gpointer user_data)
{
    Scripts *state = user_data;
    if (state->picker) g_cancellable_cancel(state->picker);
    g_clear_handle_id(&state->scroll_timer, g_source_remove);
    g_clear_object(&state->picker);
    g_clear_object(&state->active_file);
    g_clear_object(&state->drop_highlight);
    g_clear_object(&state->drag_row);
    g_free(state->scripts_path);
    g_free(state);
}

static void update_directory(Scripts *state)
{
    clear_drop(state);
    const char *path = state->scripts_path;
    g_autofree char *display_path = g_filename_display_name(path);
    g_autofree char *tooltip = g_strdup_printf("%s\nChoose folder…", display_path);
    gtk_widget_set_tooltip_text(state->title, tooltip);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->title),
        GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, display_path, -1);
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
    if (state->root_menu) gtk_popover_popdown(state->root_menu);
    g_free(state->scripts_path);
    state->scripts_path = g_strdup(path);
    update_directory(state);
}

static void folder_chosen(GFile *file, const GError *error, gpointer user_data)
{
    GWeakRef *owner = user_data;
    g_autoptr(GtkWidget) sidebar = g_weak_ref_get(owner);
    Scripts *state = sidebar ? g_object_get_data(G_OBJECT(sidebar), "scripts-state") : NULL;
    if (state && !state->stopped) {
        g_clear_object(&state->picker);
        gtk_widget_set_sensitive(state->title, TRUE);
        g_autoptr(GError) failure = error ? g_error_copy(error) : NULL;
        if (file) {
            g_autofree char *path = g_file_get_path(file);
            if (!path || !g_file_test(path, G_FILE_TEST_IS_DIR))
                g_set_error_literal(&failure, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Choose a local folder.");
            else if (!state->handlers.directory_changed ||
                     state->handlers.directory_changed(path, &failure, state->handlers.user_data))
                scripts_set_directory(sidebar, path);
        }
        if (failure && !g_error_matches(failure, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
            !g_error_matches(failure, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
            !g_error_matches(failure, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not open folder");
            gtk_alert_dialog_set_detail(alert, failure->message);
            gtk_alert_dialog_show(alert, GTK_WINDOW(gtk_widget_get_root(sidebar)));
        }
    }
    g_weak_ref_clear(owner);
    g_free(owner);
}

static void select_folder(GtkButton *button, gpointer user_data)
{
    GtkWidget *sidebar = user_data;
    Scripts *state = g_object_get_data(G_OBJECT(sidebar), "scripts-state");
    if (state->stopped || state->picker) return;
    state->picker = g_cancellable_new();
    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    GWeakRef *owner = g_new0(GWeakRef, 1);
    g_weak_ref_init(owner, sidebar);
    g_autoptr(GFile) initial = g_file_new_for_path(
        g_file_test(state->scripts_path, G_FILE_TEST_IS_DIR) ? state->scripts_path : g_get_home_dir());
    picker_select_folder(GTK_WINDOW(gtk_widget_get_root(sidebar)), "Open folder", "Open",
        initial, state->picker, folder_chosen, owner);
}

GtkWidget *scripts_new(const char *path, ScriptOpenFunc open_script,
                       ScriptMoveFunc move_script, ScriptDirectoryFunc directory_changed,
                       gpointer user_data)
{
    g_return_val_if_fail(path != NULL && open_script != NULL && move_script != NULL, NULL);
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(sidebar, "scripts-sidebar");
    gtk_widget_set_size_request(sidebar, 160, -1);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(header, "scripts-heading");
    GtkWidget *caption = gtk_label_new("Scripts");
    gtk_widget_set_hexpand(caption, TRUE);
    gtk_label_set_xalign(GTK_LABEL(caption), 0);
    gtk_box_append(GTK_BOX(header), caption);
    GtkWidget *title = gtk_button_new();
    GtkWidget *open_icon = gtk_image_new_from_resource("/org/relapse/RelapseGtk/icons/folder-open.svg");
    gtk_image_set_pixel_size(GTK_IMAGE(open_icon), 14);
    gtk_button_set_child(GTK_BUTTON(title), open_icon);
    gtk_accessible_update_property(GTK_ACCESSIBLE(title),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Choose script folder", -1);
    gtk_box_append(GTK_BOX(header), title);
    gtk_box_append(GTK_BOX(sidebar), header);

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

    Scripts *state = g_new0(Scripts, 1);
    state->sidebar = sidebar;
    state->scroll = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    state->handlers = (ScriptHandlers){open_script, move_script, directory_changed, user_data};
    state->list = GTK_LIST_BOX(list);
    g_signal_connect(list, "row-activated", G_CALLBACK(row_activated), &state->handlers);

    state->directory = gtk_directory_list_new(file_attributes, NULL);
    g_signal_connect_object(state->directory, "notify::loading", G_CALLBACK(update_placeholder), placeholder, 0);
    g_signal_connect_object(state->directory, "notify::error", G_CALLBACK(update_placeholder), placeholder, 0);
    g_autoptr(GtkTreeListModel) tree = gtk_tree_list_model_new(sorted_entries(state->directory),
        FALSE, FALSE, folder_children, NULL, NULL);
    gtk_list_box_bind_model(state->list, G_LIST_MODEL(tree), create_row, state, NULL);
    state->title = title;
    state->placeholder = GTK_LABEL(placeholder);
    state->scripts_path = g_strdup(path);
    g_object_set_data_full(G_OBJECT(sidebar), "scripts-state", state, free_scripts);
    GtkDragSource *drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drag), GTK_PHASE_CAPTURE);
    g_signal_connect(drag, "prepare", G_CALLBACK(prepare_drag), state);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_began), state);
    g_signal_connect_object(drag, "drag-end", G_CALLBACK(drag_ended), sidebar, 0);
    gtk_widget_add_controller(list, GTK_EVENT_CONTROLLER(drag));
    GtkDropTarget *drop = gtk_drop_target_new(script_row_get_type(), GDK_ACTION_MOVE);
    g_signal_connect(drop, "enter", G_CALLBACK(drag_motion), state);
    g_signal_connect(drop, "motion", G_CALLBACK(drag_motion), state);
    g_signal_connect(drop, "leave", G_CALLBACK(drag_left), state);
    g_signal_connect(drop, "drop", G_CALLBACK(drop_item), state);
    gtk_widget_add_controller(sidebar, GTK_EVENT_CONTROLLER(drop));
    GtkGesture *root_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(root_click), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(root_click), GTK_PHASE_CAPTURE);
    g_signal_connect(root_click, "pressed", G_CALLBACK(root_right_clicked), state);
    gtk_widget_add_controller(sidebar, GTK_EVENT_CONTROLLER(root_click));
    GtkEventController *header_keys = gtk_event_controller_key_new();
    g_signal_connect(header_keys, "key-pressed", G_CALLBACK(root_key_pressed), state);
    gtk_widget_add_controller(title, header_keys);
    update_directory(state);
    g_signal_connect(title, "clicked", G_CALLBACK(select_folder), sidebar);
    return sidebar;
}
