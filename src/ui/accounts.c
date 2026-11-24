#include "accounts.h"
#include "configuration.h"
#include "profiles/account.h"
#include "profiles/store.h"
#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

typedef enum {
    FILTER_ALL, FILTER_ACCOUNT, FILTER_LOGGED_OUT, FILTER_RUNNING, FILTER_STOPPED
} AccountFilter;

typedef struct {
    GtkWidget *widget;
    GtkListBox *list;
    GtkEntry *search;
    GtkWidget *filter_menu;
    GtkDropDown *filter;
    GtkWidget *manual;
    GtkWidget *captured;
    GtkEntry *place_entry;
    GtkEntry *job_entry;
    GtkLabel *target_error;
    GtkLabel *target_name;
    GtkLabel *target_game;
    GtkImage *target_avatar;
    GtkLabel *target_initial;
    char *captured_place;
    char *captured_job;
    GtkWidget *clear_selection;
    guint pending_save;
    GtkWidget *launch_button;
    GtkLabel *launch_label;
    GtkLabel *empty;
    GtkWindow *dialog;
    GtkWindow *configuration;
    char **names;
    char **pending_names;
    char *query;
    GHashTable *refresh_attempts;
    gboolean stopped;
    gboolean syncing_selection;
    gboolean launching;
    ProfileLaunchFunc launch;
    ProfileRunningFunc running;
    ProfileSessionFunc session;
    gpointer user_data;
} Accounts;

typedef struct {
    Accounts *accounts;
    GtkWidget *widget;
    char *name;
    char *cookie;
    ProfileAccount *account;
    GtkImage *avatar;
    GtkLabel *initial;
    GtkLabel *title;
    GtkLabel *detail;
    GtkLabel *message;
    GtkCheckButton *select;
    GtkWidget *live_dot;
    GtkLabel *session_label;
    GtkWidget *actions;
    GtkWidget *launch_one;
    GtkWidget *capture;
    char *launch_error;
    GtkWidget *load;
    GtkWidget *configure;
    GtkWidget *rename;
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
static void launch_profiles(Accounts *accounts, const char *only);
static void target_save_later(Accounts *accounts);
static void copy_clicked(GtkButton *button, gpointer user_data);
static void delete_clicked(GtkButton *button, gpointer user_data);

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
    gtk_menu_button_set_label(GTK_MENU_BUTTON(accounts->filter_menu), filtered ? "Filtered" : "Filter");
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
    g_autofree char *launch = g_strdup_printf("Launch %u", count);
    gtk_label_set_text(accounts->launch_label, launch);
    gtk_widget_set_visible(accounts->clear_selection, count > 0);
    gtk_widget_set_visible(accounts->launch_button, count > 0);
    gtk_widget_set_sensitive(accounts->launch_button, !accounts->launching && accounts->launch != NULL);
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

static void launch_selected(GtkButton *button, Accounts *accounts)
{
    (void)button;
    launch_profiles(accounts, NULL);
}

static void launch_one(GtkButton *button, AccountRow *row)
{
    (void)button;
    launch_profiles(row->accounts, row->name);
}

static void clear_selected(GtkButton *button, Accounts *accounts)
{
    (void)button;
    gtk_list_box_unselect_all(accounts->list);
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
    gtk_widget_set_sensitive(row->load, FALSE);
    gtk_widget_set_sensitive(row->rename, FALSE);
    gtk_widget_set_sensitive(row->configure, FALSE);
    profile_account_lookup_async(row->cookie, row->request, row_checked,
        lookup_new(G_OBJECT(row->widget), row->generation));
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
        if (row && !row->request) {
            row_sync(row, FALSE);
            row_refresh(row, FALSE);
        }
    }
}

