#include "configuration.h"
#include "picker.h"
#include "profiles/config.h"
#include "profiles/store.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

typedef enum { DIALOG_GLOBAL, DIALOG_PROFILE } DialogMode;
typedef enum {
    FIELD_WORKSPACE, FIELD_AUTOEXEC, FIELD_AUTOLAUNCH, FIELD_RAKNET, FIELD_PROGRAM, FIELD_DATA
} ConfigField;

typedef struct {
    GtkWidget *row;
    GtkButton *button;
    GtkLabel *label;
} PathControl;

typedef struct {
    GtkWindow *window;
    GtkScrolledWindow *scroll;
    DialogMode mode;
    char *profile;
    ProfileConfig *config;
    ProfileConfig *global;
    char *data_dir;
    char *original_data_dir;
    GtkLabel *data_notice;
    PathControl workspace;
    PathControl autoexec;
    PathControl program;
    PathControl data;
    GtkDropDown *default_profile;
    GtkCheckButton *overrides[2];
    GtkCheckButton *autolaunch;
    GtkCheckButton *raknet;
    GtkDropDown *autolaunch_choice;
    GtkDropDown *raknet_choice;
    GtkEntry *arguments;
    GtkWidget *arguments_row;
    GtkLabel *error;
    GCancellable *picker;
    ConfigField picker_field;
    gboolean arguments_dirty;
    gboolean updating;
    gboolean stopped;
} ConfigDialog;

static void override_changed(GtkCheckButton *button, gpointer user_data);

static GtkLabel *label_new(const char *text, const char *style)
{
    GtkLabel *label = GTK_LABEL(gtk_label_new(text));
    gtk_label_set_xalign(label, 0);
    gtk_label_set_wrap(label, TRUE);
    if (style) gtk_widget_add_css_class(GTK_WIDGET(label), style);
    return label;
}

static GtkWidget *add_row(ConfigDialog *dialog, GtkWidget *group, const char *title,
                          GtkWidget *control, ConfigField field)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row, "settings-row");
    GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_size_request(heading, dialog->mode == DIALOG_PROFILE ? 136 : 112, -1);
    gtk_widget_set_valign(heading, GTK_ALIGN_CENTER);
    if (dialog->mode == DIALOG_PROFILE && field <= FIELD_AUTOEXEC) {
        GtkCheckButton *override = GTK_CHECK_BUTTON(gtk_check_button_new());
        gtk_widget_add_css_class(GTK_WIDGET(override), "configuration-override");
        dialog->overrides[field] = override;
        g_autofree char *accessible = g_strdup_printf("Override %s", title);
        gtk_accessible_update_property(GTK_ACCESSIBLE(override), GTK_ACCESSIBLE_PROPERTY_LABEL,
            accessible, -1);
        gtk_widget_set_tooltip_text(GTK_WIDGET(override), accessible);
        g_object_set_data(G_OBJECT(override), "configuration-field", GUINT_TO_POINTER(field));
        g_signal_connect(override, "toggled", G_CALLBACK(override_changed), dialog);
        gtk_box_append(GTK_BOX(heading), GTK_WIDGET(override));
    }
    GtkWidget *label = GTK_WIDGET(label_new(title, "settings-label"));
    if (dialog->mode == DIALOG_PROFILE && field == FIELD_PROGRAM)
        gtk_widget_set_margin_start(label, 24);
    gtk_box_append(GTK_BOX(heading), label);
    gtk_box_append(GTK_BOX(row), heading);
    GtkWidget *slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(slot, TRUE);
    gtk_widget_set_valign(slot, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_widget_set_halign(control, GTK_IS_CHECK_BUTTON(control) ? GTK_ALIGN_START : GTK_ALIGN_FILL);
    gtk_box_append(GTK_BOX(slot), control);
    gtk_box_append(GTK_BOX(row), slot);
    gtk_box_append(GTK_BOX(group), row);
    return row;
}

static void set_message(GtkLabel *label, const char *message)
{
    gtk_label_set_text(label, message ? message : "");
    gtk_widget_set_visible(GTK_WIDGET(label), message && *message);
}

