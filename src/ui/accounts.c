#include "accounts.h"
#include "profiles/account.h"
#include "profiles/store.h"
#include <string.h>

typedef enum {
    FILTER_ALL, FILTER_ACCOUNT, FILTER_LOGGED_OUT, FILTER_RUNNING, FILTER_STOPPED
} AccountFilter;

typedef struct {
    GtkWidget *widget;
    GtkListBox *list;
    GtkSearchEntry *search;
    GtkDropDown *filter;
    GtkEntry *place_id;
    GtkEntry *job_id;
    GtkLabel *join_error;
    GtkLabel *selection;
    GtkWidget *clear_selection;
    GtkWidget *launch_button;
    GtkLabel *launch_label;
    GtkLabel *empty;
    GtkWindow *dialog;
    char **names;
    char *query;
    GHashTable *refresh_attempts;
    gboolean stopped;
    gboolean syncing_selection;
    gboolean launching;
    ProfileLaunchFunc launch;
    ProfileRunningFunc running;
    gpointer user_data;
} Accounts;

typedef struct {
    Accounts *accounts;
    GtkWidget *widget;
    char *name;
    char *cookie;
    ProfileAccount *account;
    GtkImage *avatar;
    GtkLabel *title;
    GtkLabel *detail;
    GtkLabel *message;
    GtkCheckButton *select;
    GtkWidget *running_label;
    char *launch_error;
    GtkWidget *refresh;
    GtkWidget *load;
    GtkWidget *rename;
    GtkWidget *copy;
    GtkWidget *delete;
    gboolean is_running;
    guint generation;
    GCancellable *request;
} AccountRow;

typedef enum { ADD_ACCOUNT, LOAD_COOKIE, NEW_PROFILE, RENAME_PROFILE, DELETE_PROFILE } DialogMode;

typedef struct {
    GtkWindow *window;
    GWeakRef owner;
    DialogMode mode;
    char *name;
    GtkWidget *cookie;
    GtkWidget *name_field;
    GtkEntry *name_entry;
    GtkWidget *identity;
    GtkImage *avatar;
    GtkLabel *title;
    GtkLabel *username;
    GtkLabel *error;
    GtkButton *submit;
    ProfileAccount *account;
    GCancellable *request;
    guint generation;
    gboolean stopped;
} AccountDialog;

typedef struct { GWeakRef owner; guint generation; } Lookup;

static void open_dialog(Accounts *accounts, DialogMode mode, const char *name);
static void row_sync(AccountRow *row, gboolean reload);
static void selection_sync(Accounts *accounts);

static void set_message(GtkLabel *label, const char *message)
{
    gtk_label_set_text(label, message ? message : "");
    gtk_widget_set_visible(GTK_WIDGET(label), message && *message);
}

static GtkLabel *label_new(const char *text, const char *style)
{
    GtkLabel *label = GTK_LABEL(gtk_label_new(text));
    gtk_label_set_xalign(label, 0);
    gtk_label_set_ellipsize(label, PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(label, 18);
    gtk_widget_set_hexpand(GTK_WIDGET(label), TRUE);
    if (style) gtk_widget_add_css_class(GTK_WIDGET(label), style);
    return label;
}

static GtkWidget *button_content(const char *icon, const char *label)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *image = gtk_image_new_from_icon_name(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(image), 12);
    gtk_box_append(GTK_BOX(box), image);
    gtk_box_append(GTK_BOX(box), gtk_label_new(label));
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    return box;
}

static GtkWidget *menu_new(GtkWidget *child, const char *label)
{
    GtkWidget *menu = gtk_menu_button_new();
    gtk_menu_button_set_child(GTK_MENU_BUTTON(menu), child);
    gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(menu), FALSE);
    gtk_widget_set_tooltip_text(menu, label);
    gtk_accessible_update_property(GTK_ACCESSIBLE(menu), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    GtkPopover *popover = GTK_POPOVER(gtk_popover_new());
    gtk_popover_set_has_arrow(popover, FALSE);
    gtk_widget_set_halign(GTK_WIDGET(popover), GTK_ALIGN_END);
    gtk_widget_add_css_class(GTK_WIDGET(popover), "account-menu");
    gtk_popover_set_child(popover, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), GTK_WIDGET(popover));
    return menu;
}

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback callback, gpointer data)
{
    GtkPopover *popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(menu));
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(button, "menu-item");
    gtk_label_set_xalign(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(button))), 0);
    gtk_box_append(GTK_BOX(gtk_popover_get_child(popover)), button);
    g_signal_connect_swapped(button, "clicked", G_CALLBACK(gtk_popover_popdown), popover);
    g_signal_connect(button, "clicked", callback, data);
    return button;
}

static void row_set_message(AccountRow *row, const char *message)
{
    if (row->launch_error) message = row->launch_error;
    g_autofree char *text = message ? g_strconcat("· ", message, NULL) : NULL;
    set_message(row->message, text);
    gtk_widget_set_tooltip_text(GTK_WIDGET(row->message), message);
}

static void set_avatar(GtkImage *image, const ProfileAccount *account)
{
    g_autoptr(GdkTexture) texture = account && account->avatar
        ? gdk_texture_new_from_bytes(account->avatar, NULL) : NULL;
    if (texture) gtk_image_set_from_paintable(image, GDK_PAINTABLE(texture));
    else gtk_image_set_from_icon_name(image, "avatar-default-symbolic");
}

static gboolean row_running(AccountRow *row)
{
    return row->accounts->running && row->accounts->running(row->name, row->accounts->user_data);
}

static gboolean search_matches(const char *text, const char *query)
{
    if (!text) return FALSE;
    g_autofree char *folded = g_utf8_casefold(text, -1);
    return strstr(folded, query) != NULL;
}