static void launch_profiles(Accounts *accounts, const char *only)
{
    if (accounts->stopped || accounts->launching || !accounts->launch) return;
    g_autoptr(GtkWidget) owner = g_object_ref(accounts->widget);
    g_autofree char *place = g_strdup(accounts->captured_place ? accounts->captured_place :
        gtk_editable_get_text(GTK_EDITABLE(accounts->place_entry)));
    g_autofree char *job = g_strdup(accounts->captured_job ? accounts->captured_job :
        gtk_editable_get_text(GTK_EDITABLE(accounts->job_entry)));
    g_strstrip(place);
    g_strstrip(job);
    g_autoptr(GError) error = NULL;
    g_autofree char *uri = profile_join_uri(place, job, &error);
    set_message(accounts->target_error, error ? error->message : NULL);
    if (error) return;
    if (*place == '0') {
        char *canonical = g_strdup_printf("%" G_GUINT64_FORMAT, g_ascii_strtoull(place, NULL, 10));
        g_free(place);
        place = canonical;
    }
    /* Launch callbacks refresh the list synchronously. Keep a stable batch and
     * destination even when rows are reordered, hidden or removed mid-launch. */
    g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(accounts->list)); child;
         child = gtk_widget_get_next_sibling(child)) {
        AccountRow *row = g_object_get_data(G_OBJECT(child), "account-row");
        if (row && (only ? g_str_equal(only, row->name) : gtk_list_box_row_is_selected(GTK_LIST_BOX_ROW(child))) &&
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
    if (!accounts->stopped) selection_sync(accounts);
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

static const ProfileJoinTarget *row_target(AccountRow *row, gboolean running)
{
    Accounts *accounts = row->accounts;
    const ProfileJoinTarget *target = running && accounts->session
        ? accounts->session(row->name, accounts->user_data) : NULL;
    return target && target->place && target->job ? target : NULL;
}

static gboolean set_place_label(GtkLabel *label, const char *place, const char *title)
{
    g_autofree char *fallback = NULL;
    if (!title || !*title) {
        fallback = g_strdup_printf("Place %s", place);
        title = fallback;
    }
    if (g_str_equal(gtk_label_get_text(label), title)) return FALSE;
    gtk_label_set_text(label, title);
    gtk_widget_set_tooltip_text(GTK_WIDGET(label), title);
    return TRUE;
}

static void show_target(Accounts *accounts)
{
    gboolean active = accounts->captured_place != NULL;
    gtk_widget_set_visible(accounts->manual, !active);
    gtk_widget_set_visible(accounts->captured, active);
    set_message(accounts->target_error, NULL);
}

static void target_portrait(Accounts *accounts, AccountRow *row)
{
    GdkPaintable *avatar = row && gtk_image_get_storage_type(row->avatar) == GTK_IMAGE_PAINTABLE
        ? gtk_image_get_paintable(row->avatar) : NULL;
    gtk_image_set_from_paintable(accounts->target_avatar, avatar);
    gtk_widget_set_visible(GTK_WIDGET(accounts->target_avatar), avatar != NULL);
    gtk_widget_set_visible(GTK_WIDGET(accounts->target_initial), avatar == NULL);
    const char *name = gtk_label_get_text(accounts->target_name);
    g_autofree char *initial = g_ascii_strup(name, 1);
    gtk_label_set_text(accounts->target_initial, initial);
}

static void capture_target(GtkButton *button, AccountRow *row)
{
    (void)button;
    Accounts *accounts = row->accounts;
    if (accounts->stopped) return;
    const ProfileJoinTarget *target = row_target(row, row_running(row));
    if (!target) {
        set_message(accounts->target_error, "Waiting for this profile to connect to a game.");
        return;
    }
    /* Own both identifiers. Row refresh, rename, exit or removal must never
     * redirect a subsequent launch to a different server. */
    g_set_str(&accounts->captured_place, target->place);
    g_set_str(&accounts->captured_job, target->job);
    gtk_label_set_text(accounts->target_name, row->name);
    set_place_label(accounts->target_game, target->place, target->title);
    target_portrait(accounts, row);
    show_target(accounts);
    target_save_later(accounts);
}

static void use_manual_target(GtkButton *button, Accounts *accounts)
{
    (void)button;
    g_clear_pointer(&accounts->captured_place, g_free);
    g_clear_pointer(&accounts->captured_job, g_free);
    gtk_image_clear(accounts->target_avatar);
    show_target(accounts);
    gtk_widget_grab_focus(GTK_WIDGET(accounts->place_entry));
    target_save_later(accounts);
}

static void reset_menu_anchor(GtkPopover *popover, gpointer data)
{
    (void)data;
    gtk_popover_set_pointing_to(popover, NULL);
    gtk_widget_set_halign(GTK_WIDGET(popover), GTK_ALIGN_END);
}

static void row_context(GtkGestureClick *click, int presses, double x, double y, AccountRow *row)
{
    if (presses != 1 || row->accounts->stopped) return;
    GtkMenuButton *button = GTK_MENU_BUTTON(row->actions);
    GtkPopover *popover = gtk_menu_button_get_popover(button);
    gtk_widget_set_halign(GTK_WIDGET(popover), GTK_ALIGN_START);
    graphene_point_t point = GRAPHENE_POINT_INIT(x, y), local;
    if (gtk_widget_compute_point(row->widget, gtk_widget_get_parent(GTK_WIDGET(popover)), &point, &local)) {
        GdkRectangle anchor = {(int)local.x, (int)local.y, 1, 1};
        gtk_popover_set_pointing_to(popover, &anchor);
    }
    gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_menu_button_popup(button);
}

static void update_row_session(AccountRow *row)
{
    Accounts *accounts = row->accounts;
    const ProfileJoinTarget *target = row_target(row, row->is_running);
    if (target)
        set_place_label(row->session_label, target->place, target->title);
    else
        set_place_label(row->session_label, NULL, row->is_running ? "Waiting for game" : "—");
    const char *hint = target ? "Use this profile’s current place and server"
        : "Waiting for this profile to connect to a game";
    if (g_strcmp0(gtk_widget_get_tooltip_text(row->capture), hint) != 0)
        gtk_widget_set_tooltip_text(row->capture, hint);

    if (target && target->title && *target->title && g_strcmp0(accounts->captured_place, target->place) == 0 &&
        set_place_label(accounts->target_game, target->place, target->title))
        target_save_later(accounts);
    if (accounts->captured_place && !gtk_image_get_paintable(accounts->target_avatar) &&
        g_str_equal(row->name, gtk_label_get_text(accounts->target_name))) target_portrait(accounts, row);
}

static char *target_state_path(void)
{
    return g_build_filename(g_get_user_state_dir(), "relapsegtk", "join-target.ini", NULL);
}

static gboolean save_target(gpointer data)
{
    Accounts *accounts = data;
    accounts->pending_save = 0;
    g_autoptr(GKeyFile) file = g_key_file_new();
    const char *section = "Join";
    g_key_file_set_string(file, section, "manual-place", gtk_editable_get_text(GTK_EDITABLE(accounts->place_entry)));
    g_key_file_set_string(file, section, "manual-job", gtk_editable_get_text(GTK_EDITABLE(accounts->job_entry)));
    if (accounts->captured_place) {
        g_key_file_set_string(file, section, "place", accounts->captured_place);
        g_key_file_set_string(file, section, "job", accounts->captured_job);
        g_key_file_set_string(file, section, "profile", gtk_label_get_text(accounts->target_name));
        g_key_file_set_string(file, section, "game", gtk_label_get_text(accounts->target_game));
    }
    g_autofree char *path = target_state_path();
    g_autofree char *directory = g_path_get_dirname(path);
    g_autoptr(GError) error = NULL;
    if (g_mkdir_with_parents(directory, 0700) == 0) {
        gsize length;
        g_autofree char *contents = g_key_file_to_data(file, &length, NULL);
        g_file_set_contents_full(path, contents, length, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error);
    } else {
        g_set_error(&error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Could not save join target: %s", g_strerror(errno));
    }
    if (error) set_message(accounts->target_error, error->message);
    return G_SOURCE_REMOVE;
}

static void target_save_later(Accounts *accounts)
{
    if (accounts->pending_save) g_source_remove(accounts->pending_save);
    accounts->pending_save = g_timeout_add(350, save_target, accounts);
}

static void manual_changed(GtkEditable *entry, Accounts *accounts)
{
    (void)entry;
    set_message(accounts->target_error, NULL);
    target_save_later(accounts);
}

static void restore_target(Accounts *accounts)
{
    const char *section = "Join";
    g_autofree char *path = target_state_path();
    g_autoptr(GKeyFile) file = g_key_file_new();
    if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL)) {
        g_autofree char *manual_place = g_key_file_get_string(file, section, "manual-place", NULL);
        g_autofree char *manual_job = g_key_file_get_string(file, section, "manual-job", NULL);
        gtk_editable_set_text(GTK_EDITABLE(accounts->place_entry), manual_place ? manual_place : "");
        gtk_editable_set_text(GTK_EDITABLE(accounts->job_entry), manual_job ? manual_job : "");
        g_autofree char *place = g_key_file_get_string(file, section, "place", NULL);
        g_autofree char *job = g_key_file_get_string(file, section, "job", NULL);
        g_autofree char *name = g_key_file_get_string(file, section, "profile", NULL);
        g_autofree char *game = g_key_file_get_string(file, section, "game", NULL);
        g_autofree char *uri = profile_join_uri(place, job, NULL);
        if (uri && job && *job && name && *name) {
            accounts->captured_place = g_steal_pointer(&place);
            accounts->captured_job = g_steal_pointer(&job);
            gtk_label_set_text(accounts->target_name, name);
            set_place_label(accounts->target_game, accounts->captured_place, game);
            target_portrait(accounts, NULL);
        }
    }
    show_target(accounts);
    g_signal_connect(accounts->place_entry, "changed", G_CALLBACK(manual_changed), accounts);
    g_signal_connect(accounts->job_entry, "changed", G_CALLBACK(manual_changed), accounts);
}

static void configuration_closed(GtkWindow *window, gpointer user_data)
{
    (void)window;
    Accounts *accounts = user_data;
    accounts->configuration = NULL;
}

static void configuration_copy_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *menu = user_data;
    Accounts *accounts = g_object_get_data(G_OBJECT(menu), "accounts");
    const char *name = g_object_get_data(G_OBJECT(menu), "profile-name");
    AccountRow *row = find_row(accounts, name);
    if (row) copy_clicked(button, row);
}

