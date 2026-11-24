#include "settings.h"
#include "configuration.h"
#include "picker.h"

#include <glib/gstdio.h>
#include <unistd.h>
#include <string.h>

typedef struct {
    GtkSpinButton *font_size;
    GtkSwitch *word_wrap;
    GtkToggleButton *indent[3];
    GtkSwitch *restore_tabs;
    GtkSwitch *output;
    gboolean updating;
    SettingsChangedFunc changed;
    RestoreTabsChangedFunc restore_changed;
    OutputChangedFunc output_changed;
    GtkWidget *launcher_button;
    GtkLabel *launcher_path;
    LauncherChangedFunc launcher_changed;
    GtkWindow *configuration;
    GtkWidget *scripts_button;
    GtkLabel *scripts_path_label;
    ScriptsChangedFunc scripts_changed;
    char *scripts_path;
    GCancellable *picker;
    gboolean stopped;
    gpointer user_data;
} Settings;

static const int indent_widths[] = {2, 4, 8};

static void set_path_label(GtkLabel *label, const char *path)
{
    if (!path || !*path) {
        gtk_label_set_text(label, "Choose…");
        gtk_widget_set_tooltip_text(GTK_WIDGET(label), NULL);
        return;
    }
    const char *home = g_get_home_dir();
    const char *relative = g_str_has_prefix(path, home) ? path + strlen(home) : NULL;
    g_autofree char *shortened = relative && (*relative == '/' || !*relative)
        ? g_strconcat("~", relative, NULL) : NULL;
    g_autofree char *display = g_filename_display_name(shortened ? shortened : path);
    g_autofree char *full = g_filename_display_name(path);
    gtk_label_set_text(label, display);
    gtk_widget_set_tooltip_text(GTK_WIDGET(label), full);
}

void settings_set_launcher(GtkWidget *page, const char *path)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    set_path_label(state->launcher_path, path);
}

void settings_set_scripts_path(GtkWidget *page, const char *path)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    g_free(state->scripts_path);
    state->scripts_path = g_strdup(path);
    set_path_label(state->scripts_path_label, path);
}

static void picker_error(GtkWidget *page, const char *title, const GError *error)
{
    if (!error || g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) ||
        g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(alert, error->message);
    gtk_alert_dialog_show(alert, GTK_WINDOW(gtk_widget_get_root(page)));
}

static void picker_finished(Settings *state)
{
    g_clear_object(&state->picker);
    gtk_widget_set_sensitive(state->launcher_button, TRUE);
    gtk_widget_set_sensitive(state->scripts_button, TRUE);
}

