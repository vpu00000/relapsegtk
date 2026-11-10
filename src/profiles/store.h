#pragma once

#include <gio/gio.h>

gboolean profile_name_valid(const char *name);
gboolean profile_create(const char *name, GError **error);
gboolean profile_rename(const char *name, const char *replacement, GError **error);
void profile_delete_async(const char *name, GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data);
gboolean profile_delete_finish(GAsyncResult *result, GError **error);
char **profiles_list(void);
char **profile_sockets_list(const char *directory);
char *profile_socket_name(const char *path);
gboolean profile_is_running(const char *name);
/* An empty destination returns NULL without an error. */
char *profile_join_uri(const char *place_id, const char *job_id, GError **error);
GSubprocess *profile_launch(const char *launcher, const char *name,
                            const char *place_id, const char *job_id, GError **error);