static void configuration_delete_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkWidget *menu = user_data;
    Accounts *accounts = g_object_get_data(G_OBJECT(menu), "accounts");
    g_autofree char *name = g_strdup(g_object_get_data(G_OBJECT(menu), "profile-name"));
    if (accounts->configuration) gtk_window_close(accounts->configuration);
    AccountRow *row = find_row(accounts, name);
    if (row) delete_clicked(NULL, row);
}

static void open_configuration(Accounts *accounts, const char *name)
{
    if (accounts->stopped) return;
    if (accounts->configuration) {
        gtk_window_present(accounts->configuration);
        return;
    }
    g_autoptr(GtkWidget) actions = g_object_ref_sink(menu_new(
        gtk_image_new_from_icon_name("view-more-symbolic"), "More profile actions"));
    gtk_widget_add_css_class(actions, "configuration-menu");
    g_object_set_data(G_OBJECT(actions), "accounts", accounts);
    g_object_set_data_full(G_OBJECT(actions), "profile-name", g_strdup(name), g_free);
    GtkWidget *copy = menu_item(actions, "Copy cookie", G_CALLBACK(configuration_copy_clicked), actions);
    AccountRow *row = find_row(accounts, name);
    gtk_widget_set_sensitive(copy, row && row->cookie);
    GtkWidget *remove = menu_item(actions, "Delete profile…", G_CALLBACK(configuration_delete_clicked), actions);
    gtk_widget_add_css_class(remove, "destructive-action");
    accounts->configuration = configuration_dialog_new(
        GTK_WINDOW(gtk_widget_get_root(accounts->widget)), name, actions);
    if (accounts->configuration)
        g_signal_connect(accounts->configuration, "destroy", G_CALLBACK(configuration_closed), accounts);
}