static void show_error(GtkWindow *parent, const char *title, const GError *error)
{
    if (!error || g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) ||
        g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return;
    g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(alert, error->message);
    gtk_alert_dialog_show(alert, parent);
}

static gboolean field_overridden(ConfigDialog *dialog, ConfigField field)
{
    switch (field) {
    case FIELD_WORKSPACE: return dialog->config->workspace_override;
    case FIELD_AUTOEXEC: return dialog->config->autoexec_override;
    case FIELD_AUTOLAUNCH: return dialog->config->autolaunch_override;
    case FIELD_RAKNET: return dialog->config->raknet_override;
    default: return FALSE;
    }
}

static void set_field_overridden(ConfigDialog *dialog, ConfigField field, gboolean overridden)
{
    switch (field) {
    case FIELD_WORKSPACE: dialog->config->workspace_override = overridden; break;
    case FIELD_AUTOEXEC: dialog->config->autoexec_override = overridden; break;
    case FIELD_AUTOLAUNCH: dialog->config->autolaunch_override = overridden; break;
    case FIELD_RAKNET: dialog->config->raknet_override = overridden; break;
    default: break;
    }
}

static void inherit_field(ConfigDialog *dialog, ConfigField field)
{
    set_field_overridden(dialog, field, FALSE);
    switch (field) {
    case FIELD_WORKSPACE:
        g_free(dialog->config->workspace);
        dialog->config->workspace = g_strdup(dialog->global->workspace);
        break;
    case FIELD_AUTOEXEC:
        g_free(dialog->config->autoexec);
        dialog->config->autoexec = g_strdup(dialog->global->autoexec);
        break;
    case FIELD_AUTOLAUNCH:
        dialog->arguments_dirty = FALSE;
        g_strfreev(dialog->config->autolaunch);
        dialog->config->autolaunch = g_strdupv(dialog->global->autolaunch);
        break;
    case FIELD_RAKNET:
        dialog->config->raknet_enabled = dialog->global->raknet_enabled;
        break;
    default: break;
    }
}

static void set_path(PathControl *control, const char *path)
{
    const char *home = g_get_home_dir();
    g_autofree char *short_path = path && g_str_has_prefix(path, home) &&
        (path[strlen(home)] == '/' || path[strlen(home)] == '\0')
        ? g_strconcat("~", path + strlen(home), NULL) : g_strdup(path);
    g_autofree char *name = short_path && *short_path
        ? g_filename_display_name(short_path) : g_strdup("Choose…");
    gtk_label_set_text(control->label, name);
    gtk_widget_set_tooltip_text(GTK_WIDGET(control->button), path && *path ? path : NULL);
}

