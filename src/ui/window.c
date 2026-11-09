#include "window.h"
#include "scripts.h"
#include "settings.h"
#include "tabs.h"
#include "toolbar.h"
#include "style.h"
#include "output.h"
#include "shader_strip.h"
#include "profiles.h"

typedef struct {
    GtkWidget *tabs;
    GtkWidget *toolbar;
    GtkWidget *output;
    GtkWidget *profiles;
    GtkWidget *scripts;
    GtkWidget *socket_picker;
    gboolean output_enabled;
    GtkStack *views;
    GtkWidget *editor_page;
    GtkWidget *settings_page;
    GtkToggleButton *editor_button;
    GtkToggleButton *settings_button;
    GSimpleAction *save;
    GSimpleAction *find[3];
    gboolean stopped;
    WindowHandlers handlers;
    gpointer user_data;
} Window;

static Window *window_data(GtkWindow *window)
{
    return g_object_get_data(G_OBJECT(window), "window");
}

void window_set_launcher(GtkWindow *window, const char *path)
{
    settings_set_launcher(window_data(window)->settings_page, path);
}

void window_set_scripts_path(GtkWindow *window, const char *path)
{
    Window *state = window_data(window);
    settings_set_scripts_path(state->settings_page, path);
    scripts_set_directory(state->scripts, path);
    toolbar_set_scripts_path(state->toolbar, path);
    tabs_set_scripts_path(state->tabs, path);
}

void window_show_settings(GtkWindow *window)
{
    Window *state = window_data(window);
    gtk_stack_set_visible_child(state->views, state->settings_page);
}

void window_set_output_target(GtkWindow *window, const char *path)
{
    output_set_target(window_data(window)->output, path);
}

void window_update_profiles(GtkWindow *window, const char *const *names,
                            const char *const *sockets, const char *selected)
{
    Window *state = window_data(window);
    profile_picker_update(state->profiles, names);
    socket_picker_update(state->socket_picker, sockets, selected);
}

static gboolean launcher_changed(const char *path, GError **error, gpointer user_data)
{
    Window *state = window_data(user_data);
    return !state->stopped && (!state->handlers.launcher_changed ||
        state->handlers.launcher_changed(path, error, state->user_data));
}

static gboolean scripts_changed(const char *path, GError **error, gpointer user_data)
{
    Window *state = window_data(user_data);
    return !state->stopped && (!state->handlers.scripts_changed ||
        state->handlers.scripts_changed(path, error, state->user_data));
}

static void socket_selected(const char *path, gpointer user_data)
{
    Window *state = window_data(user_data);
    if (!state->stopped && state->handlers.socket_selected)
        state->handlers.socket_selected(path, state->user_data);
}

static void launch_profile(const char *name, gpointer user_data)
{
    Window *state = window_data(user_data);
    if (!state->stopped && state->handlers.launch_profile)
        state->handlers.launch_profile(name, state->user_data);
}

static gboolean profile_running(const char *name, gpointer user_data)
{
    Window *state = window_data(user_data);
    return state->stopped || (state->handlers.profile_running &&
        state->handlers.profile_running(name, state->user_data));
}

static gboolean editor_visible(Window *window)
{
    return !window->stopped && window->views &&
        gtk_stack_get_visible_child(window->views) == window->editor_page;
}

void window_set_output_enabled(GtkWindow *window, gboolean enabled)
{
    Window *state = window_data(window);
    state->output_enabled = enabled;
    gtk_widget_set_visible(state->output, enabled);
    settings_set_output(state->settings_page, enabled);
}

void window_set_output_status(GtkWindow *window, const char *path, const char *text, gboolean active)
{
    output_set_status(window_data(window)->output, path, text, active);
}

void window_append_output(GtkWindow *window, const char *path, const char *level, const char *text)
{
    Window *state = window_data(window);
    if (state->output_enabled)
        output_append(state->output, path, level, text);
}