static gboolean filter_row(GtkListBoxRow *widget, gpointer user_data)
{
    Accounts *accounts = user_data;
    AccountRow *row = g_object_get_data(G_OBJECT(widget), "account-row");
    if (!row) return FALSE;
    switch ((AccountFilter)gtk_drop_down_get_selected(accounts->filter)) {
    case FILTER_ACCOUNT: if (!row->account) return FALSE; break;
    case FILTER_LOGGED_OUT: if (row->cookie) return FALSE; break;
    case FILTER_RUNNING: if (!row->is_running) return FALSE; break;
    case FILTER_STOPPED: if (row->is_running) return FALSE; break;
    default: break;
    }
    const char *query = accounts->query;
    if (!query || !*query || search_matches(row->name, query)) return TRUE;
    return row->account && (search_matches(row->account->display_name, query) ||
        search_matches(row->account->username, *query == '@' ? query + 1 : query));
}

static void update_filter(Accounts *accounts)
{
    if (accounts->stopped) return;
    g_free(accounts->query);
    accounts->query = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(accounts->search)), -1);
    g_strstrip(accounts->query);
    gboolean filtered = *accounts->query || gtk_drop_down_get_selected(accounts->filter) != FILTER_ALL;
    gtk_label_set_text(accounts->empty, filtered ? "No matching profiles." : "Add an account or create a profile.");
    gtk_list_box_invalidate_filter(accounts->list);
    selection_sync(accounts);
}

static AccountRow *find_row(Accounts *accounts, const char *name)
{
    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(accounts->list)); child;
         child = gtk_widget_get_next_sibling(child)) {
        AccountRow *row = g_object_get_data(G_OBJECT(child), "account-row");
        if (row && g_str_equal(row->name, name)) return row;
    }
    return NULL;
}

static void selection_sync(Accounts *accounts)
{
    if (accounts->stopped || accounts->syncing_selection) return;
    accounts->syncing_selection = TRUE;
    guint count = 0;
    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(accounts->list)); child;
         child = gtk_widget_get_next_sibling(child)) {
        AccountRow *row = g_object_get_data(G_OBJECT(child), "account-row");
        if (!row) continue;
        gboolean selected = gtk_list_box_row_is_selected(GTK_LIST_BOX_ROW(child));
        if (selected && (row->is_running || !filter_row(GTK_LIST_BOX_ROW(child), accounts))) {
            gtk_list_box_unselect_row(accounts->list, GTK_LIST_BOX_ROW(child));
            selected = FALSE;
        }
        gtk_check_button_set_active(row->select, selected);
        if (selected) ++count;
    }
    g_autofree char *selection = count ? g_strdup_printf("%u selected", count) : g_strdup("Select accounts");
    g_autofree char *launch = count ? g_strdup_printf("Launch %u", count) : g_strdup("Launch");
    gtk_label_set_text(accounts->selection, selection);
    gtk_label_set_text(accounts->launch_label, launch);
    gtk_widget_set_sensitive(accounts->clear_selection, count > 0 && !accounts->launching);
    gtk_widget_set_sensitive(accounts->launch_button, count > 0 && !accounts->launching && accounts->launch);
    accounts->syncing_selection = FALSE;
}

static void selected_rows_changed(GtkListBox *list, gpointer user_data)
{
    (void)list;
    selection_sync(user_data);
}

static void select_toggled(GtkCheckButton *button, gpointer user_data)
{
    AccountRow *row = user_data;
    Accounts *accounts = row->accounts;
    if (accounts->stopped || accounts->syncing_selection) return;
    if (gtk_check_button_get_active(button) && !row->is_running)
        gtk_list_box_select_row(accounts->list, GTK_LIST_BOX_ROW(row->widget));
    else gtk_list_box_unselect_row(accounts->list, GTK_LIST_BOX_ROW(row->widget));
    selection_sync(accounts);
}

static void clear_selection_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    Accounts *accounts = user_data;
    if (!accounts->stopped) gtk_list_box_unselect_all(accounts->list);
}

static void search_changed(GtkEditable *editable, gpointer user_data)
{
    (void)editable;
    update_filter(user_data);
}

static void filter_changed(GObject *object, GParamSpec *property, gpointer user_data)
{
    (void)object;
    (void)property;
    update_filter(user_data);
}

static void free_lookup(Lookup *lookup)
{
    g_weak_ref_clear(&lookup->owner);
    g_free(lookup);
}

static Lookup *lookup_new(GObject *owner, guint generation)
{
    Lookup *lookup = g_new0(Lookup, 1);
    g_weak_ref_init(&lookup->owner, owner);
    lookup->generation = generation;
    return lookup;
}

static void row_checked(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)source;
    Lookup *lookup = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(ProfileAccount) account = profile_account_lookup_finish(result, &error);
    g_autoptr(GtkWidget) widget = g_weak_ref_get(&lookup->owner);
    AccountRow *row = widget ? g_object_get_data(G_OBJECT(widget), "account-row") : NULL;
    if (row && !row->accounts->stopped) {
        g_clear_object(&row->request);
        row_sync(row, FALSE);
        if (lookup->generation == row->generation) {
            if (account && profile_account_save(row->name, account, &error)) {
                row_sync(row, TRUE);
            } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                row_set_message(row, error ? error->message : "Could not refresh account.");
            }
        }
    }
    free_lookup(lookup);
}

static void row_refresh(AccountRow *row, gboolean automatic)
{
    if (row->accounts->stopped || row->request || !row->cookie) return;
    g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, row->cookie, -1);
    g_autofree char *key = g_strdup_printf("%s:%s", row->name, hash);
    if (automatic && g_hash_table_contains(row->accounts->refresh_attempts, key)) return;
    g_hash_table_add(row->accounts->refresh_attempts, g_steal_pointer(&key));
    row->request = g_cancellable_new();
    if (row->account) row_set_message(row, "Checking…");
    else {
        gtk_label_set_text(row->detail, "Checking account…");
        gtk_widget_set_tooltip_text(GTK_WIDGET(row->detail), "Checking account…");
        row_set_message(row, NULL);
    }
    gtk_widget_set_sensitive(row->refresh, FALSE);
    gtk_widget_set_sensitive(row->load, FALSE);
    gtk_widget_set_sensitive(row->rename, FALSE);
    gtk_widget_set_sensitive(row->delete, FALSE);
    profile_account_lookup_async(row->cookie, row->request, row_checked,
        lookup_new(G_OBJECT(row->widget), row->generation));
}