static void update_controls(ConfigDialog *dialog)
{
    dialog->updating = TRUE;
    if (dialog->mode == DIALOG_PROFILE) {
        set_path(&dialog->data, dialog->data_dir);
        gtk_widget_set_sensitive(GTK_WIDGET(dialog->data.button), dialog->picker == NULL);
        gtk_widget_set_visible(GTK_WIDGET(dialog->data_notice),
            g_strcmp0(dialog->data_dir, dialog->original_data_dir) != 0);
    }
    for (guint i = 0; i < G_N_ELEMENTS(dialog->overrides); ++i)
        if (dialog->overrides[i])
            gtk_check_button_set_active(dialog->overrides[i], field_overridden(dialog, i));
    gboolean workspace_editable = dialog->mode != DIALOG_PROFILE || dialog->config->workspace_override;
    gboolean autoexec_editable = dialog->mode != DIALOG_PROFILE || dialog->config->autoexec_override;
    gboolean autolaunch_editable = dialog->mode != DIALOG_PROFILE || dialog->config->autolaunch_override;
    gboolean raknet_editable = dialog->mode != DIALOG_PROFILE || dialog->config->raknet_override;
    set_path(&dialog->workspace, dialog->config->workspace);
    set_path(&dialog->autoexec, dialog->config->autoexec);
    gtk_widget_set_sensitive(GTK_WIDGET(dialog->workspace.button), workspace_editable);
    gtk_widget_set_sensitive(GTK_WIDGET(dialog->autoexec.button), autoexec_editable);
    gboolean enabled = dialog->config->autolaunch != NULL;
    if (dialog->autolaunch_choice)
        gtk_drop_down_set_selected(dialog->autolaunch_choice,
            !dialog->config->autolaunch_override ? 0 : enabled ? 2 : 1);
    else {
        gtk_check_button_set_active(dialog->autolaunch, enabled);
        gtk_widget_set_sensitive(GTK_WIDGET(dialog->autolaunch), autolaunch_editable);
    }
    gtk_widget_set_visible(dialog->program.row, enabled);
    gtk_widget_set_visible(dialog->arguments_row, enabled);
    set_path(&dialog->program, enabled ? dialog->config->autolaunch[0] : NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(dialog->program.button), autolaunch_editable);
    gtk_widget_set_sensitive(GTK_WIDGET(dialog->arguments), autolaunch_editable);
    if (!dialog->arguments_dirty) {
        GString *arguments = g_string_new(NULL);
        if (enabled) {
            for (guint i = 1; dialog->config->autolaunch[i]; ++i) {
                const char *argument = dialog->config->autolaunch[i];
                g_autofree char *quoted = NULL;
                if (!*argument || strpbrk(argument, " \t\n\"'\\"))
                    quoted = g_shell_quote(argument);
                if (arguments->len) g_string_append_c(arguments, ' ');
                g_string_append(arguments, quoted ? quoted : argument);
            }
        }
        gtk_editable_set_text(GTK_EDITABLE(dialog->arguments), arguments->str);
        g_string_free(arguments, TRUE);
    }
    if (dialog->raknet_choice)
        gtk_drop_down_set_selected(dialog->raknet_choice,
            !dialog->config->raknet_override ? 0 : dialog->config->raknet_enabled ? 2 : 1);
    else {
        gtk_check_button_set_active(dialog->raknet, dialog->config->raknet_enabled);
        gtk_widget_set_sensitive(GTK_WIDGET(dialog->raknet), raknet_editable);
    }
    if (dialog->scroll) {
        GtkWidget *content = gtk_scrolled_window_get_child(dialog->scroll);
        int height = 0;
        gtk_widget_measure(content, GTK_ORIENTATION_VERTICAL, 450, NULL, &height, NULL, NULL);
        gtk_scrolled_window_set_min_content_height(dialog->scroll, MIN(height, 460));
    }
    dialog->updating = FALSE;
}

static void path_chosen(GFile *file, const GError *error, gpointer user_data)
{
    GWeakRef *owner = user_data;
    g_autoptr(GtkWindow) window = g_weak_ref_get(owner);
    ConfigDialog *dialog = window ? g_object_get_data(G_OBJECT(window), "configuration-dialog") : NULL;
    if (dialog && !dialog->stopped) {
        g_clear_object(&dialog->picker);
        g_autoptr(GError) failure = error ? g_error_copy(error) : NULL;
        if (file) {
            g_autofree char *path = g_file_get_path(file);
            gboolean executable = dialog->picker_field == FIELD_PROGRAM;
            if (!path || (executable
                    ? !g_file_test(path, G_FILE_TEST_IS_REGULAR) || g_access(path, X_OK) != 0
                    : !g_file_test(path, G_FILE_TEST_IS_DIR))) {
                g_set_error_literal(&failure, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    executable ? "Choose a local executable file." : "Choose a local folder.");
            } else switch (dialog->picker_field) {
            case FIELD_WORKSPACE:
                g_free(dialog->config->workspace);
                dialog->config->workspace = g_strdup(path);
                dialog->config->workspace_override = TRUE;
                break;
            case FIELD_AUTOEXEC:
                g_free(dialog->config->autoexec);
                dialog->config->autoexec = g_strdup(path);
                dialog->config->autoexec_override = TRUE;
                break;
            case FIELD_PROGRAM:
                if (!dialog->config->autolaunch) {
                    dialog->config->autolaunch = g_new0(char *, 2);
                    dialog->config->autolaunch[0] = g_strdup(path);
                } else {
                    g_free(dialog->config->autolaunch[0]);
                    dialog->config->autolaunch[0] = g_strdup(path);
                }
                dialog->config->autolaunch_override = TRUE;
                break;
            case FIELD_DATA:
                g_free(dialog->data_dir);
                dialog->data_dir = g_strdup(path);
                break;
            default: break;
            }
        }
        update_controls(dialog);
        show_error(dialog->window, "Could not select path", failure);
    }
    g_weak_ref_clear(owner);
    g_free(owner);
}