static void output_changed(gboolean enabled, gpointer user_data)
{
    GtkWindow *window = user_data;
    Window *state = window_data(window);
    if (state->stopped) return;
    window_set_output_enabled(window, enabled);
    g_autoptr(GError) error = NULL;
    if (state->handlers.output_changed &&
        !state->handlers.output_changed(enabled, &error, state->user_data)) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not save settings");
        gtk_alert_dialog_set_detail(alert, error ? error->message : "Changes will apply for this session only.");
        gtk_alert_dialog_show(alert, window);
    }
}

void window_set_execute_enabled(GtkWindow *window, gboolean enabled)
{
    Window *state = window_data(window);
    toolbar_set_execute_enabled(state->toolbar, enabled && editor_visible(state));
}

void window_set_status(GtkWindow *window, const char *label, const char *description, gboolean ready)
{
    toolbar_set_status(window_data(window)->toolbar, label, description, ready);
}

void window_open_script(GtkWindow *window, GFile *file, const char *display_name)
{
    Window *state = window_data(window);
    tabs_open(state->tabs, file, display_name);
    gtk_stack_set_visible_child(state->views, state->editor_page);
}

void window_set_editor_settings(GtkWindow *window, const EditorSettings *settings)
{
    Window *state = window_data(window);
    tabs_set_editor_settings(state->tabs, settings);
    settings_set_editor(state->settings_page, settings);
    style_set_editor_font_size(gtk_widget_get_display(GTK_WIDGET(window)), settings->font_size);
}

gboolean window_start_session(GtkWindow *window, gboolean enabled, GError **error)
{
    Window *state = window_data(window);
    settings_set_restore_tabs(state->settings_page, enabled);
    if (enabled && !tabs_restore_session(state->tabs, error)) {
        settings_set_restore_tabs(state->settings_page, FALSE);
        return FALSE;
    }
    tabs_set_session_enabled(state->tabs, enabled);
    return TRUE;
}

static void restore_tabs_changed(gboolean enabled, gpointer user_data)
{
    GtkWindow *window = user_data;
    Window *state = window_data(window);
    if (state->stopped)
        return;
    tabs_set_session_enabled(state->tabs, enabled);
    g_autoptr(GError) error = NULL;
    if (state->handlers.restore_tabs_changed &&
        !state->handlers.restore_tabs_changed(enabled, &error, state->user_data)) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not save settings");
        gtk_alert_dialog_set_detail(alert, error ? error->message : "Changes will apply for this session only.");
        gtk_alert_dialog_show(alert, window);
    }
}

static void editor_settings_changed(const EditorSettings *settings, gpointer user_data)
{
    GtkWindow *window = user_data;
    Window *state = window_data(window);
    if (state->stopped)
        return;
    window_set_editor_settings(window, settings);
    g_autoptr(GError) error = NULL;
    if (state->handlers.editor_settings_changed &&
        !state->handlers.editor_settings_changed(settings, &error, state->user_data)) {
        g_autoptr(GtkAlertDialog) alert = gtk_alert_dialog_new("Could not save settings");
        gtk_alert_dialog_set_detail(alert, error ? error->message : "Changes will apply for this session only.");
        gtk_alert_dialog_show(alert, window);
    }
}

GtkTextBuffer *window_get_buffer(GtkWindow *window)
{
    Window *state = window_data(window);
    return editor_visible(state) ? tabs_get_buffer(state->tabs) : NULL;
}

const char *window_get_script_name(GtkWindow *window)
{
    Window *state = window_data(window);
    return editor_visible(state) ? tabs_get_name(state->tabs) : NULL;
}

static void execute_requested(gpointer user_data)
{
    Window *window = user_data;
    if (editor_visible(window) && window->handlers.execute)
        window->handlers.execute(window->user_data);
}

static void script_selected(GFile *file, const char *display_name, gpointer user_data)
{
    Window *window = user_data;
    if (window->handlers.open_script)
        window->handlers.open_script(file, display_name, window->user_data);
}

static GFile *script_renamed(GFile *file, const char *name, GError **error, gpointer user_data)
{
    Window *window = user_data;
    return tabs_rename(window->tabs, file, name, error);
}

static void save_active_requested(gpointer user_data)
{
    Window *window = user_data;
    if (editor_visible(window))
        tabs_save(window->tabs);
}