static void launcher_chosen(GFile *file, const GError *error, gpointer user_data)
{
    GWeakRef *owner = user_data;
    g_autoptr(GtkWidget) page = g_weak_ref_get(owner);
    Settings *state = page ? g_object_get_data(G_OBJECT(page), "settings") : NULL;
    if (state && !state->stopped) {
        picker_finished(state);
        g_autoptr(GError) failure = error ? g_error_copy(error) : NULL;
        if (file) {
            g_autofree char *path = g_file_get_path(file);
            if (!path || !g_file_test(path, G_FILE_TEST_IS_REGULAR) || g_access(path, X_OK) != 0)
                g_set_error_literal(&failure, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Choose a local executable file.");
            else if (!state->launcher_changed || state->launcher_changed(path, &failure, state->user_data))
                settings_set_launcher(page, path);
        }
        picker_error(page, "Could not select launcher", failure);
    }
    g_weak_ref_clear(owner);
    g_free(owner);
}

static void scripts_chosen(GFile *file, const GError *error, gpointer user_data)
{
    GWeakRef *owner = user_data;
    g_autoptr(GtkWidget) page = g_weak_ref_get(owner);
    Settings *state = page ? g_object_get_data(G_OBJECT(page), "settings") : NULL;
    if (state && !state->stopped) {
        picker_finished(state);
        g_autoptr(GError) failure = error ? g_error_copy(error) : NULL;
        if (file) {
            g_autofree char *path = g_file_get_path(file);
            if (!path || !g_file_test(path, G_FILE_TEST_IS_DIR))
                g_set_error_literal(&failure, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Choose a local folder.");
            else if (!state->scripts_changed || state->scripts_changed(path, &failure, state->user_data))
                settings_set_scripts_path(page, path);
        }
        picker_error(page, "Could not select script folder", failure);
    }
    g_weak_ref_clear(owner);
    g_free(owner);
}

static GWeakRef *picker_owner(GtkWidget *page, GtkWidget *button)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->picker = g_cancellable_new();
    gtk_widget_set_sensitive(button, FALSE);
    GWeakRef *owner = g_new0(GWeakRef, 1);
    g_weak_ref_init(owner, page);
    return owner;
}

static void select_launcher(GtkButton *button, gpointer user_data)
{
    GtkWidget *page = user_data;
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    if (state->stopped || state->picker) return;
    GWeakRef *owner = picker_owner(page, GTK_WIDGET(button));
    picker_open_file(GTK_WINDOW(gtk_widget_get_root(page)), "Select launcher", "Select",
        NULL, state->picker, launcher_chosen, owner);
}

static void select_scripts(GtkButton *button, gpointer user_data)
{
    GtkWidget *page = user_data;
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    if (state->stopped || state->picker) return;
    GWeakRef *owner = picker_owner(page, GTK_WIDGET(button));
    g_autoptr(GFile) initial = state->scripts_path && g_file_test(state->scripts_path, G_FILE_TEST_IS_DIR)
        ? g_file_new_for_path(state->scripts_path) : g_file_new_for_path(g_get_home_dir());
    picker_select_folder(GTK_WINDOW(gtk_widget_get_root(page)), "Select script folder", "Select",
        initial, state->picker, scripts_chosen, owner);
}

static void configuration_closed(GtkWindow *window, gpointer user_data)
{
    (void)window;
    Settings *state = user_data;
    state->configuration = NULL;
}

static void open_configuration(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkWidget *page = user_data;
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    if (state->stopped) return;
    if (state->configuration) {
        gtk_window_present(state->configuration);
        return;
    }
    state->configuration = configuration_dialog_new(
        GTK_WINDOW(gtk_widget_get_root(page)), NULL, NULL);
    if (state->configuration)
        g_signal_connect(state->configuration, "destroy", G_CALLBACK(configuration_closed), state);
}

void settings_shutdown(GtkWidget *page)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->stopped = TRUE;
    if (state->picker) g_cancellable_cancel(state->picker);
    if (state->configuration) gtk_window_destroy(state->configuration);
}

static void free_settings(gpointer user_data)
{
    Settings *state = user_data;
    if (state->picker) g_cancellable_cancel(state->picker);
    g_clear_object(&state->picker);
    g_free(state->scripts_path);
    g_free(state);
}

void settings_set_output(GtkWidget *page, gboolean enabled)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->updating = TRUE;
    gtk_switch_set_active(state->output, enabled);
    state->updating = FALSE;
}

static void output_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)property;
    Settings *state = user_data;
    if (!state->updating && state->output_changed)
        state->output_changed(gtk_switch_get_active(GTK_SWITCH(object)), state->user_data);
}

void settings_set_restore_tabs(GtkWidget *page, gboolean enabled)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->updating = TRUE;
    gtk_switch_set_active(state->restore_tabs, enabled);
    state->updating = FALSE;
}

static void restore_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)property;
    Settings *state = user_data;
    if (!state->updating && state->restore_changed)
        state->restore_changed(gtk_switch_get_active(GTK_SWITCH(object)), state->user_data);
}

void settings_set_editor(GtkWidget *page, const EditorSettings *settings)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->updating = TRUE;
    gtk_spin_button_set_value(state->font_size, settings->font_size);
    gtk_switch_set_active(state->word_wrap, settings->word_wrap);
    for (guint i = 0; i < G_N_ELEMENTS(indent_widths); ++i)
        if (settings->indent_width == indent_widths[i])
            gtk_toggle_button_set_active(state->indent[i], TRUE);
    state->updating = FALSE;
}