static void settings_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    AccountRow *row = user_data;
    if (!row_running(row) && !row->request) open_configuration(row->accounts, row->name);
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
        gboolean photo = gtk_image_get_storage_type(row->avatar) == GTK_IMAGE_PAINTABLE;
        gtk_widget_set_visible(GTK_WIDGET(row->avatar), photo);
        gtk_widget_set_visible(GTK_WIDGET(row->initial), !photo);
        row_set_message(row, NULL);
    }
    const char *username = row->account ? row->account->username : NULL;
    g_autofree char *detail = username ? g_strdup_printf("@%s", username)
        : g_strdup(!row->cookie ? "No account linked" : row->request ? "Checking account…" : "Needs refresh");
    gtk_label_set_text(row->detail, detail);
    gtk_widget_set_tooltip_text(GTK_WIDGET(row->detail), detail);
    if (error) row_set_message(row, error->message);
    gboolean running = row_running(row);
    gboolean running_changed = running != row->is_running;
    row->is_running = running;
    gtk_widget_set_sensitive(GTK_WIDGET(row->select), !running);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row->widget), !running);
    gtk_widget_set_visible(GTK_WIDGET(row->select), !running);
    gtk_widget_set_visible(row->live_dot, running);
    gtk_widget_set_visible(row->launch_one, !running);
    gtk_widget_set_visible(row->capture, running);
    gtk_widget_set_visible(row->load, !running);
    gtk_widget_set_visible(row->configure, !running);
    gtk_widget_set_visible(row->rename, !running);
    if (running && gtk_widget_get_parent(row->widget) == GTK_WIDGET(row->accounts->list)) {
        gtk_list_box_unselect_row(row->accounts->list, GTK_LIST_BOX_ROW(row->widget));
        if (row->launch_error) accounts_set_launch_error(row->accounts->widget, row->name, NULL);
    }
    gtk_widget_set_sensitive(row->load, !running && !row->request);
    gtk_widget_set_sensitive(row->configure, !running && !row->request);
    gtk_widget_set_sensitive(row->rename, !running && !row->request);
    gtk_widget_set_tooltip_text(row->load, running ? "Close the profile to load a cookie" : "Load a cookie into this profile");
    gtk_widget_set_tooltip_text(row->configure, running ? "Close the profile to change its settings" : "Profile settings");
    if (running) gtk_widget_add_css_class(row->widget, "running");
    else gtk_widget_remove_css_class(row->widget, "running");
    if (running_changed || changed || reload)
        gtk_list_box_row_changed(GTK_LIST_BOX_ROW(row->widget));
    update_row_session(row);
    selection_sync(row->accounts);
    if (!row->account) row_refresh(row, TRUE);
}