static void delete_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)user_data;
    g_autoptr(GError) error = NULL;
    if (!g_file_delete_finish(G_FILE(source), result, &error) &&
        !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning("delete failed: %s", error ? error->message : "unknown");
}

static void delete_confirmed(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Window *window = user_data;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL);
    if (choice != 1 || window->stopped) return;
    GFile *file = tabs_get_current_file(window->tabs);
    if (!file) return;
    g_object_ref(file);
    tabs_close_current(window->tabs);
    g_file_delete_async(file, G_PRIORITY_DEFAULT, NULL, delete_finished, NULL);
    g_object_unref(file);
}

static void delete_active_requested(gpointer user_data)
{
    Window *window = user_data;
    if (!editor_visible(window)) return;
    GFile *file = tabs_get_current_file(window->tabs);
    if (!file) return;
    g_autofree char *basename = g_file_get_basename(file);
    g_autofree char *msg = g_strdup_printf("Delete %s?", basename ? basename : "this script");
    GtkAlertDialog *alert = gtk_alert_dialog_new("%s", msg);
    gtk_alert_dialog_set_detail(alert, "This will remove the file from disk.");
    const char *buttons[] = {"Cancel", "Delete", NULL};
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 0);
    gtk_alert_dialog_set_default_button(alert, 0);
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(window->tabs)));
    gtk_alert_dialog_choose(alert, parent, NULL, delete_confirmed, window);
    g_object_unref(alert);
}

static void tabs_changed(GtkWidget *tabs, gpointer user_data)
{
    Window *window = user_data;
    if (window->stopped)
        return;
    gboolean have_buffer = editor_visible(window) && tabs_get_buffer(tabs) != NULL;
    g_simple_action_set_enabled(window->save, have_buffer);
    for (guint i = 0; i < G_N_ELEMENTS(window->find); ++i)
        g_simple_action_set_enabled(window->find[i], have_buffer);
    if (window->toolbar)
        toolbar_set_active_enabled(window->toolbar, have_buffer);
    if (window->handlers.active_script_changed)
        window->handlers.active_script_changed(window->user_data);
}

static void save_requested(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    Window *window = user_data;
    if (editor_visible(window))
        tabs_save(window->tabs);
}

static void find_requested(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)parameter;
    Window *window = user_data;
    if (!editor_visible(window))
        return;
    const char *name = g_action_get_name(G_ACTION(action));
    if (g_str_equal(name, "find"))
        tabs_find(window->tabs);
    else
        tabs_find_next(window->tabs, g_str_equal(name, "find-previous"));
}

static void view_selected(GtkToggleButton *button, gpointer user_data)
{
    Window *window = user_data;
    if (!window->stopped && gtk_toggle_button_get_active(button))
        gtk_stack_set_visible_child_name(window->views,
            g_object_get_data(G_OBJECT(button), "view-name"));
}

static void view_changed(GtkStack *views, GParamSpec *property, gpointer user_data)
{
    (void)property;
    Window *window = user_data;
    if (window->stopped || !gtk_stack_get_visible_child(views))
        return;
    gtk_toggle_button_set_active(editor_visible(window) ? window->editor_button : window->settings_button, TRUE);
    tabs_changed(window->tabs, window);
}

static GtkToggleButton *navigation_button(const char *label, const char *view,
                                          const char *icon, Window *window)
{
    GtkWidget *button = gtk_toggle_button_new();
    gtk_widget_add_css_class(button, "navigation-button");
    gtk_widget_set_tooltip_text(button, label);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    GtkWidget *image = gtk_image_new_from_resource(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(image), 22);
    gtk_button_set_child(GTK_BUTTON(button), image);
    g_object_set_data_full(G_OBJECT(button), "view-name", g_strdup(view), g_free);
    g_signal_connect(button, "toggled", G_CALLBACK(view_selected), window);
    return GTK_TOGGLE_BUTTON(button);
}