static void refresh_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    AccountRow *row = user_data;
    if (row->accounts->stopped || row->request) return;
    row_sync(row, FALSE);
    row_refresh(row, FALSE);
}

static void refresh_all_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    Accounts *accounts = user_data;
    if (accounts->stopped) return;
    g_auto(GStrv) names = profiles_list();
    accounts_update(accounts->widget, (const char *const *)names);
    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(accounts->list)); child;
         child = gtk_widget_get_next_sibling(child)) {
        AccountRow *row = g_object_get_data(G_OBJECT(child), "account-row");
        if (row) refresh_clicked(NULL, row);
    }
}

static void launch_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    Accounts *accounts = user_data;
    if (accounts->stopped || accounts->launching || !accounts->launch) return;
    g_autoptr(GtkWidget) owner = g_object_ref(accounts->widget);
    g_autofree char *place = g_strdup(gtk_editable_get_text(GTK_EDITABLE(accounts->place_id)));
    g_autofree char *job = g_strdup(gtk_editable_get_text(GTK_EDITABLE(accounts->job_id)));
    g_autoptr(GError) error = NULL;
    g_autofree char *uri = profile_join_uri(place, job, &error);
    set_message(accounts->join_error, error ? error->message : NULL);
    gtk_widget_set_tooltip_text(GTK_WIDGET(accounts->join_error), error ? error->message : NULL);
    if (error) return;
    /* Launch callbacks refresh the list synchronously. Keep a stable batch and
     * destination even when rows are reordered, hidden or removed mid-launch. */
    g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(accounts->list)); child;
         child = gtk_widget_get_next_sibling(child)) {
        AccountRow *row = g_object_get_data(G_OBJECT(child), "account-row");
        if (row && gtk_list_box_row_is_selected(GTK_LIST_BOX_ROW(child)) &&
            filter_row(GTK_LIST_BOX_ROW(child), accounts) && !row_running(row))
            g_ptr_array_add(names, g_strdup(row->name));
    }
    accounts->launching = TRUE;
    selection_sync(accounts);
    for (guint i = 0; i < names->len && !accounts->stopped; ++i) {
        const char *name = g_ptr_array_index(names, i);
        AccountRow *row = find_row(accounts, name);
        if (!row || row_running(row)) continue;
        accounts_set_launch_error(accounts->widget, name, NULL);
        row_set_message(row, "Launching…");
        g_autoptr(GError) failure = NULL;
        gboolean launched = accounts->launch(name, place, job, &failure, accounts->user_data);
        if (accounts->stopped) break;
        row = find_row(accounts, name);
        if (!row) continue;
        if (launched) {
            gtk_list_box_unselect_row(accounts->list, GTK_LIST_BOX_ROW(row->widget));
            row_set_message(row, NULL);
            row_sync(row, FALSE);
        } else accounts_set_launch_error(accounts->widget, name, failure ? failure->message : "Could not launch profile.");
    }
    accounts->launching = FALSE;
    selection_sync(accounts);
}

void accounts_set_launch_error(GtkWidget *widget, const char *name, const char *message)
{
    Accounts *accounts = g_object_get_data(G_OBJECT(widget), "accounts");
    if (accounts->stopped) return;
    AccountRow *row = find_row(accounts, name);
    if (!row) return;
    g_free(row->launch_error);
    row->launch_error = g_strdup(message);
    row_set_message(row, NULL);
}

static void join_changed(GtkEditable *editable, gpointer user_data)
{
    (void)editable;
    Accounts *accounts = user_data;
    set_message(accounts->join_error, NULL);
}

static GtkEntry *join_entry(Accounts *accounts, const char *label, const char *hint, int width)
{
    GtkEntry *entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(entry, label);
    gtk_editable_set_width_chars(GTK_EDITABLE(entry), width);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(entry), width);
    gtk_widget_set_tooltip_text(GTK_WIDGET(entry), hint);
    gtk_widget_add_css_class(GTK_WIDGET(entry), "accounts-join-entry");
    gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    g_signal_connect(entry, "changed", G_CALLBACK(join_changed), accounts);
    return entry;
}

static void load_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    AccountRow *row = user_data;
    open_dialog(row->accounts, LOAD_COOKIE, row->name);
}

static void rename_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    AccountRow *row = user_data;
    open_dialog(row->accounts, RENAME_PROFILE, row->name);
}

static void copy_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    AccountRow *row = user_data;
    if (row->accounts->stopped) return;
    g_autoptr(GError) error = NULL;
    g_autofree char *cookie = profile_cookie_read(row->name, &error);
    if (!cookie) {
        row_set_message(row, error ? error->message : "No cookie saved for this profile.");
        return;
    }
    gdk_clipboard_set_text(gtk_widget_get_clipboard(row->widget), cookie);
    row_set_message(row, "Cookie copied");
}

static void delete_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    AccountRow *row = user_data;
    if (!row->accounts->stopped && !row_running(row) && !row->request)
        open_dialog(row->accounts, DELETE_PROFILE, row->name);
}

static void free_row(gpointer user_data)
{
    AccountRow *row = user_data;
    if (row->request) g_cancellable_cancel(row->request);
    g_clear_object(&row->request);
    profile_account_free(row->account);
    g_free(row->cookie);
    g_free(row->name);
    g_free(row->launch_error);
    g_free(row);
}