static GtkWidget *roster_columns(GtkWidget *parent)
{
    GtkWidget *columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(columns), TRUE);
    gtk_widget_set_hexpand(columns, TRUE);
    gtk_box_append(GTK_BOX(parent), columns);
    return columns;
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
    GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *columns = roster_columns(heading);
    GtkWidget *identity = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_box_append(GTK_BOX(columns), identity);
    row->select = GTK_CHECK_BUTTON(gtk_check_button_new());
    gtk_widget_add_css_class(GTK_WIDGET(row->select), "account-select");
    gtk_widget_set_valign(GTK_WIDGET(row->select), GTK_ALIGN_CENTER);
    g_autofree char *select_label = g_strdup_printf("Select %s", name);
    gtk_accessible_update_property(GTK_ACCESSIBLE(row->select), GTK_ACCESSIBLE_PROPERTY_LABEL, select_label, -1);
    gtk_widget_set_tooltip_text(GTK_WIDGET(row->select), select_label);
    GtkWidget *marker = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(marker, 18, -1);
    gtk_box_append(GTK_BOX(marker), GTK_WIDGET(row->select));
    row->live_dot = gtk_label_new("●");
    gtk_widget_add_css_class(row->live_dot, "roster-dot");
    gtk_box_append(GTK_BOX(identity), marker);
    row->avatar = GTK_IMAGE(gtk_image_new_from_icon_name("avatar-default-symbolic"));
    gtk_image_set_pixel_size(row->avatar, 20);
    gtk_widget_set_valign(GTK_WIDGET(row->avatar), GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(GTK_WIDGET(row->avatar), "account-avatar");
    g_autofree char *initial = g_ascii_strup(name, 1);
    row->initial = GTK_LABEL(gtk_label_new(initial));
    gtk_widget_add_css_class(GTK_WIDGET(row->initial), "roster-initial");
    gtk_widget_set_valign(GTK_WIDGET(row->initial), GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(identity), GTK_WIDGET(row->initial));
    gtk_box_append(GTK_BOX(identity), GTK_WIDGET(row->avatar));
    row->title = label_new(name, "account-name");
    gtk_box_append(GTK_BOX(identity), GTK_WIDGET(row->title));
    GtkWidget *account = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(account, GTK_ALIGN_CENTER);
    row->detail = label_new(NULL, "account-detail");
    row->message = label_new(NULL, "row-message");
    gtk_widget_set_visible(GTK_WIDGET(row->message), FALSE);
    gtk_box_append(GTK_BOX(account), GTK_WIDGET(row->detail));
    gtk_box_append(GTK_BOX(account), GTK_WIDGET(row->message));
    gtk_box_append(GTK_BOX(columns), account);
    GtkWidget *session = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    row->session_label = label_new("Available", "session-label");
    gtk_box_append(GTK_BOX(session), row->live_dot);
    gtk_box_append(GTK_BOX(session), GTK_WIDGET(row->session_label));
    gtk_box_append(GTK_BOX(columns), session);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(buttons, FALSE);
    gtk_widget_set_size_request(buttons, 100, -1);
    gtk_widget_set_valign(buttons, GTK_ALIGN_CENTER);
    row->launch_one = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(row->launch_one), button_content("media-playback-start-symbolic", "Launch"));
    gtk_widget_add_css_class(row->launch_one, "launch-one");
    g_autofree char *launch_hint = g_strdup_printf("Launch %s", name);
    gtk_widget_set_tooltip_text(row->launch_one, launch_hint);
    gtk_accessible_update_property(GTK_ACCESSIBLE(row->launch_one), GTK_ACCESSIBLE_PROPERTY_LABEL, launch_hint, -1);
    g_signal_connect(row->launch_one, "clicked", G_CALLBACK(launch_one), row);
    GtkWidget *action_space = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(action_space, TRUE);
    gtk_box_append(GTK_BOX(buttons), action_space);
    gtk_box_append(GTK_BOX(buttons), row->launch_one);
    GtkWidget *menu = menu_new(gtk_image_new_from_icon_name("view-more-symbolic"), "Profile actions");
    row->actions = menu;
    g_signal_connect(gtk_menu_button_get_popover(GTK_MENU_BUTTON(menu)), "closed", G_CALLBACK(reset_menu_anchor), NULL);
    gtk_widget_add_css_class(menu, "profile-actions-menu");
    row->capture = menu_item(menu, "Set as join target", G_CALLBACK(capture_target), row);
    row->configure = menu_item(menu, "Profile settings…", G_CALLBACK(settings_clicked), row);
    row->rename = menu_item(menu, "Rename…", G_CALLBACK(rename_clicked), row);
    row->load = menu_item(menu, "Load cookie…", G_CALLBACK(load_clicked), row);
    gtk_box_append(GTK_BOX(buttons), menu);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkGesture *context = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(context), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(context), GTK_PHASE_CAPTURE);
    g_signal_connect(context, "pressed", G_CALLBACK(row_context), row);
    gtk_widget_add_controller(row->widget, GTK_EVENT_CONTROLLER(context));
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
    if (!gtk_widget_get_mapped(widget)) {
        char **latest = accounts->pending_names ? accounts->pending_names : accounts->names;
        if (!latest || !g_strv_equal((const char *const *)latest, names)) {
            g_strfreev(accounts->pending_names);
            accounts->pending_names = g_strdupv((char **)names);
        }
        return;
    }
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
            if (!row) gtk_list_box_append(accounts->list, row_new(accounts, names[i]));
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

