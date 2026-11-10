#include "profiles.h"
#include "profiles/account.h"
#include "profiles/store.h"

typedef struct {
    GtkWidget *button;
    GtkImage *avatar;
    GtkLabel *summary;
    GtkLabel *detail;
    GtkWidget *arrow;
    GListStore *model;
    GtkMultiSelection *selection;
    char **chosen;
    gboolean updating;
    gboolean initialized;
    SocketSelectedFunc selected;
    gpointer user_data;
} SocketPicker;

typedef struct {
    char *profile;
    gboolean has_cookie;
    ProfileAccount *account;
    GdkTexture *texture;
} SocketIdentity;

typedef struct {
    GtkImage *avatar;
    GtkLabel *title;
    GtkLabel *detail;
} SocketRow;

static void free_identity(gpointer user_data)
{
    SocketIdentity *identity = user_data;
    g_free(identity->profile);
    profile_account_free(identity->account);
    g_clear_object(&identity->texture);
    g_free(identity);
}

static GtkStringObject *socket_item_new(const char *path)
{
    GtkStringObject *item = gtk_string_object_new(path);
    SocketIdentity *identity = g_new0(SocketIdentity, 1);
    identity->profile = profile_socket_name(path);
    if (profile_name_valid(identity->profile)) {
        g_autofree char *cookie = profile_cookie_read(identity->profile, NULL);
        identity->has_cookie = cookie != NULL;
        identity->account = profile_account_load(identity->profile, cookie);
    }
    g_object_set_data_full(G_OBJECT(item), "socket-identity", identity, free_identity);
    return item;
}

static gboolean identity_equal(GtkStringObject *left, GtkStringObject *right)
{
    SocketIdentity *a = g_object_get_data(G_OBJECT(left), "socket-identity");
    SocketIdentity *b = g_object_get_data(G_OBJECT(right), "socket-identity");
    if (g_strcmp0(gtk_string_object_get_string(left), gtk_string_object_get_string(right)) ||
        a->has_cookie != b->has_cookie || (a->account != NULL) != (b->account != NULL)) return FALSE;
    if (!a->account) return TRUE;
    if (g_strcmp0(a->account->username, b->account->username)) return FALSE;
    return a->account->avatar == b->account->avatar ||
        (a->account->avatar && b->account->avatar && g_bytes_equal(a->account->avatar, b->account->avatar));
}

static void set_avatar(GtkImage *image, SocketIdentity *identity)
{
    if (!identity->texture && identity->account && identity->account->avatar)
        identity->texture = gdk_texture_new_from_bytes(identity->account->avatar, NULL);
    if (identity->texture) gtk_image_set_from_paintable(image, GDK_PAINTABLE(identity->texture));
    else gtk_image_set_from_icon_name(image, "avatar-default-symbolic");
}

static void item_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    (void)factory; (void)user_data;
    SocketRow *row = g_new0(SocketRow, 1);
    g_object_set_data_full(G_OBJECT(item), "socket-row", row, g_free);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    row->avatar = GTK_IMAGE(gtk_image_new());
    gtk_image_set_pixel_size(row->avatar, 24);
    gtk_widget_set_valign(GTK_WIDGET(row->avatar), GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(row->avatar));
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(labels, TRUE);
    gtk_widget_set_valign(labels, GTK_ALIGN_CENTER);
    row->title = GTK_LABEL(gtk_label_new(NULL));
    row->detail = GTK_LABEL(gtk_label_new(NULL));
    GtkLabel *fields[] = {row->title, row->detail};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) {
        gtk_label_set_xalign(fields[i], 0);
        gtk_label_set_ellipsize(fields[i], PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(fields[i], 28);
        gtk_box_append(GTK_BOX(labels), GTK_WIDGET(fields[i]));
    }
    gtk_widget_add_css_class(GTK_WIDGET(row->title), "socket-name");
    gtk_widget_add_css_class(GTK_WIDGET(row->detail), "socket-profile");
    gtk_box_append(GTK_BOX(box), labels);
    gtk_list_item_set_child(item, box);
}

static void item_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    (void)factory; (void)user_data;
    SocketIdentity *identity = g_object_get_data(gtk_list_item_get_item(item), "socket-identity");
    SocketRow *row = g_object_get_data(G_OBJECT(item), "socket-row");
    g_autofree char *title = identity->account ? g_strconcat("@", identity->account->username, NULL)
        : g_strdup(identity->profile);
    g_autofree char *detail = identity->account ? g_strdup_printf("Profile: %s", identity->profile)
        : profile_name_valid(identity->profile) ? g_strdup(identity->has_cookie ? "Account unavailable" : "Logged out") : NULL;
    gtk_label_set_text(row->title, title);
    gtk_label_set_text(row->detail, detail ? detail : "");
    gtk_widget_set_visible(GTK_WIDGET(row->detail), detail != NULL);
    set_avatar(row->avatar, identity);
    g_autofree char *tooltip = detail ? g_strdup_printf("%s\n%s", title, detail) : g_strdup(title);
    gtk_widget_set_tooltip_text(gtk_list_item_get_child(item), tooltip);
}