static void values_changed(GtkWidget *widget, gpointer user_data)
{
    Settings *state = user_data;
    if (state->updating || !state->changed ||
        (GTK_IS_TOGGLE_BUTTON(widget) && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))))
        return;
    EditorSettings settings = {
        .font_size = gtk_spin_button_get_value_as_int(state->font_size),
        .word_wrap = gtk_switch_get_active(state->word_wrap),
        .indent_width = 4
    };
    for (guint i = 0; i < G_N_ELEMENTS(indent_widths); ++i)
        if (gtk_toggle_button_get_active(state->indent[i]))
            settings.indent_width = indent_widths[i];
    state->changed(&settings, state->user_data);
}

static void wrap_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)property;
    values_changed(GTK_WIDGET(object), user_data);
}

static GtkWidget *label_new(const char *text, const char *style)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    if (style)
        gtk_widget_add_css_class(label, style);
    return label;
}

static void section_start(GtkGrid *grid, int *row, const char *title)
{
    if (*row > 0) {
        GtkWidget *line = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_grid_attach(grid, line, 0, (*row)++, 3, 1);
    }
    GtkWidget *heading = label_new(title, "preferences-section");
    gtk_grid_attach(grid, heading, 0, *row, 1, 1);
}

static void add_option(GtkGrid *grid, int *row, const char *name, GtkWidget *control)
{
    GtkWidget *label = label_new(name, NULL);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_grid_attach(grid, label, 1, *row, 1, 1);
    gtk_widget_set_halign(control, GTK_ALIGN_START);
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    gtk_grid_attach(grid, control, 2, (*row)++, 1, 1);
}

static GtkWidget *path_button(GtkLabel **value, const char *action)
{
    GtkWidget *button = gtk_button_new();
    gtk_widget_add_css_class(button, "preference-path");
    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    *value = GTK_LABEL(label_new("Choose…", NULL));
    gtk_label_set_ellipsize(*value, PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(*value, 52);
    gtk_box_append(GTK_BOX(line), GTK_WIDGET(*value));
    GtkWidget *icon = gtk_image_new_from_resource("/org/relapse/RelapseGtk/icons/folder-open.svg");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 14);
    gtk_box_append(GTK_BOX(line), icon);
    gtk_button_set_child(GTK_BUTTON(button), line);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, action, -1);
    gtk_accessible_update_relation(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_RELATION_DESCRIBED_BY,
                                   *value, NULL, -1);
    gtk_widget_set_tooltip_text(button, action);
    return button;
}