static void row_sync(AccountRow *row, gboolean reload)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *cookie = profile_cookie_read(row->name, &error);
    gboolean changed = g_strcmp0(cookie, row->cookie) != 0;
    if (changed) {
        ++row->generation;
        if (row->request) g_cancellable_cancel(row->request);
    }
    if (reload || changed) {
        g_free(row->cookie);
        row->cookie = g_steal_pointer(&cookie);
        profile_account_free(row->account);
        row->account = profile_account_load(row->name, row->cookie);
        gtk_label_set_text(row->title, row->name);
        gtk_widget_set_tooltip_text(GTK_WIDGET(row->title), row->name);
        set_avatar(row->avatar, row->account);
        row_set_message(row, NULL);
    }
    g_autofree char *detail = row->account
        ? g_strdup_printf("%s · @%s", row->account->display_name, row->account->username)
        : g_strdup(!row->cookie ? "Logged out" : row->request ? "Checking account…" : "Needs refresh");
    gtk_label_set_text(row->detail, detail);
    gtk_widget_set_tooltip_text(GTK_WIDGET(row->detail), detail);
    if (error) row_set_message(row, error->message);
    gboolean running = row_running(row);
    row->is_running = running;
    gtk_widget_set_sensitive(GTK_WIDGET(row->select), !running);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row->widget), !running);
    gtk_widget_set_visible(row->running_label, running);
    if (running && gtk_widget_get_parent(row->widget) == GTK_WIDGET(row->accounts->list)) {
        gtk_list_box_unselect_row(row->accounts->list, GTK_LIST_BOX_ROW(row->widget));
        if (row->launch_error) accounts_set_launch_error(row->accounts->widget, row->name, NULL);
    }
    gtk_widget_set_sensitive(row->refresh, row->cookie && !row->request);
    gtk_widget_set_sensitive(row->load, !running && !row->request);
    gtk_widget_set_sensitive(row->rename, !running && !row->request);
    gtk_widget_set_sensitive(row->copy, row->cookie != NULL);
    gtk_widget_set_sensitive(row->delete, !running && !row->request);
    gtk_widget_set_tooltip_text(row->delete, running ? "Close the profile to delete it" : NULL);
    gtk_widget_set_tooltip_text(row->load, running ? "Close the profile to load a cookie" : "Load a cookie into this profile");
    if (running) gtk_widget_add_css_class(row->widget, "running");
    else gtk_widget_remove_css_class(row->widget, "running");
    gtk_list_box_row_changed(GTK_LIST_BOX_ROW(row->widget));
    selection_sync(row->accounts);
    if (!row->account) row_refresh(row, TRUE);
}

static GtkWidget *row_new(Accounts *accounts, const char *name)
{
    AccountRow *row = g_new0(AccountRow, 1);
    row->accounts = accounts;
    row->name = g_strdup(name);
    row->widget = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row->widget), FALSE);
    gtk_widget_add_css_class(row->widget, "account-row");
    g_object_set_data_full(G_OBJECT(row->widget), "account-row", row, free_row);
    GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    row->select = GTK_CHECK_BUTTON(gtk_check_button_new());
    gtk_widget_add_css_class(GTK_WIDGET(row->select), "account-select");
    gtk_widget_set_valign(GTK_WIDGET(row->select), GTK_ALIGN_CENTER);
    g_autofree char *select_label = g_strdup_printf("Select %s", name);
    gtk_accessible_update_property(GTK_ACCESSIBLE(row->select), GTK_ACCESSIBLE_PROPERTY_LABEL, select_label, -1);
    gtk_widget_set_tooltip_text(GTK_WIDGET(row->select), select_label);
    gtk_box_append(GTK_BOX(heading), GTK_WIDGET(row->select));
    row->avatar = GTK_IMAGE(gtk_image_new_from_icon_name("avatar-default-symbolic"));
    gtk_image_set_pixel_size(row->avatar, 24);
    gtk_widget_set_valign(GTK_WIDGET(row->avatar), GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(GTK_WIDGET(row->avatar), "account-avatar");
    gtk_box_append(GTK_BOX(heading), GTK_WIDGET(row->avatar));
    GtkWidget *identity = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(identity, TRUE);
    gtk_widget_set_valign(identity, GTK_ALIGN_CENTER);
    row->title = label_new(name, "account-name");
    gtk_label_set_max_width_chars(row->title, -1);
    row->detail = label_new("Logged out", "account-detail");
    gtk_label_set_max_width_chars(row->detail, -1);
    gtk_widget_set_hexpand(GTK_WIDGET(row->detail), FALSE);
    row->message = label_new(NULL, "account-detail");
    gtk_widget_set_visible(GTK_WIDGET(row->message), FALSE);
    GtkWidget *detail_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(detail_line), GTK_WIDGET(row->detail));
    gtk_box_append(GTK_BOX(detail_line), GTK_WIDGET(row->message));
    gtk_box_append(GTK_BOX(identity), GTK_WIDGET(row->title));
    gtk_box_append(GTK_BOX(identity), detail_line);
    gtk_box_append(GTK_BOX(heading), identity);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(buttons, GTK_ALIGN_CENTER);
    row->running_label = gtk_label_new("Running");
    gtk_widget_add_css_class(row->running_label, "account-running");
    GtkWidget *gear = gtk_image_new_from_resource("/org/relapse/RelapseGtk/icons/settings.svg");
    gtk_image_set_pixel_size(GTK_IMAGE(gear), 14);
    GtkWidget *settings = menu_new(gear, "Profile settings");
    gtk_widget_add_css_class(settings, "account-settings");
    row->rename = menu_item(settings, "Rename profile", G_CALLBACK(rename_clicked), row);
    row->load = menu_item(settings, "Load cookie", G_CALLBACK(load_clicked), row);
    row->copy = menu_item(settings, "Copy cookie", G_CALLBACK(copy_clicked), row);
    row->refresh = menu_item(settings, "Refresh", G_CALLBACK(refresh_clicked), row);
    GtkPopover *popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(settings));
    gtk_box_append(GTK_BOX(gtk_popover_get_child(popover)), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    row->delete = menu_item(settings, "Delete profile", G_CALLBACK(delete_clicked), row);
    gtk_widget_add_css_class(row->delete, "destructive-action");
    gtk_box_append(GTK_BOX(buttons), row->running_label);
    gtk_box_append(GTK_BOX(buttons), settings);
    gtk_box_append(GTK_BOX(heading), buttons);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row->widget), heading);
    g_signal_connect(row->select, "toggled", G_CALLBACK(select_toggled), row);
    row_sync(row, TRUE);
    return row->widget;
}