static void update_summary(SocketPicker *state)
{
    guint count = g_strv_length(state->chosen);
    guint available = g_list_model_get_n_items(G_LIST_MODEL(state->model));
    g_autofree char *label = NULL;
    g_autofree char *detail = NULL;
    g_autoptr(GString) tooltip = g_string_new("Execute on selected clients");
    gtk_image_set_from_resource(state->avatar, "/org/relapse/RelapseGtk/icons/profiles.svg");
    if (count == 1) {
        g_autoptr(GtkBitset) selected = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(state->selection));
        g_autoptr(GtkStringObject) item = g_list_model_get_item(G_LIST_MODEL(state->model), gtk_bitset_get_minimum(selected));
        SocketIdentity *identity = g_object_get_data(G_OBJECT(item), "socket-identity");
        label = identity->account ? g_strconcat("@", identity->account->username, NULL) : g_strdup(identity->profile);
        detail = identity->account ? g_strdup(identity->profile)
            : profile_name_valid(identity->profile) ? g_strdup(identity->has_cookie ? "Account unavailable" : "Logged out") : NULL;
        set_avatar(state->avatar, identity);
    } else label = count ? g_strdup_printf("%u clients", count) : g_strdup(available ? "Select clients" : "No Instances");
    for (guint i = 0; state->chosen[i]; ++i) {
        g_autofree char *profile = profile_socket_name(state->chosen[i]);
        g_string_append_printf(tooltip, "\n%s", profile);
    }
    if (!available) g_string_assign(tooltip, "No running clients. Launch a profile to connect.");
    else g_string_append(tooltip, "\nCtrl-click to toggle · Shift-click for a range");
    gtk_label_set_text(state->summary, label);
    gtk_label_set_width_chars(state->summary, count == 1 ? 12 : -1);
    gtk_label_set_text(state->detail, detail ? detail : "");
    gtk_widget_set_visible(GTK_WIDGET(state->detail), detail != NULL);
    gtk_widget_set_visible(GTK_WIDGET(state->avatar), count > 0);
    gtk_widget_set_visible(state->arrow, available > 0);
    gtk_widget_set_tooltip_text(state->button, tooltip->str);
    gtk_widget_set_sensitive(state->button, available > 0);
}

static void selection_changed(GtkSelectionModel *selection, guint position, guint n_items, gpointer user_data)
{
    (void)position; (void)n_items;
    SocketPicker *state = user_data;
    if (state->updating) return;
    g_autoptr(GPtrArray) chosen = g_ptr_array_new_with_free_func(g_free);
    guint count = g_list_model_get_n_items(G_LIST_MODEL(state->model));
    for (guint i = 0; i < count; ++i) {
        if (!gtk_selection_model_is_selected(selection, i)) continue;
        g_autoptr(GtkStringObject) item = g_list_model_get_item(G_LIST_MODEL(state->model), i);
        g_ptr_array_add(chosen, g_strdup(gtk_string_object_get_string(item)));
    }
    g_ptr_array_add(chosen, NULL);
    gboolean changed = !g_strv_equal((const char *const *)state->chosen, (const char *const *)chosen->pdata);
    g_strfreev(state->chosen);
    state->chosen = (char **)g_ptr_array_steal(chosen, NULL);
    update_summary(state);
    if (changed && state->selected) state->selected((const char *const *)state->chosen, state->user_data);
}

static void free_socket_picker(gpointer user_data)
{
    SocketPicker *state = user_data;
    g_signal_handlers_disconnect_by_data(state->selection, state);
    g_strfreev(state->chosen);
    g_object_unref(state->selection);
    g_object_unref(state->model);
    g_free(state);
}