GtkWidget *settings_new(const EditorSettings *settings, SettingsChangedFunc changed,
                       RestoreTabsChangedFunc restore_callback, OutputChangedFunc output_callback,
                       LauncherChangedFunc launcher_callback, ScriptsChangedFunc scripts_callback,
                       gpointer user_data)
{
    Settings *state = g_new0(Settings, 1);
    state->changed = changed;
    state->restore_changed = restore_callback;
    state->output_changed = output_callback;
    state->launcher_changed = launcher_callback;
    state->scripts_changed = scripts_callback;
    state->user_data = user_data;
    GtkWidget *page = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(page), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    g_object_set_data_full(G_OBJECT(page), "settings", state, free_settings);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_add_css_class(content, "settings-page");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(page), content);
    gtk_box_append(GTK_BOX(content), label_new("Settings", "settings-title"));

    GtkGrid *grid = GTK_GRID(gtk_grid_new());
    gtk_grid_set_column_spacing(grid, 26);
    gtk_grid_set_row_spacing(grid, 6);
    gtk_widget_add_css_class(GTK_WIDGET(grid), "settings-group");
    gtk_widget_add_css_class(GTK_WIDGET(grid), "preferences-grid");
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(grid));
    int row = 0;
    section_start(grid, &row, "Editor");

    GtkWidget *font = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->font_size = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(
        EDITOR_FONT_SIZE_MIN, EDITOR_FONT_SIZE_MAX, 1));
    gtk_widget_add_css_class(GTK_WIDGET(state->font_size), "font-size");
    gtk_spin_button_set_numeric(state->font_size, TRUE);
    gtk_spin_button_set_update_policy(state->font_size, GTK_UPDATE_IF_VALID);
    gtk_editable_set_width_chars(GTK_EDITABLE(state->font_size), 2);
    gtk_editable_set_alignment(GTK_EDITABLE(state->font_size), 0.5);
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->font_size),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Editor font size in pixels", -1);
    gtk_box_append(GTK_BOX(font), GTK_WIDGET(state->font_size));
    gtk_box_append(GTK_BOX(font), label_new("px", "settings-description"));
    add_option(grid, &row, "Font size", font);

    state->word_wrap = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->word_wrap), "word-wrap");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->word_wrap),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Word wrap", -1);

    GtkWidget *indent = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(indent, "indent-options");
    for (guint i = 0; i < G_N_ELEMENTS(indent_widths); ++i) {
        g_autofree char *text = g_strdup_printf("%d", indent_widths[i]);
        state->indent[i] = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(text));
        if (i)
            gtk_toggle_button_set_group(state->indent[i], state->indent[0]);
        g_autofree char *label = g_strdup_printf("Indent with %d spaces", indent_widths[i]);
        gtk_accessible_update_property(GTK_ACCESSIBLE(state->indent[i]),
            GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
        gtk_box_append(GTK_BOX(indent), GTK_WIDGET(state->indent[i]));
        g_signal_connect(state->indent[i], "toggled", G_CALLBACK(values_changed), state);
    }
    GtkWidget *indent_control = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(indent_control), indent);
    gtk_box_append(GTK_BOX(indent_control), label_new("spaces", "settings-description"));
    add_option(grid, &row, "Indent width", indent_control);
    add_option(grid, &row, "Wrap long lines", GTK_WIDGET(state->word_wrap));
    settings_set_editor(page, settings);
    g_signal_connect(state->font_size, "value-changed", G_CALLBACK(values_changed), state);
    g_signal_connect(state->word_wrap, "notify::active", G_CALLBACK(wrap_changed), state);

    section_start(grid, &row, "Workspace");
    state->scripts_button = path_button(&state->scripts_path_label, "Choose script folder");
    add_option(grid, &row, "Script folder", state->scripts_button);
    g_signal_connect(state->scripts_button, "clicked", G_CALLBACK(select_scripts), page);

    state->restore_tabs = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->restore_tabs), "restore-tabs");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->restore_tabs),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Restore tabs on startup", -1);
    gtk_switch_set_active(state->restore_tabs, TRUE);
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->restore_tabs), "Reopen tabs and unsaved drafts on startup");
    add_option(grid, &row, "Restore tabs on startup", GTK_WIDGET(state->restore_tabs));
    g_signal_connect(state->restore_tabs, "notify::active", G_CALLBACK(restore_changed), state);

    GtkWidget *clear = gtk_button_new_with_label("Clear");
    gtk_accessible_update_property(GTK_ACCESSIBLE(clear), GTK_ACCESSIBLE_PROPERTY_LABEL, "Clear untitled tabs", -1);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(clear), "win.clear-untitled");
    add_option(grid, &row, "Untitled tabs", clear);

    state->output = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->output), "redirect-output");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->output),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Redirect script output", -1);
    add_option(grid, &row, "Show script output", GTK_WIDGET(state->output));
    g_signal_connect(state->output, "notify::active", G_CALLBACK(output_changed), state);

    section_start(grid, &row, "Profiles");
    state->launcher_button = path_button(&state->launcher_path, "Choose launcher executable");
    add_option(grid, &row, "Launcher", state->launcher_button);
    g_signal_connect(state->launcher_button, "clicked", G_CALLBACK(select_launcher), page);

    GtkWidget *configuration = gtk_button_new_with_label("Configure…");
    gtk_accessible_update_property(GTK_ACCESSIBLE(configuration),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Configure global profile defaults", -1);
    add_option(grid, &row, "Profile defaults", configuration);
    g_signal_connect(configuration, "clicked", G_CALLBACK(open_configuration), page);

    return page;
}