static void roster_mapped(GtkWidget *widget, Accounts *accounts)
{
    g_auto(GStrv) pending = g_steal_pointer(&accounts->pending_names);
    char **names = pending ? pending : accounts->names;
    if (names) accounts_update(widget, (const char *const *)names);
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
    if (accounts->pending_save) {
        g_source_remove(accounts->pending_save);
        save_target(accounts);
    }
    accounts->stopped = TRUE;
    if (accounts->dialog) {
        AccountDialog *dialog = g_object_get_data(G_OBJECT(accounts->dialog), "account-dialog");
        dialog_stop(dialog);
        gtk_window_destroy(accounts->dialog);
        accounts->dialog = NULL;
    }
    if (accounts->configuration) gtk_window_destroy(accounts->configuration);
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
    g_strfreev(accounts->pending_names);
    if (accounts->pending_save) g_source_remove(accounts->pending_save);
    g_free(accounts->captured_place);
    g_free(accounts->captured_job);
    g_free(accounts->query);
    g_hash_table_unref(accounts->refresh_attempts);
    g_free(accounts);
}

static int roster_order(GtkListBoxRow *a, GtkListBoxRow *b, gpointer data)
{
    (void)data;
    AccountRow *left = g_object_get_data(G_OBJECT(a), "account-row");
    AccountRow *right = g_object_get_data(G_OBJECT(b), "account-row");
    if (left->is_running != right->is_running) return left->is_running ? -1 : 1;
    return g_ascii_strcasecmp(left->name, right->name);
}

static GtkEntry *join_entry(const char *name, const char *hint, int width)
{
    GtkEntry *entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(entry, name);
    gtk_widget_set_tooltip_text(GTK_WIDGET(entry), hint);
    gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    gtk_editable_set_width_chars(GTK_EDITABLE(entry), width);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(entry), width);
    gtk_entry_set_max_length(entry, 128);
    gtk_widget_add_css_class(GTK_WIDGET(entry), "target-entry");
    return entry;
}