void accounts_update(GtkWidget *widget, const char *const *names)
{
    Accounts *accounts = g_object_get_data(G_OBJECT(widget), "accounts");
    if (accounts->stopped) return;
    gboolean was_syncing = accounts->syncing_selection;
    accounts->syncing_selection = TRUE;
    if (!accounts->names || !g_strv_equal((const char *const *)accounts->names, names)) {
        g_strfreev(accounts->names);
        accounts->names = g_strdupv((char **)names);
        for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(accounts->list)); child;) {
            GtkWidget *next = gtk_widget_get_next_sibling(child);
            AccountRow *row = g_object_get_data(G_OBJECT(child), "account-row");
            if (row && !g_strv_contains(names, row->name)) gtk_list_box_remove(accounts->list, child);
            child = next;
        }
        for (guint i = 0; names[i]; ++i) {
            AccountRow *row = find_row(accounts, names[i]);
            GtkWidget *existing = row ? row->widget : NULL;
            if (!existing) gtk_list_box_insert(accounts->list, row_new(accounts, names[i]), i);
            else if (existing != GTK_WIDGET(gtk_list_box_get_row_at_index(accounts->list, i))) {
                gboolean selected = gtk_list_box_row_is_selected(GTK_LIST_BOX_ROW(existing));
                g_object_ref(existing);
                gtk_list_box_remove(accounts->list, existing);
                gtk_list_box_insert(accounts->list, existing, i);
                if (selected) gtk_list_box_select_row(accounts->list, GTK_LIST_BOX_ROW(existing));
                g_object_unref(existing);
            }
        }
    }
    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(accounts->list)); child;
         child = gtk_widget_get_next_sibling(child)) {
        AccountRow *row = g_object_get_data(G_OBJECT(child), "account-row");
        if (row) row_sync(row, FALSE);
    }
    accounts->syncing_selection = was_syncing;
    selection_sync(accounts);
}

static void reload_accounts(Accounts *accounts)
{
    g_auto(GStrv) names = profiles_list();
    accounts_update(accounts->widget, (const char *const *)names);
    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(accounts->list)); child;
         child = gtk_widget_get_next_sibling(child)) {
        AccountRow *row = g_object_get_data(G_OBJECT(child), "account-row");
        if (row) row_sync(row, TRUE);
    }
}

static void dialog_stop(AccountDialog *dialog)
{
    dialog->stopped = TRUE;
    if (dialog->request) g_cancellable_cancel(dialog->request);
}

static gboolean dialog_closed(GtkWindow *window, gpointer user_data)
{
    (void)window;
    AccountDialog *dialog = user_data;
    dialog_stop(dialog);
    g_autoptr(GtkWidget) owner = g_weak_ref_get(&dialog->owner);
    Accounts *accounts = owner ? g_object_get_data(G_OBJECT(owner), "accounts") : NULL;
    if (accounts) accounts->dialog = NULL;
    return FALSE;
}

static void free_dialog(gpointer user_data)
{
    AccountDialog *dialog = user_data;
    dialog_stop(dialog);
    g_clear_object(&dialog->request);
    profile_account_free(dialog->account);
    g_weak_ref_clear(&dialog->owner);
    g_free(dialog->name);
    g_free(dialog);
}

static void cookie_changed(GtkEditable *editable, gpointer user_data)
{
    (void)editable;
    AccountDialog *dialog = user_data;
    ++dialog->generation;
    if (dialog->request) g_cancellable_cancel(dialog->request);
    g_clear_object(&dialog->request);
    g_clear_pointer(&dialog->account, profile_account_free);
    gtk_widget_set_visible(dialog->identity, FALSE);
    if (dialog->mode == ADD_ACCOUNT) gtk_widget_set_visible(dialog->name_field, FALSE);
    gtk_button_set_label(dialog->submit, "Check cookie");
    gtk_widget_set_sensitive(GTK_WIDGET(dialog->submit), TRUE);
    set_message(dialog->error, NULL);
}

static void dialog_checked(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)source;
    Lookup *lookup = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(ProfileAccount) account = profile_account_lookup_finish(result, &error);
    g_autoptr(GtkWindow) window = g_weak_ref_get(&lookup->owner);
    AccountDialog *dialog = window ? g_object_get_data(G_OBJECT(window), "account-dialog") : NULL;
    if (dialog && !dialog->stopped && dialog->generation == lookup->generation) {
        g_clear_object(&dialog->request);
        gtk_widget_set_sensitive(GTK_WIDGET(dialog->submit), TRUE);
        if (account) {
            dialog->account = g_steal_pointer(&account);
            gtk_label_set_text(dialog->title, dialog->account->display_name);
            g_autofree char *username = g_strconcat("@", dialog->account->username, NULL);
            gtk_label_set_text(dialog->username, username);
            set_avatar(dialog->avatar, dialog->account);
            gtk_widget_set_visible(dialog->identity, TRUE);
            if (dialog->mode == ADD_ACCOUNT) {
                gtk_editable_set_text(GTK_EDITABLE(dialog->name_entry), dialog->account->username);
                gtk_widget_set_visible(dialog->name_field, TRUE);
                gtk_widget_grab_focus(GTK_WIDGET(dialog->name_entry));
            }
            gtk_button_set_label(dialog->submit, dialog->mode == ADD_ACCOUNT ? "Add account" : "Load cookie");
            set_message(dialog->error, NULL);
        } else {
            gtk_button_set_label(dialog->submit, "Check cookie");
            set_message(dialog->error, error->message);
        }
    }
    free_lookup(lookup);
}