GtkWidget *socket_picker_new(SocketSelectedFunc selected, gpointer user_data)
{
    SocketPicker *state = g_new0(SocketPicker, 1);
    state->selected = selected;
    state->user_data = user_data;
    state->chosen = g_new0(char *, 1);
    state->model = g_list_store_new(GTK_TYPE_STRING_OBJECT);
    state->selection = gtk_multi_selection_new(G_LIST_MODEL(g_object_ref(state->model)));
    state->button = gtk_menu_button_new();
    gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(state->button), FALSE);
    gtk_widget_add_css_class(state->button, "socket-picker");
    gtk_widget_add_css_class(state->button, "execution-picker");
    gtk_accessible_update_property(GTK_ACCESSIBLE(state->button), GTK_ACCESSIBLE_PROPERTY_LABEL, "Clients to execute scripts in", -1);
    g_object_set_data_full(G_OBJECT(state->button), "socket-picker", state, free_socket_picker);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(content, "socket-current");
    state->avatar = GTK_IMAGE(gtk_image_new());
    gtk_image_set_pixel_size(state->avatar, 20);
    gtk_widget_set_valign(GTK_WIDGET(state->avatar), GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(state->avatar));
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(labels, TRUE);
    /* Keep the lone empty-state label centered just like a two-line identity. */
    gtk_widget_set_valign(labels, GTK_ALIGN_CENTER);
    state->summary = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(state->summary), "socket-name");
    gtk_label_set_ellipsize(state->summary, PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(state->summary, 18);
    gtk_label_set_xalign(state->summary, 0);
    gtk_box_append(GTK_BOX(labels), GTK_WIDGET(state->summary));
    state->detail = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(state->detail), "socket-profile");
    gtk_label_set_xalign(state->detail, 0);
    gtk_label_set_ellipsize(state->detail, PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(state->detail, 18);
    gtk_box_append(GTK_BOX(labels), GTK_WIDGET(state->detail));
    gtk_box_append(GTK_BOX(content), labels);
    state->arrow = gtk_image_new_from_resource("/org/relapse/RelapseGtk/icons/caret-down.svg");
    gtk_image_set_pixel_size(GTK_IMAGE(state->arrow), 12);
    gtk_box_append(GTK_BOX(content), state->arrow);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(state->button), content);
    GtkPopover *popover = GTK_POPOVER(gtk_popover_new());
    gtk_popover_set_has_arrow(popover, FALSE);
    gtk_popover_set_position(popover, GTK_POS_TOP);
    gtk_widget_set_halign(GTK_WIDGET(popover), GTK_ALIGN_END);
    gtk_widget_add_css_class(GTK_WIDGET(popover), "socket-choices");
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 280);
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(item_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(item_bind), NULL);
    GtkWidget *list = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(state->selection)), factory);
    gtk_accessible_update_property(GTK_ACCESSIBLE(list), GTK_ACCESSIBLE_PROPERTY_LABEL, "Execution clients", -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_popover_set_child(popover, scroll);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(state->button), GTK_WIDGET(popover));
    g_signal_connect(state->selection, "selection-changed", G_CALLBACK(selection_changed), state);
    update_summary(state);
    return state->button;
}

void socket_picker_update(GtkWidget *picker, const char *const *paths)
{
    SocketPicker *state = g_object_get_data(G_OBJECT(picker), "socket-picker");
    state->updating = TRUE;
    guint count = g_list_model_get_n_items(G_LIST_MODEL(state->model));
    gboolean paths_changed = count != g_strv_length((char **)paths);
    for (guint i = 0; i < count && !paths_changed; ++i) {
        g_autoptr(GtkStringObject) item = g_list_model_get_item(G_LIST_MODEL(state->model), i);
        paths_changed = g_strcmp0(gtk_string_object_get_string(item), paths[i]) != 0;
    }
    if (paths_changed) {
        g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_object_unref);
        for (guint i = 0; paths[i]; ++i) g_ptr_array_add(entries, socket_item_new(paths[i]));
        g_list_store_splice(state->model, 0, count, entries->pdata, entries->len);
    } else {
        for (guint i = 0; paths[i]; ++i) {
            g_autoptr(GtkStringObject) old = g_list_model_get_item(G_LIST_MODEL(state->model), i);
            g_autoptr(GtkStringObject) updated = socket_item_new(paths[i]);
            if (!identity_equal(old, updated)) {
                gpointer replacement[] = {updated};
                g_list_store_splice(state->model, i, 1, replacement, 1);
            }
        }
    }
    g_autoptr(GtkBitset) selected = gtk_bitset_new_empty();
    for (guint i = 0; paths[i]; ++i)
        if (g_strv_contains((const char *const *)state->chosen, paths[i])) gtk_bitset_add(selected, i);
    /* Choose the first client only on initial discovery. A closed target must
     * never silently redirect a later execution to a different client. */
    if (!state->initialized && paths[0]) {
        gtk_bitset_add(selected, 0);
        state->initialized = TRUE;
    }
    g_autoptr(GtkBitset) mask = gtk_bitset_new_range(0, g_list_model_get_n_items(G_LIST_MODEL(state->model)));
    gtk_selection_model_set_selection(GTK_SELECTION_MODEL(state->selection), selected, mask);
    state->updating = FALSE;
    selection_changed(GTK_SELECTION_MODEL(state->selection), 0, 0, state);
}