static GtkWidget *make_target_bar(Accounts *accounts)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(bar, "shared-target");
    GtkLabel *label = label_new("Join target", "target-heading");
    gtk_widget_set_hexpand(GTK_WIDGET(label), FALSE);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(label));
    GtkWidget *divider = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_valign(divider, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(bar), divider);

    accounts->manual = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_hexpand(accounts->manual, TRUE);
    gtk_widget_set_valign(accounts->manual, GTK_ALIGN_CENTER);
    accounts->place_entry = join_entry("Place ID", "Leave both fields blank to open Roblox Home", 17);
    gtk_entry_set_input_purpose(accounts->place_entry, GTK_INPUT_PURPOSE_DIGITS);
    accounts->job_entry = join_entry("Job ID (optional)", "Leave blank to join any server in this place", 18);
    gtk_widget_set_hexpand(GTK_WIDGET(accounts->job_entry), TRUE);
    gtk_box_append(GTK_BOX(accounts->manual), GTK_WIDGET(accounts->place_entry));
    gtk_box_append(GTK_BOX(accounts->manual), GTK_WIDGET(accounts->job_entry));
    gtk_box_append(GTK_BOX(bar), accounts->manual);

    accounts->captured = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_hexpand(accounts->captured, TRUE);
    gtk_widget_set_valign(accounts->captured, GTK_ALIGN_CENTER);
    accounts->target_avatar = GTK_IMAGE(gtk_image_new());
    gtk_image_set_pixel_size(accounts->target_avatar, 22);
    gtk_widget_add_css_class(GTK_WIDGET(accounts->target_avatar), "account-avatar");
    accounts->target_initial = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(accounts->target_initial), "roster-initial");
    gtk_box_append(GTK_BOX(accounts->captured), GTK_WIDGET(accounts->target_avatar));
    gtk_box_append(GTK_BOX(accounts->captured), GTK_WIDGET(accounts->target_initial));
    accounts->target_name = label_new(NULL, "target-name");
    gtk_widget_set_hexpand(GTK_WIDGET(accounts->target_name), FALSE);
    accounts->target_game = label_new(NULL, "target-game");
    gtk_box_append(GTK_BOX(accounts->captured), GTK_WIDGET(accounts->target_name));
    gtk_box_append(GTK_BOX(accounts->captured), gtk_label_new("·"));
    gtk_box_append(GTK_BOX(accounts->captured), GTK_WIDGET(accounts->target_game));
    GtkWidget *detach = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(detach, "detach-target");
    gtk_widget_set_tooltip_text(detach, "Return to manual target");
    gtk_accessible_update_property(GTK_ACCESSIBLE(detach), GTK_ACCESSIBLE_PROPERTY_LABEL, "Return to manual target", -1);
    g_signal_connect(detach, "clicked", G_CALLBACK(use_manual_target), accounts);
    gtk_box_append(GTK_BOX(accounts->captured), detach);
    gtk_box_append(GTK_BOX(bar), accounts->captured);
    return bar;
}