static void select_path(GtkButton *button, gpointer user_data)
{
    ConfigDialog *dialog = user_data;
    if (dialog->stopped || dialog->picker) return;
    ConfigField field = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "configuration-field"));
    const char *path = NULL;
    switch (field) {
    case FIELD_WORKSPACE: path = dialog->config->workspace; break;
    case FIELD_AUTOEXEC: path = dialog->config->autoexec; break;
    case FIELD_PROGRAM:
        path = dialog->config->autolaunch ? dialog->config->autolaunch[0] : NULL;
        break;
    case FIELD_DATA: path = dialog->data_dir; break;
    default: break;
    }
    g_autoptr(GFile) initial = NULL;
    if (path && *path && g_file_test(path, G_FILE_TEST_EXISTS)) {
        initial = g_file_new_for_path(path);
        if (field == FIELD_PROGRAM) {
            g_autoptr(GFile) parent = g_file_get_parent(initial);
            g_set_object(&initial, parent);
        }
    } else initial = g_file_new_for_path(g_get_home_dir());
    dialog->picker = g_cancellable_new();
    dialog->picker_field = field;
    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    GWeakRef *owner = g_new0(GWeakRef, 1);
    g_weak_ref_init(owner, dialog->window);
    const char *title = field == FIELD_PROGRAM ? "Select autolaunch executable" : "Select folder";
    if (field == FIELD_PROGRAM)
        picker_open_file(dialog->window, title, "Select", initial, dialog->picker, path_chosen, owner);
    else picker_select_folder(dialog->window, title, "Select", initial, dialog->picker, path_chosen, owner);
}

static void override_changed(GtkCheckButton *button, gpointer user_data)
{
    ConfigDialog *dialog = user_data;
    if (dialog->updating) return;
    ConfigField field = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "configuration-field"));
    if (gtk_check_button_get_active(button)) set_field_overridden(dialog, field, TRUE);
    else inherit_field(dialog, field);
    update_controls(dialog);
}

static PathControl path_control(ConfigDialog *dialog, GtkWidget *group, const char *title, ConfigField field)
{
    PathControl control = {0};
    control.button = GTK_BUTTON(gtk_button_new());
    gtk_widget_add_css_class(GTK_WIDGET(control.button), "configuration-path");
    control.label = GTK_LABEL(gtk_label_new("Select…"));
    gtk_label_set_xalign(control.label, 0);
    gtk_label_set_ellipsize(control.label, PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_width_chars(control.label, 1);
    gtk_widget_set_hexpand(GTK_WIDGET(control.label), TRUE);
    gtk_button_set_child(control.button, GTK_WIDGET(control.label));
    g_autofree char *accessible = g_strdup_printf("Select %s", title);
    gtk_accessible_update_property(GTK_ACCESSIBLE(control.button), GTK_ACCESSIBLE_PROPERTY_LABEL,
        accessible, -1);
    g_object_set_data(G_OBJECT(control.button), "configuration-field", GUINT_TO_POINTER(field));
    g_signal_connect(control.button, "clicked", G_CALLBACK(select_path), dialog);
    control.row = add_row(dialog, group, title, GTK_WIDGET(control.button), field);
    return control;
}

static void autolaunch_changed(GtkCheckButton *button, gpointer user_data)
{
    ConfigDialog *dialog = user_data;
    if (dialog->updating) return;
    gboolean enabled = gtk_check_button_get_active(button);
    if (enabled && !dialog->config->autolaunch) {
        dialog->config->autolaunch = g_new0(char *, 2);
        dialog->config->autolaunch[0] = g_strdup("");
    } else if (!enabled) g_clear_pointer(&dialog->config->autolaunch, g_strfreev);
    dialog->config->autolaunch_override = TRUE;
    update_controls(dialog);
}

static void raknet_changed(GtkCheckButton *button, gpointer user_data)
{
    ConfigDialog *dialog = user_data;
    if (dialog->updating) return;
    dialog->config->raknet_enabled = gtk_check_button_get_active(button);
    dialog->config->raknet_override = TRUE;
    update_controls(dialog);
}

static void autolaunch_choice_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)property;
    ConfigDialog *dialog = user_data;
    if (dialog->updating) return;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    if (selected == 0) inherit_field(dialog, FIELD_AUTOLAUNCH);
    else {
        gboolean enabled = selected == 2;
        if (enabled && !dialog->config->autolaunch) {
            dialog->config->autolaunch = g_new0(char *, 2);
            dialog->config->autolaunch[0] = g_strdup("");
        } else if (!enabled) g_clear_pointer(&dialog->config->autolaunch, g_strfreev);
        dialog->config->autolaunch_override = TRUE;
    }
    update_controls(dialog);
}