static GtkWidget *create_navigation(Window *window)
{
    GtkWidget *rail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(rail, "navigation-rail");
    gtk_accessible_update_property(GTK_ACCESSIBLE(rail), GTK_ACCESSIBLE_PROPERTY_LABEL, "Navigation", -1);
    window->editor_button = navigation_button("Editor", "editor",
        "/org/relapse/RelapseGtk/icons/editor.svg", window);
    window->settings_button = navigation_button("Settings", "settings",
        "/org/relapse/RelapseGtk/icons/settings.svg", window);
    gtk_toggle_button_set_group(window->settings_button, window->editor_button);
    gtk_box_append(GTK_BOX(rail), GTK_WIDGET(window->editor_button));
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(rail), spacer);
    gtk_box_append(GTK_BOX(rail), GTK_WIDGET(window->settings_button));
    gtk_toggle_button_set_active(window->editor_button, TRUE);
    g_signal_connect(window->views, "notify::visible-child-name", G_CALLBACK(view_changed), window);
    return rail;
}

static void window_destroyed(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    Window *window = user_data;
    window->stopped = TRUE;
    if (window->profiles) {
        profile_picker_shutdown(window->profiles);
        window->profiles = NULL;
    }
    if (window->settings_page) {
        settings_shutdown(window->settings_page);
        window->settings_page = NULL;
    }
    if (window->toolbar) {
        toolbar_shutdown(window->toolbar);
        window->toolbar = NULL;
    }
    if (window->tabs) {
        tabs_shutdown(window->tabs);
        g_clear_object(&window->tabs);
    }
    g_clear_object(&window->views);
    g_clear_object(&window->save);
    for (guint i = 0; i < G_N_ELEMENTS(window->find); ++i)
        g_clear_object(&window->find[i]);
}

static void free_window(gpointer user_data)
{
    window_destroyed(NULL, user_data);
    g_free(user_data);
}

static void toggle_maximize(GtkWindow *window)
{
    if (gtk_window_is_maximized(window))
        gtk_window_unmaximize(window);
    else
        gtk_window_maximize(window);
}

static GtkWidget *create_titlebar(GtkWindow *window)
{
    GtkWidget *bar = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(bar), FALSE);

    GtkWidget *logo = gtk_image_new_from_resource("/org/relapse/RelapseGtk/icons/relapse.svg");
    gtk_image_set_pixel_size(GTK_IMAGE(logo), 20);
    gtk_widget_add_css_class(logo, "app-logo");
    gtk_widget_set_margin_start(logo, 4);
    gtk_widget_set_margin_end(logo, 4);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(bar), logo);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_widget_add_css_class(title, "title");
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    g_object_bind_property(window, "title", title, "label", G_BINDING_SYNC_CREATE);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(bar), title);

    const struct {
        const char *glyph;
        const char *label;
        GCallback callback;
    } controls[] = {
        {"−", "Minimize", G_CALLBACK(gtk_window_minimize)},
        {"□", "Maximize or restore", G_CALLBACK(toggle_maximize)},
        {"×", "Close", G_CALLBACK(gtk_window_close)}
    };
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    for (guint i = 0; i < G_N_ELEMENTS(controls); ++i) {
        GtkWidget *button = gtk_button_new_with_label(controls[i].glyph);
        gtk_widget_add_css_class(button, "window-control");
        gtk_widget_set_tooltip_text(button, controls[i].label);
        gtk_accessible_update_property(GTK_ACCESSIBLE(button),
                                       GTK_ACCESSIBLE_PROPERTY_LABEL, controls[i].label, -1);
        g_signal_connect_swapped(button, "clicked", controls[i].callback, window);
        gtk_box_append(GTK_BOX(buttons), button);
    }
    gtk_header_bar_pack_end(GTK_HEADER_BAR(bar), buttons);

    GtkWidget *wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(wrap, "titlebar-wrap");
    gtk_box_append(GTK_BOX(wrap), bar);
    gtk_box_append(GTK_BOX(wrap), shader_strip_new());
    return wrap;
}