GtkWidget *accounts_new(ProfileLaunchFunc launch, ProfileRunningFunc running,
                        ProfileSessionFunc session, gpointer user_data)
{
    Accounts *accounts = g_new0(Accounts, 1);
    accounts->refresh_attempts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    accounts->launch = launch;
    accounts->running = running;
    accounts->session = session;
    accounts->user_data = user_data;
    accounts->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(accounts->widget, "accounts-page");
    g_object_set_data_full(G_OBJECT(accounts->widget), "accounts", accounts, free_accounts);
    g_signal_connect(accounts->widget, "map", G_CALLBACK(roster_mapped), accounts);
    GtkWidget *roster = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(roster, TRUE);
    gtk_widget_add_css_class(roster, "profile-roster");
    gtk_box_append(GTK_BOX(accounts->widget), roster);
    GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(heading, "roster-heading");
    gtk_box_append(GTK_BOX(heading), GTK_WIDGET(label_new("Profiles", "accounts-title")));

    GtkWidget *filter = menu_new(gtk_label_new("Filter"), "Filter profiles");
    accounts->filter_menu = filter;
    gtk_menu_button_set_label(GTK_MENU_BUTTON(filter), "Filter");
    gtk_widget_add_css_class(filter, "filter-menu");
    GtkPopover *filter_popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(filter));
    GtkBox *filter_box = GTK_BOX(gtk_popover_get_child(filter_popover));
    gtk_box_set_spacing(filter_box, 5);
    accounts->search = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(accounts->search, "Filter by name…");
    gtk_editable_set_width_chars(GTK_EDITABLE(accounts->search), 18);
    gtk_widget_add_css_class(GTK_WIDGET(accounts->search), "accounts-search");
    gtk_accessible_update_property(GTK_ACCESSIBLE(accounts->search), GTK_ACCESSIBLE_PROPERTY_LABEL, "Filter by name", -1);
    const char *filters[] = {"All profiles", "With account", "Logged out", "Running", "Not running", NULL};
    accounts->filter = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(filters));
    gtk_widget_add_css_class(GTK_WIDGET(accounts->filter), "accounts-filter");
    gtk_accessible_update_property(GTK_ACCESSIBLE(accounts->filter), GTK_ACCESSIBLE_PROPERTY_LABEL, "Profile status", -1);
    gtk_box_append(filter_box, GTK_WIDGET(accounts->search));
    gtk_box_append(filter_box, GTK_WIDGET(accounts->filter));
    gtk_box_append(GTK_BOX(heading), filter);
    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, "Refresh all profiles");
    gtk_accessible_update_property(GTK_ACCESSIBLE(refresh), GTK_ACCESSIBLE_PROPERTY_LABEL, "Refresh all profiles", -1);
    gtk_widget_add_css_class(refresh, "roster-refresh");
    gtk_box_append(GTK_BOX(heading), refresh);
    GtkWidget *create = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(create, "create-profile");
    gtk_widget_set_valign(create, GTK_ALIGN_CENTER);
    GtkWidget *new_profile = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(new_profile), button_content("list-add-symbolic", "New profile"));
    gtk_widget_add_css_class(new_profile, "new-profile");
    g_signal_connect(new_profile, "clicked", G_CALLBACK(new_clicked), accounts);
    gtk_box_append(GTK_BOX(create), new_profile);
    GtkWidget *add = menu_new(gtk_image_new_from_resource("/org/relapse/RelapseGtk/icons/caret-down.svg"), "More creation actions");
    gtk_widget_add_css_class(add, "accounts-add");
    menu_item(add, "Add account…", G_CALLBACK(add_clicked), accounts);
    gtk_box_append(GTK_BOX(create), add);
    gtk_box_append(GTK_BOX(heading), create);
    accounts->clear_selection = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(accounts->clear_selection, "clear-selection");
    gtk_widget_set_tooltip_text(accounts->clear_selection, "Clear selection");
    gtk_accessible_update_property(GTK_ACCESSIBLE(accounts->clear_selection), GTK_ACCESSIBLE_PROPERTY_LABEL, "Clear selection", -1);
    g_signal_connect(accounts->clear_selection, "clicked", G_CALLBACK(clear_selected), accounts);
    gtk_box_append(GTK_BOX(heading), accounts->clear_selection);
    accounts->launch_button = gtk_button_new();
    GtkWidget *bulk = button_content("media-playback-start-symbolic", "Launch");
    accounts->launch_label = GTK_LABEL(gtk_widget_get_last_child(bulk));
    gtk_button_set_child(GTK_BUTTON(accounts->launch_button), bulk);
    gtk_widget_add_css_class(accounts->launch_button, "batch-launch");
    gtk_widget_set_tooltip_text(accounts->launch_button, "Launch selected profiles");
    g_signal_connect(accounts->launch_button, "clicked", G_CALLBACK(launch_selected), accounts);
    gtk_box_append(GTK_BOX(heading), accounts->launch_button);
    gtk_box_append(GTK_BOX(roster), heading);
    gtk_box_append(GTK_BOX(roster), make_target_bar(accounts));
    accounts->target_error = label_new(NULL, "target-error");
    gtk_label_set_max_width_chars(accounts->target_error, -1);
    set_message(accounts->target_error, NULL);
    gtk_box_append(GTK_BOX(roster), GTK_WIDGET(accounts->target_error));
    GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(titles, "roster-columns");
    GtkWidget *columns = roster_columns(titles);
    const char *labels[] = {"Profile", "Account", "Current session"};
    for (guint i = 0; i < G_N_ELEMENTS(labels); i++)
        gtk_box_append(GTK_BOX(columns), GTK_WIDGET(label_new(labels[i], NULL)));
    GtkWidget *end = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(end, 100, -1);
    gtk_box_append(GTK_BOX(titles), end);
    gtk_box_append(GTK_BOX(roster), titles);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    accounts->list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(accounts->list, GTK_SELECTION_MULTIPLE);
    gtk_list_box_set_activate_on_single_click(accounts->list, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(accounts->list), "accounts-list");
    accounts->empty = GTK_LABEL(gtk_label_new("Create a profile to get started."));
    gtk_widget_add_css_class(GTK_WIDGET(accounts->empty), "roster-empty");
    gtk_list_box_set_placeholder(accounts->list, GTK_WIDGET(accounts->empty));
    gtk_list_box_set_filter_func(accounts->list, filter_row, accounts, NULL);
    gtk_list_box_set_sort_func(accounts->list, roster_order, NULL, NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(accounts->list));
    gtk_box_append(GTK_BOX(roster), scroll);
    restore_target(accounts);
    g_signal_connect(accounts->list, "selected-rows-changed", G_CALLBACK(selected_rows_changed), accounts);
    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_all_clicked), accounts);
    g_signal_connect(accounts->search, "changed", G_CALLBACK(search_changed), accounts);
    g_signal_connect(accounts->filter, "notify::selected", G_CALLBACK(filter_changed), accounts);
    selection_sync(accounts);
    return accounts->widget;
}