static void dialog_deleted(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)source;
    Lookup *lookup = user_data;
    g_autoptr(GError) error = NULL;
    gboolean deleted = profile_delete_finish(result, &error);
    g_autoptr(GtkWindow) window = g_weak_ref_get(&lookup->owner);
    AccountDialog *dialog = window ? g_object_get_data(G_OBJECT(window), "account-dialog") : NULL;
    if (dialog && !dialog->stopped) {
        g_clear_object(&dialog->request);
        g_autoptr(GtkWidget) owner = g_weak_ref_get(&dialog->owner);
        Accounts *accounts = owner ? g_object_get_data(G_OBJECT(owner), "accounts") : NULL;
        if (accounts && !accounts->stopped) {
            if (deleted) {
                reload_accounts(accounts);
                gtk_window_close(dialog->window);
            } else {
                gtk_button_set_label(dialog->submit, "Delete");
                gtk_widget_set_sensitive(GTK_WIDGET(dialog->submit), TRUE);
                set_message(dialog->error, error ? error->message : "Could not delete profile.");
            }
        }
    }
    free_lookup(lookup);
}

static void dialog_submit(GtkWidget *button, gpointer user_data)
{
    (void)button;
    AccountDialog *dialog = user_data;
    if (dialog->stopped || dialog->request) return;
    g_autoptr(GtkWidget) owner = g_weak_ref_get(&dialog->owner);
    Accounts *accounts = owner ? g_object_get_data(G_OBJECT(owner), "accounts") : NULL;
    if (!accounts || accounts->stopped) return;
    gboolean cookie_mode = dialog->mode == ADD_ACCOUNT || dialog->mode == LOAD_COOKIE;
    const char *cookie = cookie_mode ? gtk_editable_get_text(GTK_EDITABLE(dialog->cookie)) : NULL;
    if (cookie_mode && !dialog->account) {
        dialog->request = g_cancellable_new();
        gtk_widget_set_sensitive(GTK_WIDGET(dialog->submit), FALSE);
        gtk_button_set_label(dialog->submit, "Checking…");
        set_message(dialog->error, NULL);
        profile_account_lookup_async(cookie, dialog->request, dialog_checked,
            lookup_new(G_OBJECT(dialog->window), dialog->generation));
        return;
    }
    if (dialog->name && accounts->running && accounts->running(dialog->name, accounts->user_data)) {
        set_message(dialog->error, "Close this profile before changing it.");
        return;
    }
    if (dialog->mode == DELETE_PROFILE) {
        dialog->request = g_cancellable_new();
        gtk_widget_set_sensitive(GTK_WIDGET(dialog->submit), FALSE);
        gtk_button_set_label(dialog->submit, "Deleting…");
        set_message(dialog->error, NULL);
        profile_delete_async(dialog->name, dialog->request, dialog_deleted,
            lookup_new(G_OBJECT(dialog->window), 0));
        return;
    }
    const char *name = gtk_editable_get_text(GTK_EDITABLE(dialog->name_entry));
    g_autoptr(GError) error = NULL;
    gboolean ok;
    if (dialog->mode == NEW_PROFILE) ok = profile_create(name, &error);
    else if (dialog->mode == RENAME_PROFILE) ok = profile_rename(dialog->name, name, &error);
    else ok = profile_account_import(dialog->mode == LOAD_COOKIE ? dialog->name : name,
        cookie, dialog->account, dialog->mode == ADD_ACCOUNT, &error);
    if (!ok) { set_message(dialog->error, error->message); return; }
    reload_accounts(accounts);
    gtk_window_close(dialog->window);
}