GtkWindow *window_new(GtkApplication *application, const char *scripts_path,
                      const WindowHandlers *handlers, gpointer user_data)
{
    Window *state = g_new0(Window, 1);
    state->user_data = user_data;
    GtkWindow *window = GTK_WINDOW(gtk_application_window_new(application));
    gtk_widget_add_css_class(GTK_WIDGET(window), "relapse-ui");
    g_object_set_data_full(G_OBJECT(window), "window", state, free_window);
    g_signal_connect(window, "destroy", G_CALLBACK(window_destroyed), state);
    gtk_window_set_default_size(window, 860, 480);
    gtk_window_set_title(window, "relapsegtk");
    gtk_window_set_titlebar(window, create_titlebar(window));

    state->save = g_simple_action_new("save", NULL);
    g_simple_action_set_enabled(state->save, FALSE);
    g_signal_connect(state->save, "activate", G_CALLBACK(save_requested), state);
    g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(state->save));
    const char *save_accels[] = {"<Control>s", NULL};
    gtk_application_set_accels_for_action(application, "win.save", save_accels);

    const char *find_names[] = {"find", "find-next", "find-previous"};
    const char *find_accels[] = {"<Control>f", "F3", "<Shift>F3"};
    for (guint i = 0; i < G_N_ELEMENTS(find_names); ++i) {
        state->find[i] = g_simple_action_new(find_names[i], NULL);
        g_simple_action_set_enabled(state->find[i], FALSE);
        g_signal_connect(state->find[i], "activate", G_CALLBACK(find_requested), state);
        g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(state->find[i]));
        g_autofree char *action = g_strconcat("win.", find_names[i], NULL);
        const char *accels[] = {find_accels[i], NULL};
        gtk_application_set_accels_for_action(application, action, accels);
    }

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    state->tabs = g_object_ref_sink(tabs_new(scripts_path, tabs_changed, state));
    GtkWidget *editor_split = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(editor_split, "editor-output-split");
    gtk_widget_set_vexpand(editor_split, TRUE);
    gtk_paned_set_start_child(GTK_PANED(editor_split), state->tabs);
    state->output = output_new();
    gtk_widget_set_visible(state->output, FALSE);
    gtk_paned_set_end_child(GTK_PANED(editor_split), state->output);
    gtk_paned_set_resize_end_child(GTK_PANED(editor_split), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(editor_split), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(editor_split), FALSE);
    gtk_paned_set_position(GTK_PANED(editor_split), 240);
    gtk_box_append(GTK_BOX(box), editor_split);
    ToolbarHandlers toolbar_handlers = {
        .execute       = execute_requested,
        .open_script   = script_selected,
        .save_active   = save_active_requested,
        .delete_active = delete_active_requested,
    };
    state->socket_picker = socket_picker_new(socket_selected, window);
    state->toolbar = toolbar_new(window, scripts_path, state->socket_picker, &toolbar_handlers, state);
    gtk_box_append(GTK_BOX(box), state->toolbar);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    state->profiles = profile_picker_new(launch_profile, profile_running, window);
    state->scripts = scripts_new(scripts_path, script_selected, script_renamed, state);
    gtk_widget_set_vexpand(state->scripts, TRUE);
    gtk_box_append(GTK_BOX(sidebar), state->scripts);
    gtk_box_append(GTK_BOX(sidebar), state->profiles);
    gtk_paned_set_start_child(GTK_PANED(paned), sidebar);
    gtk_paned_set_end_child(GTK_PANED(paned), box);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 220);
    state->editor_page = paned;
    state->views = g_object_ref_sink(GTK_STACK(gtk_stack_new()));
    gtk_widget_set_hexpand(GTK_WIDGET(state->views), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(state->views), TRUE);
    gtk_stack_add_titled(state->views, paned, "editor", "Editor");
    EditorSettings defaults = editor_settings_defaults();
    state->settings_page = settings_new(&defaults, editor_settings_changed, restore_tabs_changed,
                                        output_changed, launcher_changed, scripts_changed, window);
    gtk_stack_add_titled(state->views, state->settings_page, "settings", "Settings");
    window_set_editor_settings(window, &defaults);
    settings_set_scripts_path(state->settings_page, scripts_path);

    GtkWidget *shell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(shell), create_navigation(state));
    gtk_box_append(GTK_BOX(shell), GTK_WIDGET(state->views));
    gtk_window_set_child(window, shell);
    tabs_changed(state->tabs, state);
    if (handlers)
        state->handlers = *handlers;
    return window;
}