static void raknet_choice_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)property;
    ConfigDialog *dialog = user_data;
    if (dialog->updating) return;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    if (selected == 0) inherit_field(dialog, FIELD_RAKNET);
    else {
        dialog->config->raknet_override = TRUE;
        dialog->config->raknet_enabled = selected == 2;
    }
    update_controls(dialog);
}

static void arguments_changed(GtkEditable *editable, gpointer user_data)
{
    (void)editable;
    ConfigDialog *dialog = user_data;
    if (dialog->updating) return;
    dialog->arguments_dirty = TRUE;
    dialog->config->autolaunch_override = TRUE;
}

static GtkWidget *configuration_group(ConfigDialog *dialog)
{
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    if (dialog->mode == DIALOG_GLOBAL) {
        GtkWidget *profiles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(profiles, "settings-group");
        gtk_widget_add_css_class(profiles, "configuration-list");
        g_auto(GStrv) names = profiles_list();
        g_autoptr(GtkStringList) list = gtk_string_list_new(NULL);
        guint selected = 0;
        if (!g_strv_contains((const char *const *)names, dialog->config->default_profile))
            gtk_string_list_append(list, dialog->config->default_profile);
        for (guint i = 0; names[i]; ++i) {
            if (g_str_equal(names[i], dialog->config->default_profile)) selected = i;
            gtk_string_list_append(list, names[i]);
        }
        dialog->default_profile = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(g_steal_pointer(&list)), NULL));
        gtk_drop_down_set_selected(dialog->default_profile, selected);
        gtk_drop_down_set_enable_search(dialog->default_profile, FALSE);
        add_row(dialog, profiles, "Default profile", GTK_WIDGET(dialog->default_profile), FIELD_PROGRAM);
        gtk_box_append(GTK_BOX(column), profiles);
    }
    GtkWidget *group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(group, "settings-group");
    gtk_widget_add_css_class(group, "configuration-list");
    dialog->workspace = path_control(dialog, group, "Workspace", FIELD_WORKSPACE);
    dialog->autoexec = path_control(dialog, group, "Autoexec", FIELD_AUTOEXEC);
    if (dialog->mode == DIALOG_PROFILE) {
        const char *choices[] = {"Inherit", "Disabled", "Enabled", NULL};
        dialog->autolaunch_choice = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(choices));
        gtk_drop_down_set_enable_search(dialog->autolaunch_choice, FALSE);
        add_row(dialog, group, "Autolaunch", GTK_WIDGET(dialog->autolaunch_choice), FIELD_AUTOLAUNCH);
    } else {
        dialog->autolaunch = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Enabled"));
        gtk_accessible_update_property(GTK_ACCESSIBLE(dialog->autolaunch), GTK_ACCESSIBLE_PROPERTY_LABEL,
            "Enable autolaunch", -1);
        add_row(dialog, group, "Autolaunch", GTK_WIDGET(dialog->autolaunch), FIELD_AUTOLAUNCH);
    }
    dialog->program = path_control(dialog, group, "Executable", FIELD_PROGRAM);
    dialog->arguments = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(dialog->arguments, "Optional");
    gtk_editable_set_width_chars(GTK_EDITABLE(dialog->arguments), 1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(dialog->arguments), GTK_ACCESSIBLE_PROPERTY_LABEL,
        "Autolaunch arguments", -1);
    gtk_widget_set_tooltip_text(GTK_WIDGET(dialog->arguments), "Use shell-style quoting for arguments containing spaces.");
    dialog->arguments_row = add_row(dialog, group, "Arguments", GTK_WIDGET(dialog->arguments), FIELD_PROGRAM);
    if (dialog->mode == DIALOG_PROFILE) {
        const char *choices[] = {"Inherit", "Disabled", "Enabled", NULL};
        dialog->raknet_choice = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(choices));
        gtk_drop_down_set_enable_search(dialog->raknet_choice, FALSE);
        add_row(dialog, group, "RakNet", GTK_WIDGET(dialog->raknet_choice), FIELD_RAKNET);
    } else {
        dialog->raknet = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Enabled"));
        gtk_accessible_update_property(GTK_ACCESSIBLE(dialog->raknet), GTK_ACCESSIBLE_PROPERTY_LABEL,
            "Enable RakNet", -1);
        add_row(dialog, group, "RakNet", GTK_WIDGET(dialog->raknet), FIELD_RAKNET);
    }
    gtk_box_append(GTK_BOX(column), group);
    if (dialog->autolaunch_choice) {
        g_signal_connect(dialog->autolaunch_choice, "notify::selected", G_CALLBACK(autolaunch_choice_changed), dialog);
        g_signal_connect(dialog->raknet_choice, "notify::selected", G_CALLBACK(raknet_choice_changed), dialog);
    } else {
        g_signal_connect(dialog->autolaunch, "toggled", G_CALLBACK(autolaunch_changed), dialog);
        g_signal_connect(dialog->raknet, "toggled", G_CALLBACK(raknet_changed), dialog);
    }
    g_signal_connect(dialog->arguments, "changed", G_CALLBACK(arguments_changed), dialog);
    return column;
}