static void open_dialog(Accounts *accounts, DialogMode mode, const char *name)
{
    if (accounts->stopped) return;
    if (accounts->dialog) { gtk_window_present(accounts->dialog); return; }
    AccountDialog *dialog = g_new0(AccountDialog, 1);
    dialog->mode = mode;
    dialog->name = g_strdup(name);
    g_weak_ref_init(&dialog->owner, accounts->widget);
    dialog->window = GTK_WINDOW(gtk_window_new());
    accounts->dialog = dialog->window;
    g_object_set_data_full(G_OBJECT(dialog->window), "account-dialog", dialog, free_dialog);
    const char *title = mode == ADD_ACCOUNT ? "Add account" : mode == LOAD_COOKIE ? "Load cookie"
        : mode == NEW_PROFILE ? "New profile" : mode == DELETE_PROFILE ? "Delete profile" : "Rename profile";
    gtk_window_set_title(dialog->window, title);
    gtk_window_set_transient_for(dialog->window, GTK_WINDOW(gtk_widget_get_root(accounts->widget)));
    gtk_window_set_modal(dialog->window, TRUE);
    gtk_window_set_resizable(dialog->window, FALSE);
    gtk_window_set_default_size(dialog->window, 380, -1);
    gtk_widget_add_css_class(GTK_WIDGET(dialog->window), "relapse-ui");
    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *heading = gtk_label_new(title);
    gtk_widget_add_css_class(heading, "title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), heading);
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), FALSE);
    gtk_window_set_titlebar(dialog->window, header);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(box, "account-dialog");
    if (mode == DELETE_PROFILE) {
        g_autofree char *question = g_strdup_printf("Delete profile “%s”?", name);
        GtkWidget *message = gtk_label_new(question);
        gtk_label_set_xalign(GTK_LABEL(message), 0);
        gtk_label_set_wrap(GTK_LABEL(message), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(message), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(message), 44);
        gtk_widget_add_css_class(message, "account-name");
        gtk_box_append(GTK_BOX(box), message);
        GtkWidget *detail = gtk_label_new("Its saved login and local data will be moved to Trash.");
        gtk_label_set_xalign(GTK_LABEL(detail), 0);
        gtk_label_set_wrap(GTK_LABEL(detail), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(detail), 44);
        gtk_widget_add_css_class(detail, "account-detail");
        gtk_box_append(GTK_BOX(box), detail);
    }
    gboolean cookie_mode = mode == ADD_ACCOUNT || mode == LOAD_COOKIE;
    if (cookie_mode) {
        if (name) {
            g_autofree char *caption = g_strdup_printf("Profile: %s", name);
            gtk_box_append(GTK_BOX(box), GTK_WIDGET(label_new(caption, "account-detail")));
        }
        gtk_box_append(GTK_BOX(box), GTK_WIDGET(label_new("Cookie", NULL)));
        dialog->cookie = gtk_password_entry_new();
        gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(dialog->cookie), TRUE);
        gtk_accessible_update_property(GTK_ACCESSIBLE(dialog->cookie), GTK_ACCESSIBLE_PROPERTY_LABEL, ".ROBLOSECURITY cookie", -1);
        g_object_set(dialog->cookie, "placeholder-text", "Paste a .ROBLOSECURITY cookie", NULL);
        gtk_box_append(GTK_BOX(box), dialog->cookie);
        dialog->identity = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        dialog->avatar = GTK_IMAGE(gtk_image_new());
        gtk_image_set_pixel_size(dialog->avatar, 40);
        GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        dialog->title = label_new(NULL, "account-name");
        dialog->username = label_new(NULL, "account-detail");
        gtk_box_append(GTK_BOX(labels), GTK_WIDGET(dialog->title));
        gtk_box_append(GTK_BOX(labels), GTK_WIDGET(dialog->username));
        gtk_box_append(GTK_BOX(dialog->identity), GTK_WIDGET(dialog->avatar));
        gtk_box_append(GTK_BOX(dialog->identity), labels);
        gtk_widget_set_visible(dialog->identity, FALSE);
        gtk_box_append(GTK_BOX(box), dialog->identity);
    }
    dialog->name_field = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(dialog->name_field), GTK_WIDGET(label_new("Profile name", NULL)));
    dialog->name_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_max_length(dialog->name_entry, 64);
    gtk_entry_set_activates_default(dialog->name_entry, TRUE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(dialog->name_entry), GTK_ACCESSIBLE_PROPERTY_LABEL, "Profile name", -1);
    gtk_editable_set_text(GTK_EDITABLE(dialog->name_entry), name ? name : "");
    gtk_box_append(GTK_BOX(dialog->name_field), GTK_WIDGET(dialog->name_entry));
    gtk_widget_set_visible(dialog->name_field, !cookie_mode && mode != DELETE_PROFILE);
    gtk_box_append(GTK_BOX(box), dialog->name_field);
    dialog->error = label_new(NULL, "script-rename-error");
    gtk_label_set_ellipsize(dialog->error, PANGO_ELLIPSIZE_NONE);
    gtk_label_set_wrap(dialog->error, TRUE);
    gtk_label_set_max_width_chars(dialog->error, 44);
    gtk_widget_set_visible(GTK_WIDGET(dialog->error), FALSE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(dialog->error));
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    dialog->submit = GTK_BUTTON(gtk_button_new_with_label(cookie_mode ? "Check cookie" : mode == NEW_PROFILE ? "Create"
        : mode == DELETE_PROFILE ? "Delete" : "Rename"));
    if (mode == DELETE_PROFILE) gtk_widget_add_css_class(GTK_WIDGET(dialog->submit), "destructive-action");
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), GTK_WIDGET(dialog->submit));
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_window_set_default_widget(dialog->window, mode == DELETE_PROFILE ? cancel : GTK_WIDGET(dialog->submit));
    gtk_window_set_child(dialog->window, box);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_close), dialog->window);
    g_signal_connect(dialog->submit, "clicked", G_CALLBACK(dialog_submit), dialog);
    g_signal_connect(dialog->window, "close-request", G_CALLBACK(dialog_closed), dialog);
    if (cookie_mode) {
        g_signal_connect(dialog->cookie, "changed", G_CALLBACK(cookie_changed), dialog);
        g_signal_connect(dialog->cookie, "activate", G_CALLBACK(dialog_submit), dialog);
    }
    gtk_window_present(dialog->window);
    gtk_widget_grab_focus(mode == DELETE_PROFILE ? cancel : cookie_mode ? dialog->cookie : GTK_WIDGET(dialog->name_entry));
}

static void add_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    open_dialog(user_data, ADD_ACCOUNT, NULL);
}

static void new_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    open_dialog(user_data, NEW_PROFILE, NULL);
}

void accounts_shutdown(GtkWidget *widget)
{
    Accounts *accounts = g_object_get_data(G_OBJECT(widget), "accounts");
    if (accounts->stopped) return;
    accounts->stopped = TRUE;
    if (accounts->dialog) {
        AccountDialog *dialog = g_object_get_data(G_OBJECT(accounts->dialog), "account-dialog");
        dialog_stop(dialog);
        gtk_window_destroy(accounts->dialog);
        accounts->dialog = NULL;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(accounts->list)); child;
         child = gtk_widget_get_next_sibling(child)) {
        AccountRow *row = g_object_get_data(G_OBJECT(child), "account-row");
        if (row && row->request) g_cancellable_cancel(row->request);
    }
}

static void free_accounts(gpointer user_data)
{
    Accounts *accounts = user_data;
    g_strfreev(accounts->names);
    g_free(accounts->query);
    g_hash_table_unref(accounts->refresh_attempts);
    g_free(accounts);
}

