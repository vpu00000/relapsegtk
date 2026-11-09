#include "settings.h"
#include "picker.h"

#include <glib/gstdio.h>
#include <unistd.h>

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
    GtkWidget *scripts_button;
    GtkLabel *scripts_path_label;
    ScriptsChangedFunc scripts_changed;
    char *scripts_path;
    GCancellable *picker;
    gboolean stopped;
    gpointer user_data;
} Settings;

static const int indent_widths[] = {2, 4, 8};

void settings_set_launcher(GtkWidget *page, const char *path)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    g_autofree char *name = path && *path ? g_filename_display_basename(path) : g_strdup("None selected");
    gtk_label_set_text(state->launcher_path, name);
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->launcher_path), path);
}

void settings_set_scripts_path(GtkWidget *page, const char *path)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    g_free(state->scripts_path);
    state->scripts_path = g_strdup(path);
    g_autofree char *name = path && *path ? g_filename_display_basename(path) : g_strdup("None selected");
    gtk_label_set_text(state->scripts_path_label, name);
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->scripts_path_label), path);
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

void settings_shutdown(GtkWidget *page)
{
    Settings *state = g_object_get_data(G_OBJECT(page), "settings");
    state->stopped = TRUE;
    if (state->picker) g_cancellable_cancel(state->picker);
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

static void add_row(GtkWidget *group, const char *title, const char *description, GtkWidget *control)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_add_css_class(row, "settings-row");
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(labels, TRUE);
    gtk_widget_set_valign(labels, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(labels), label_new(title, "settings-label"));
    GtkWidget *detail = label_new(description, "settings-description");
    gtk_label_set_wrap(GTK_LABEL(detail), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(detail), 33);
    gtk_box_append(GTK_BOX(labels), detail);
    gtk_box_append(GTK_BOX(row), labels);
    GtkWidget *slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(slot, 132, -1);
    gtk_widget_set_valign(slot, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_widget_set_halign(control, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(slot), control);
    gtk_box_append(GTK_BOX(row), slot);
    gtk_box_append(GTK_BOX(group), row);
}

static GtkWidget *section_new(GtkWidget *column, const char *title, GtkWidget **out_group)
{
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(section, TRUE);
    gtk_box_append(GTK_BOX(column), section);
    gtk_box_append(GTK_BOX(section), label_new(title, "settings-group-title"));
    GtkWidget *group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(group, "settings-group");
    gtk_widget_set_hexpand(group, TRUE);
    gtk_box_append(GTK_BOX(section), group);
    *out_group = group;
    return section;
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

    GtkWidget *columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_widget_add_css_class(columns, "settings-columns");
    gtk_widget_set_hexpand(columns, TRUE);
    gtk_box_append(GTK_BOX(content), columns);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_hexpand(left, TRUE);
    gtk_widget_set_valign(left, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(columns), left);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_hexpand(right, TRUE);
    gtk_widget_set_valign(right, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(columns), right);

    GtkWidget *editor_group = NULL;
    section_new(left, "Editor", &editor_group);

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
    add_row(editor_group, "Font size", "Editor text and line numbers", font);

    state->word_wrap = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->word_wrap), "word-wrap");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->word_wrap),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Word wrap", -1);
    add_row(editor_group, "Word wrap", "Wrap long lines to fit the editor", GTK_WIDGET(state->word_wrap));

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
    add_row(editor_group, "Indentation", "Spaces used by Tab and auto-indent", indent);
    settings_set_editor(page, settings);
    g_signal_connect(state->font_size, "value-changed", G_CALLBACK(values_changed), state);
    g_signal_connect(state->word_wrap, "notify::active", G_CALLBACK(wrap_changed), state);

    GtkWidget *session_group = NULL;
    section_new(left, "Session", &session_group);
    state->restore_tabs = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->restore_tabs), "restore-tabs");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->restore_tabs),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Restore tabs on startup", -1);
    gtk_switch_set_active(state->restore_tabs, TRUE);
    add_row(session_group, "Restore tabs on startup", "Reopen tabs and unsaved drafts", GTK_WIDGET(state->restore_tabs));
    g_signal_connect(state->restore_tabs, "notify::active", G_CALLBACK(restore_changed), state);

    GtkWidget *output_group = NULL;
    section_new(right, "Output", &output_group);
    state->output = GTK_SWITCH(gtk_switch_new());
    gtk_widget_add_css_class(GTK_WIDGET(state->output), "redirect-output");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->output),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Redirect script output", -1);
    add_row(output_group, "Redirect script output", "Show your scripts’ output below the editor", GTK_WIDGET(state->output));
    g_signal_connect(state->output, "notify::active", G_CALLBACK(output_changed), state);

    GtkWidget *scripts_group = NULL;
    section_new(right, "Scripts", &scripts_group);
    GtkWidget *scripts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    state->scripts_button = gtk_button_new_with_label("Select…");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->scripts_button),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Select script folder", -1);
    gtk_box_append(GTK_BOX(scripts), state->scripts_button);
    state->scripts_path_label = GTK_LABEL(gtk_label_new("None selected"));
    gtk_label_set_ellipsize(state->scripts_path_label, PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(state->scripts_path_label, 16);
    gtk_widget_add_css_class(GTK_WIDGET(state->scripts_path_label), "settings-description");
    gtk_box_append(GTK_BOX(scripts), GTK_WIDGET(state->scripts_path_label));
    add_row(scripts_group, "Script folder", "Folder shown in the Scripts library", scripts);
    g_signal_connect(state->scripts_button, "clicked", G_CALLBACK(select_scripts), page);

    GtkWidget *launcher_group = NULL;
    section_new(right, "Profiles", &launcher_group);
    GtkWidget *launcher = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    state->launcher_button = gtk_button_new_with_label("Select…");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->launcher_button),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Select launcher", -1);
    gtk_box_append(GTK_BOX(launcher), state->launcher_button);
    state->launcher_path = GTK_LABEL(gtk_label_new("None selected"));
    gtk_label_set_ellipsize(state->launcher_path, PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(state->launcher_path, 16);
    gtk_widget_add_css_class(GTK_WIDGET(state->launcher_path), "settings-description");
    gtk_box_append(GTK_BOX(launcher), GTK_WIDGET(state->launcher_path));
    add_row(launcher_group, "Launcher", "Executable used to launch Profiles", launcher);
    g_signal_connect(state->launcher_button, "clicked", G_CALLBACK(select_launcher), page);

    return page;
}