static gboolean close_dialog(GtkWindow *window, gpointer user_data)
{
    (void)window;
    ConfigDialog *dialog = user_data;
    dialog->stopped = TRUE;
    if (dialog->picker) g_cancellable_cancel(dialog->picker);
    return FALSE;
}

static void free_dialog(gpointer user_data)
{
    ConfigDialog *dialog = user_data;
    if (dialog->picker) g_cancellable_cancel(dialog->picker);
    g_clear_object(&dialog->picker);
    profile_config_free(dialog->config);
    profile_config_free(dialog->global);
    g_free(dialog->data_dir);
    g_free(dialog->original_data_dir);
    g_free(dialog->profile);
    g_free(dialog);
}

static char **dialog_command(ConfigDialog *dialog, GError **error)
{
    if (!dialog->config->autolaunch) return NULL;
    gint count = 0;
    g_auto(GStrv) arguments = NULL;
    g_autofree char *text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(dialog->arguments)));
    if (*g_strstrip(text) && !g_shell_parse_argv(text, &count, &arguments, error)) return NULL;
    g_autoptr(GPtrArray) command = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(command, g_strdup(dialog->config->autolaunch[0]));
    for (gint i = 0; i < count; ++i) g_ptr_array_add(command, g_strdup(arguments[i]));
    g_ptr_array_add(command, NULL);
    return (char **)g_ptr_array_free(g_steal_pointer(&command), FALSE);
}

static void save_dialog(GtkButton *button, gpointer user_data)
{
    (void)button;
    ConfigDialog *dialog = user_data;
    if (dialog->stopped || dialog->picker) return;
    g_autoptr(GError) error = NULL;
    gboolean saved = FALSE;
    if (dialog->mode == DIALOG_GLOBAL) {
        GtkStringObject *item = GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(dialog->default_profile));
        if (item) {
            g_free(dialog->config->default_profile);
            dialog->config->default_profile = g_strdup(gtk_string_object_get_string(item));
        }
    }
    char **command = dialog_command(dialog, &error);
    if (!error) {
        g_strfreev(dialog->config->autolaunch);
        dialog->config->autolaunch = command;
        saved = dialog->mode == DIALOG_PROFILE
            ? profile_settings_save(dialog->profile, dialog->config, dialog->data_dir, &error)
            : profile_config_save(NULL, dialog->config, &error);
    }
    if (saved) gtk_window_close(dialog->window);
    else set_message(dialog->error, error ? error->message : "Could not save settings.");
}