GtkWidget *accounts_new(ProfileLaunchFunc launch, ProfileRunningFunc running, gpointer user_data)
{
    Accounts *accounts = g_new0(Accounts, 1);
    accounts->refresh_attempts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    accounts->launch = launch;
    accounts->running = running;
    accounts->user_data = user_data;
    accounts->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(accounts->widget, "accounts-page");
    g_object_set_data_full(G_OBJECT(accounts->widget), "accounts", accounts, free_accounts);
    GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(heading), GTK_WIDGET(label_new("Accounts", "accounts-title")));
    accounts->search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(accounts->search, "Search profiles or accounts…");
    gtk_editable_set_width_chars(GTK_EDITABLE(accounts->search), 24);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(accounts->search), 24);
    gtk_widget_add_css_class(GTK_WIDGET(accounts->search), "accounts-search");
    gtk_accessible_update_property(GTK_ACCESSIBLE(accounts->search), GTK_ACCESSIBLE_PROPERTY_LABEL,
        "Search profiles and accounts", -1);
    const char *filters[] = {"All profiles", "With account", "Logged out", "Running", "Not running", NULL};
    accounts->filter = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(filters));
    gtk_drop_down_set_enable_search(accounts->filter, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(accounts->filter), "accounts-filter");
    gtk_accessible_update_property(GTK_ACCESSIBLE(accounts->filter), GTK_ACCESSIBLE_PROPERTY_LABEL,
        "Filter profiles", -1);
    gtk_box_append(GTK_BOX(heading), GTK_WIDGET(accounts->search));
    gtk_box_append(GTK_BOX(heading), GTK_WIDGET(accounts->filter));
    GtkWidget *refresh = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(refresh), button_content("view-refresh-symbolic", "Refresh all"));
    gtk_widget_set_tooltip_text(refresh, "Refresh all");
    gtk_widget_add_css_class(refresh, "accounts-refresh");
    GtkWidget *add = menu_new(button_content("list-add-symbolic", "Add"), "Add profile");
    gtk_widget_add_css_class(add, "accounts-add");
    menu_item(add, "Add account", G_CALLBACK(add_clicked), accounts);
    menu_item(add, "New profile", G_CALLBACK(new_clicked), accounts);
    gtk_box_append(GTK_BOX(heading), refresh);
    gtk_box_append(GTK_BOX(heading), add);
    gtk_box_append(GTK_BOX(accounts->widget), heading);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 100);
    gtk_widget_set_vexpand(scroll, TRUE);
    accounts->list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(accounts->list, GTK_SELECTION_MULTIPLE);
    gtk_list_box_set_activate_on_single_click(accounts->list, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(accounts->list), "accounts-list");
    accounts->empty = GTK_LABEL(gtk_label_new("Add an account or create a profile."));
    gtk_label_set_wrap(accounts->empty, TRUE);
    gtk_label_set_max_width_chars(accounts->empty, 22);
    gtk_widget_add_css_class(GTK_WIDGET(accounts->empty), "account-detail");
    gtk_list_box_set_placeholder(accounts->list, GTK_WIDGET(accounts->empty));
    gtk_list_box_set_filter_func(accounts->list, filter_row, accounts, NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(accounts->list));
    gtk_box_append(GTK_BOX(accounts->widget), scroll);
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(footer, "accounts-actions");
    accounts->join_error = label_new(NULL, "script-rename-error");
    gtk_label_set_max_width_chars(accounts->join_error, -1);
    gtk_widget_set_visible(GTK_WIDGET(accounts->join_error), FALSE);
    gtk_box_append(GTK_BOX(footer), GTK_WIDGET(accounts->join_error));
    GtkWidget *join = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    accounts->selection = GTK_LABEL(gtk_label_new("Select accounts"));
    gtk_label_set_xalign(accounts->selection, 0);
    gtk_label_set_width_chars(accounts->selection, 13);
    gtk_widget_add_css_class(GTK_WIDGET(accounts->selection), "account-detail");
    gtk_box_append(GTK_BOX(join), GTK_WIDGET(accounts->selection));
    accounts->clear_selection = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(accounts->clear_selection, "accounts-clear-selection");
    gtk_widget_set_tooltip_text(accounts->clear_selection, "Clear selection");
    gtk_accessible_update_property(GTK_ACCESSIBLE(accounts->clear_selection), GTK_ACCESSIBLE_PROPERTY_LABEL, "Clear selection", -1);
    gtk_box_append(GTK_BOX(join), accounts->clear_selection);
    accounts->place_id = join_entry(accounts, "Place ID", "Leave blank to launch normally.", 14);
    gtk_entry_set_input_purpose(accounts->place_id, GTK_INPUT_PURPOSE_DIGITS);
    accounts->job_id = join_entry(accounts, "Job ID (optional)", "Join a specific server in this place.", 36);
    gtk_box_append(GTK_BOX(join), GTK_WIDGET(accounts->place_id));
    gtk_box_append(GTK_BOX(join), GTK_WIDGET(accounts->job_id));
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(join), spacer);
    accounts->launch_button = gtk_button_new();
    GtkWidget *launch_content = button_content("media-playback-start-symbolic", "Launch");
    accounts->launch_label = GTK_LABEL(gtk_widget_get_last_child(launch_content));
    gtk_button_set_child(GTK_BUTTON(accounts->launch_button), launch_content);
    gtk_widget_add_css_class(accounts->launch_button, "accounts-launch");
    gtk_widget_set_tooltip_text(accounts->launch_button, "Launch selected accounts");
    gtk_box_append(GTK_BOX(join), accounts->launch_button);
    gtk_box_append(GTK_BOX(footer), join);
    gtk_box_append(GTK_BOX(accounts->widget), footer);
    g_signal_connect(accounts->list, "selected-rows-changed", G_CALLBACK(selected_rows_changed), accounts);
    g_signal_connect(accounts->clear_selection, "clicked", G_CALLBACK(clear_selection_clicked), accounts);
    g_signal_connect(accounts->launch_button, "clicked", G_CALLBACK(launch_clicked), accounts);
    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_all_clicked), accounts);
    g_signal_connect(accounts->search, "changed", G_CALLBACK(search_changed), accounts);
    g_signal_connect(accounts->filter, "notify::selected", G_CALLBACK(filter_changed), accounts);
    selection_sync(accounts);
    return accounts->widget;
}
