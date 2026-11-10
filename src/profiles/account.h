#pragma once

#include <gio/gio.h>

typedef struct {
    gint64 user_id;
    char *username;
    char *display_name;
    char *cookie_hash;
    gint64 checked_at;
    GBytes *avatar;
} ProfileAccount;

void profile_account_free(ProfileAccount *account);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ProfileAccount, profile_account_free)

char *profile_cookie_parse(const char *text, GError **error);
char *profile_cookie_read(const char *name, GError **error);
ProfileAccount *profile_account_load(const char *name, const char *cookie);
gboolean profile_account_save(const char *name, const ProfileAccount *account, GError **error);
gboolean profile_account_import(const char *name, const char *cookie, const ProfileAccount *account,
                                gboolean create, GError **error);
void profile_account_lookup_async(const char *cookie, GCancellable *cancellable,
                                  GAsyncReadyCallback callback, gpointer user_data);
ProfileAccount *profile_account_lookup_finish(GAsyncResult *result, GError **error);