GtkWindow *configuration_dialog_new(GtkWindow *parent, const char *profile, GtkWidget *actions)
{
    if (profile && !profile_name_valid(profile)) return NULL;
    DialogMode mode = profile ? DIALOG_PROFILE : DIALOG_GLOBAL;
    ConfigDialog *dialog = g_new0(ConfigDialog, 1);
    dialog->mode = mode;
    dialog->profile = g_strdup(profile);
    g_autoptr(GError) error = NULL;
    dialog->config = profile_config_load(profile, &error);
    if (profile && dialog->config) {
        dialog->global = profile_config_load(NULL, &error);
        if (dialog->global) dialog->data_dir = profile_data_directory(profile, &error);
        dialog->original_data_dir = g_strdup(dialog->data_dir);
    }
    if (error) {
        show_error(parent, "Could not load Relapse settings", error);
        free_dialog(dialog);
        return NULL;
    }
    g_autofree char *title = mode == DIALOG_GLOBAL ? g_strdup("Relapse settings")
        : g_strdup_printf("Settings · %s", profile);
    dialog->window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(dialog->window, title);
    gtk_window_set_transient_for(dialog->window, parent);
    gtk_window_set_modal(dialog->window, TRUE);
    gtk_window_set_destroy_with_parent(dialog->window, TRUE);
    gtk_window_set_resizable(dialog->window, TRUE);
    gtk_window_set_default_size(dialog->window, 480, -1);
    gtk_widget_add_css_class(GTK_WIDGET(dialog->window), "relapse-ui");
    gtk_widget_add_css_class(GTK_WIDGET(dialog->window), "configuration-dialog");
    g_object_set_data_full(G_OBJECT(dialog->window), "configuration-dialog", dialog, free_dialog);
    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *heading = gtk_label_new(title);
    gtk_label_set_ellipsize(GTK_LABEL(heading), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(heading, "title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), heading);
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), FALSE);
    if (actions) gtk_header_bar_pack_end(GTK_HEADER_BAR(header), actions);
    gtk_window_set_titlebar(dialog->window, header);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(box, "account-dialog");
    GtkWidget *content;
    if (profile) {
        content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        GtkWidget *storage = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(storage, "settings-group");
        gtk_widget_add_css_class(storage, "configuration-list");
        dialog->data = path_control(dialog, storage, "Data directory", FIELD_DATA);
        gtk_box_append(GTK_BOX(content), storage);
        dialog->data_notice = label_new("Existing files stay in their current folder.", "settings-description");
        gtk_box_append(GTK_BOX(content), GTK_WIDGET(dialog->data_notice));
        gtk_box_append(GTK_BOX(content), GTK_WIDGET(label_new("Overrides", "configuration-section")));
        gtk_box_append(GTK_BOX(content), configuration_group(dialog));
    } else content = configuration_group(dialog);
    GtkWidget *scroll = gtk_scrolled_window_new();
    dialog->scroll = GTK_SCROLLED_WINDOW(scroll);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 460);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);
    gtk_box_append(GTK_BOX(box), scroll);
    update_controls(dialog);
    dialog->error = label_new(NULL, "script-rename-error");
    gtk_label_set_max_width_chars(dialog->error, 48);
    gtk_widget_set_visible(GTK_WIDGET(dialog->error), FALSE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(dialog->error));
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(buttons, "configuration-actions");
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *save = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(save, "configuration-save");
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), save);
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_window_set_default_widget(dialog->window, save);
    gtk_window_set_child(dialog->window, box);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_close), dialog->window);
    g_signal_connect(save, "clicked", G_CALLBACK(save_dialog), dialog);
    g_signal_connect(dialog->window, "close-request", G_CALLBACK(close_dialog), dialog);
    gtk_window_present(dialog->window);
    return dialog->window;
}
