#include "dialogs.h"

typedef struct {
    GtkWindow *window;
    gint64 id;
    DialogChoiceFunc choice;
    gpointer user_data;
    gboolean completed;
} Dialog;

static void finish(Dialog *dialog, guint choice)
{
    if (dialog->completed) return;
    dialog->completed = TRUE;
    gtk_widget_set_visible(GTK_WIDGET(dialog->window), FALSE);
    if (dialog->choice) dialog->choice(dialog->id, choice, dialog->user_data);
}

static void clicked(GtkButton *button, gpointer user_data)
{
    finish(user_data, GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "choice")));
}

static gboolean close_requested(GtkWindow *window, gpointer user_data)
{
    (void)window;
    finish(user_data, 0);
    return TRUE;
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
                            GdkModifierType modifiers, gpointer user_data)
{
    (void)controller; (void)keycode; (void)modifiers;
    if (keyval != GDK_KEY_Escape) return FALSE;
    finish(user_data, 0);
    return TRUE;
}

GtkWindow *dialog_window_new(GtkWindow *parent, gint64 id, const char *title, const char *text,
                             const char *const *buttons, const char *icon, guint default_button,
                             DialogChoiceFunc choice, gpointer user_data)
{
    Dialog *dialog = g_new0(Dialog, 1);
    dialog->id = id;
    dialog->choice = choice;
    dialog->user_data = user_data;
    GtkWindow *window = dialog->window = GTK_WINDOW(gtk_window_new());
    gtk_widget_add_css_class(GTK_WIDGET(window), "relapse-ui");
    gtk_window_set_title(window, title);
    gtk_window_set_transient_for(window, parent);
    gtk_window_set_default_size(window, 420, -1);
    g_object_set_data_full(G_OBJECT(window), "dialog", dialog, g_free);
    g_signal_connect(window, "close-request", G_CALLBACK(close_requested), dialog);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), dialog);
    gtk_widget_add_controller(GTK_WIDGET(window), keys);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(box, 20);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    gtk_window_set_child(window, box);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_box_append(GTK_BOX(box), content);
    if (!g_str_equal(icon, "none")) {
        const char *name = g_str_equal(icon, "error") ? "dialog-error-symbolic" :
                           g_str_equal(icon, "question") ? "dialog-question-symbolic" :
                           g_str_equal(icon, "warning") ? "dialog-warning-symbolic" : "dialog-information-symbolic";
        GtkWidget *image = gtk_image_new_from_icon_name(name);
        gtk_image_set_pixel_size(GTK_IMAGE(image), 32);
        gtk_widget_set_valign(image, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(content), image);
    }
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 64);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 400);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), label);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(content), scroll);

    GtkWidget *actions = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(actions), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(actions), 4);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(actions), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(actions), 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), actions);
    for (guint i = 0; buttons[i]; ++i) {
        GtkWidget *button = gtk_button_new_with_label(buttons[i]);
        g_object_set_data(G_OBJECT(button), "choice", GUINT_TO_POINTER(i + 1));
        g_signal_connect(button, "clicked", G_CALLBACK(clicked), dialog);
        gtk_flow_box_insert(GTK_FLOW_BOX(actions), button, -1);
        if (i + 1 == default_button) {
            gtk_window_set_default_widget(window, button);
            gtk_widget_grab_focus(button);
        }
    }
    return window;
}
