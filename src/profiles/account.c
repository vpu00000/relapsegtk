#include "account.h"
#include "store.h"

#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <linux/fs.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { COOKIE_LIMIT = 16384, FILE_LIMIT = 1024 * 1024 };

static gboolean failure(GError **error, GIOErrorEnum code, const char *message)
{
    g_set_error_literal(error, G_IO_ERROR, code, message);
    return FALSE;
}

static gboolean io_failure(GError **error, const char *message)
{
    int saved = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved), "%s: %s", message, g_strerror(saved));
    return FALSE;
}

void profile_account_free(ProfileAccount *account)
{
    if (!account) return;
    g_free(account->username);
    g_free(account->display_name);
    g_free(account->cookie_hash);
    g_clear_pointer(&account->avatar, g_bytes_unref);
    g_free(account);
}

static char *cookie_from_header(const char *text)
{
    g_auto(GStrv) parts = g_strsplit(text, ";", -1);
    for (guint i = 0; parts[i]; ++i) {
        const char *part = g_strstrip(parts[i]);
        if (g_str_has_prefix(part, ".ROBLOSECURITY="))
            return g_strdup(part + strlen(".ROBLOSECURITY="));
    }
    return NULL;
}

char *profile_cookie_parse(const char *text, GError **error)
{
    if (!text || !*text || strlen(text) > COOKIE_LIMIT) {
        failure(error, G_IO_ERROR_INVALID_ARGUMENT, "Paste a .ROBLOSECURITY cookie.");
        return NULL;
    }
    g_autofree char *input = g_strdup(text);
    g_strstrip(input);
    char *cookie = cookie_from_header(input);
    if (!cookie) cookie = g_strdup(input);
    if (!*cookie) goto invalid;
    for (const unsigned char *p = (const unsigned char *)cookie; *p; ++p)
        if (*p <= 32 || *p >= 127 || *p == ';' || *p == '"' || *p == ',') goto invalid;
    return cookie;
invalid:
    g_free(cookie);
    failure(error, G_IO_ERROR_INVALID_ARGUMENT, "Paste the cookie value or a .ROBLOSECURITY cookie header.");
    return NULL;
}

static int directory_at(int parent, const char *name, gboolean create, GError **error)
{
    if (create && mkdirat(parent, name, 0700) != 0 && errno != EEXIST) {
        io_failure(error, "Could not create profile directory");
        return -1;
    }
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    if (fd < 0) {
        io_failure(error, "Could not open profile directory");
    } else if (fstat(fd, &info) != 0 || info.st_uid != getuid()) {
        close(fd);
        fd = -1;
        failure(error, G_IO_ERROR_PERMISSION_DENIED, "Profile directory must belong to you.");
    }
    return fd;
}

static int store_open(gboolean create, GError **error)
{
    g_autofree char *path = g_build_filename(g_get_home_dir(), ".local", "share", "relapse", "profiles", NULL);
    if (create && g_mkdir_with_parents(path, 0700) != 0) {
        io_failure(error, "Could not create profile store");
        return -1;
    }
    return directory_at(AT_FDCWD, path, FALSE, error);
}

static int profile_open(const char *name, GError **error)
{
    if (!profile_name_valid(name)) {
        failure(error, G_IO_ERROR_INVALID_ARGUMENT, "Invalid profile name.");
        return -1;
    }
    int store = store_open(FALSE, error);
    if (store < 0) return -1;
    int profile = directory_at(store, name, FALSE, error);
    close(store);
    return profile;
}

static char *read_at(int directory, const char *name, GError **error)
{
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENOENT) io_failure(error, "Could not read profile file");
        return NULL;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid() || info.st_size > FILE_LIMIT) {
        close(fd);
        failure(error, G_IO_ERROR_INVALID_DATA, "Invalid profile file.");
        return NULL;
    }
    GString *contents = g_string_sized_new(info.st_size + 1);
    char buffer[4096];
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || contents->len + MAX(count, 0) > FILE_LIMIT) {
            if (count < 0) io_failure(error, "Could not read profile file");
            else failure(error, G_IO_ERROR_INVALID_DATA, "Profile file is too large.");
            close(fd);
            g_string_free(contents, TRUE);
            return NULL;
        }
        if (!count) break;
        g_string_append_len(contents, buffer, count);
    }
    close(fd);
    if (memchr(contents->str, 0, contents->len)) {
        g_string_free(contents, TRUE);
        failure(error, G_IO_ERROR_INVALID_DATA, "Invalid profile file.");
        return NULL;
    }
    return g_string_free(contents, FALSE);
}

static gboolean write_at(int directory, const char *name, const char *contents, GError **error)
{
    g_autofree char *uuid = g_uuid_string_random();
    g_autofree char *temporary = g_strconcat(".account-", uuid, NULL);
    int fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return io_failure(error, "Could not save profile file");
    gboolean ok = TRUE;
    size_t remaining = strlen(contents);
    while (remaining) {
        ssize_t count = write(fd, contents, remaining);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = io_failure(error, "Could not save profile file"); break; }
        contents += count;
        remaining -= count;
    }
    if (ok && fsync(fd) != 0) ok = io_failure(error, "Could not sync profile file");
    close(fd);
    if (ok && renameat(directory, temporary, directory, name) != 0)
        ok = io_failure(error, "Could not replace profile file");
    if (ok && fsync(directory) != 0) ok = io_failure(error, "Could not sync profile directory");
    if (!ok) unlinkat(directory, temporary, 0);
    return ok;
}

static int descend(int parent, const char *path, gboolean create, GError **error)
{
    g_auto(GStrv) parts = g_strsplit(path, "/", -1);
    int current = dup(parent);
    for (guint i = 0; current >= 0 && parts[i]; ++i) {
        int next = directory_at(current, parts[i], create, error);
        close(current);
        current = next;
    }
    return current;
}

static char *cookie_read_at(int profile, GError **error)
{
    g_autoptr(GError) local = NULL;
    int directory = descend(profile, "state/app/data/sober", FALSE, &local);
    if (directory < 0) {
        if (!g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            g_propagate_error(error, g_steal_pointer(&local));
        return NULL;
    }
    g_autofree char *contents = read_at(directory, "cookies", error);
    close(directory);
    if (!contents) return NULL;
    g_autofree char *cookie = cookie_from_header(contents);
    return cookie && *cookie ? profile_cookie_parse(cookie, error) : NULL;
}

char *profile_cookie_read(const char *name, GError **error)
{
    int profile = profile_open(name, error);
    if (profile < 0) return NULL;
    char *cookie = cookie_read_at(profile, error);
    close(profile);
    return cookie;
}

static const char *json_string(JsonObject *object, const char *key)
{
    JsonNode *node = json_object_get_member(object, key);
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
        ? json_node_get_string(node) : NULL;
}

static gint64 json_integer(JsonObject *object, const char *key)
{
    JsonNode *node = json_object_get_member(object, key);
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64
        ? json_node_get_int(node) : 0;
}

static gboolean valid_label(const char *text)
{
    if (!text || !*text || strlen(text) > 256 || !g_utf8_validate(text, -1, NULL)) return FALSE;
    for (const char *p = text; *p; ++p) if ((unsigned char)*p < 32) return FALSE;
    return TRUE;
}

ProfileAccount *profile_account_load(const char *name, const char *cookie)
{
    if (!cookie) return NULL;
    int profile = profile_open(name, NULL);
    if (profile < 0) return NULL;
    g_autofree char *data = read_at(profile, "account.json", NULL);
    close(profile);
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!data || !json_parser_load_from_data(parser, data, -1, NULL)) return NULL;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return NULL;
    JsonObject *object = json_node_get_object(root);
    const char *username = json_string(object, "username");
    const char *display = json_string(object, "display_name");
    const char *hash = json_string(object, "cookie_sha256");
    g_autofree char *expected = g_compute_checksum_for_string(G_CHECKSUM_SHA256, cookie, -1);
    gint64 id = json_integer(object, "user_id");
    if (json_integer(object, "version") != 1 || id <= 0 || !valid_label(username) ||
        !valid_label(display) || g_strcmp0(hash, expected) != 0) return NULL;
    ProfileAccount *account = g_new0(ProfileAccount, 1);
    account->user_id = id;
    account->username = g_strdup(username);
    account->display_name = g_strdup(display);
    account->cookie_hash = g_strdup(hash);
    account->checked_at = json_integer(object, "checked_at");
    const char *avatar = json_string(object, "avatar_png_base64");
    if (avatar) {
        gsize size;
        guchar *bytes = g_base64_decode(avatar, &size);
        account->avatar = g_bytes_new_take(bytes, size);
    }
    return account;
}

static gboolean account_write_at(int profile, const ProfileAccount *account, GError **error)
{
    g_autoptr(JsonObject) object = json_object_new();
    json_object_set_int_member(object, "version", 1);
    json_object_set_int_member(object, "user_id", account->user_id);
    json_object_set_string_member(object, "username", account->username);
    json_object_set_string_member(object, "display_name", account->display_name);
    json_object_set_string_member(object, "cookie_sha256", account->cookie_hash);
    json_object_set_int_member(object, "checked_at", account->checked_at);
    if (account->avatar) {
        gsize size;
        const guchar *data = g_bytes_get_data(account->avatar, &size);
        g_autofree char *encoded = g_base64_encode(data, size);
        json_object_set_string_member(object, "avatar_png_base64", encoded);
    }
    g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(root, object);
    g_autofree char *data = json_to_string(root, TRUE);
    return write_at(profile, "account.json", data, error);
}

gboolean profile_account_save(const char *name, const ProfileAccount *account, GError **error)
{
    int profile = profile_open(name, error);
    if (profile < 0) return FALSE;
    g_autofree char *cookie = cookie_read_at(profile, error);
    g_autofree char *hash = cookie ? g_compute_checksum_for_string(G_CHECKSUM_SHA256, cookie, -1) : NULL;
    gboolean ok = FALSE;
    if (g_strcmp0(hash, account->cookie_hash) != 0) {
        if (!error || !*error) failure(error, G_IO_ERROR_BUSY, "The login changed. Refresh this account again.");
    } else {
        ok = account_write_at(profile, account, error);
    }
    close(profile);
    return ok;
}

static void remove_staging(int parent, const char *name)
{
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { unlinkat(parent, name, 0); return; }
    g_autofree char *path = g_strdup_printf("/proc/self/fd/%d", fd);
    g_autoptr(GDir) directory = g_dir_open(path, 0, NULL);
    const char *child;
    while (directory && (child = g_dir_read_name(directory))) remove_staging(fd, child);
    close(fd);
    unlinkat(parent, name, AT_REMOVEDIR);
}

static gboolean prepare_state(int profile, GError **error)
{
    struct stat status;
    if (fstatat(profile, "state", &status, AT_SYMLINK_NOFOLLOW) == 0) {
        int state = directory_at(profile, "state", FALSE, error);
        if (state < 0) return FALSE;
        g_autofree char *format = read_at(state, ".format", error);
        close(state);
        if (!format || !g_str_equal(g_strstrip(format), "1")) {
            if (!error || !*error) failure(error, G_IO_ERROR_INVALID_DATA, "Unrecognized profile layout.");
            return FALSE;
        }
        return TRUE;
    }
    if (errno != ENOENT) return io_failure(error, "Could not inspect profile layout");
    g_autofree char *uuid = g_uuid_string_random();
    g_autofree char *staging = g_strconcat(".account-state-", uuid, NULL);
    int state = directory_at(profile, staging, TRUE, error);
    if (state < 0) return FALSE;
    const char *paths[] = {"app/data/sober/appData", "app/config/sober", "app/cache", "app/.local/state", "assets", "assets-work"};
    gboolean ok = TRUE;
    for (guint i = 0; ok && i < G_N_ELEMENTS(paths); ++i) {
        int fd = descend(state, paths[i], TRUE, error);
        ok = fd >= 0;
        if (fd >= 0) close(fd);
    }
    if (ok) ok = write_at(state, ".format", "1\n", error);
    close(state);
    if (ok && syscall(SYS_renameat2, profile, staging, profile, "state", RENAME_NOREPLACE) != 0)
        ok = io_failure(error, "Could not initialize profile");
    if (!ok) remove_staging(profile, staging);
    return ok;
}

gboolean profile_account_import(const char *name, const char *text, const ProfileAccount *account,
                                gboolean create, GError **error)
{
    if (!profile_name_valid(name))
        return failure(error, G_IO_ERROR_INVALID_ARGUMENT, "Use 1–64 letters, digits, dots, underscores or hyphens for the profile name.");
    g_autofree char *cookie = profile_cookie_parse(text, error);
    if (!cookie) return FALSE;
    g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, cookie, -1);
    if (!account || account->user_id <= 0 || !valid_label(account->username) || !valid_label(account->display_name) ||
        g_strcmp0(hash, account->cookie_hash) != 0)
        return failure(error, G_IO_ERROR_INVALID_ARGUMENT, "Check the cookie before adding the account.");
    int store = store_open(create, error);
    if (store < 0) return FALSE;
    g_autofree char *uuid = g_uuid_string_random();
    g_autofree char *staging = create ? g_strconcat(".account import ", uuid, NULL) : NULL;
    int profile = directory_at(store, create ? staging : name, create, error);
    int lock = profile < 0 ? -1 : openat(profile, ".lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    int sober = -1;
    gboolean ok = FALSE;
    struct stat status;
    g_autofree char *previous = NULL;
    g_autoptr(GString) header = NULL;
    g_auto(GStrv) parts = NULL;
    if (profile < 0) goto done;
    if (lock < 0) { io_failure(error, "Could not lock profile"); goto done; }
    if (fstat(lock, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != getuid()) {
        failure(error, G_IO_ERROR_PERMISSION_DENIED, "Invalid profile lock.");
        goto done;
    }
    if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
        failure(error, G_IO_ERROR_BUSY, "Close this profile before loading a cookie.");
        goto done;
    }
    if (!create) {
        struct stat current;
        if (fstat(profile, &status) != 0 || fstatat(store, name, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
            status.st_dev != current.st_dev || status.st_ino != current.st_ino) {
            failure(error, G_IO_ERROR_BUSY, "The profile was renamed. Try again.");
            goto done;
        }
    }
    if (!prepare_state(profile, error)) goto done;
    sober = descend(profile, "state/app/data/sober", FALSE, error);
    if (sober < 0) goto done;
    previous = read_at(sober, "cookies", error);
    if (error && *error) goto done;
    header = g_string_new(NULL);
    parts = g_strsplit(previous ? previous : "", ";", -1);
    for (guint i = 0; parts[i]; ++i) {
        const char *part = g_strstrip(parts[i]);
        if (*part && !g_str_has_prefix(part, ".ROBLOSECURITY=")) {
            if (header->len) g_string_append(header, "; ");
            g_string_append(header, part);
        }
    }
    if (header->len) g_string_append(header, "; ");
    g_string_append_printf(header, ".ROBLOSECURITY=%s", cookie);
    if (!account_write_at(profile, account, error) || !write_at(sober, "cookies", header->str, error)) goto done;
    if (create && syscall(SYS_renameat2, store, staging, store, name, RENAME_NOREPLACE) != 0) {
        io_failure(error, "Could not add profile");
        goto done;
    }
    ok = TRUE;
done:
    if (sober >= 0) close(sober);
    if (lock >= 0) close(lock);
    if (profile >= 0) close(profile);
    if (create && !ok) remove_staging(store, staging);
    close(store);
    return ok;
}

typedef struct { GByteArray *bytes; GCancellable *cancellable; } Download;

static size_t receive(void *data, size_t size, size_t count, void *user_data)
{
    Download *download = user_data;
    if (count > FILE_LIMIT / MAX(size, 1) || download->bytes->len + size * count > FILE_LIMIT) return 0;
    g_byte_array_append(download->bytes, data, size * count);
    return size * count;
}

static int progress(void *user_data, curl_off_t total, curl_off_t now, curl_off_t upload_total, curl_off_t upload_now)
{
    (void)total; (void)now; (void)upload_total; (void)upload_now;
    return g_cancellable_is_cancelled(user_data);
}

static GBytes *download(const char *url, const char *cookie, GCancellable *cancellable, GError **error)
{
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
        g_once_init_leave(&initialized, code == CURLE_OK ? 1 : 2);
    }
    CURL *curl = initialized == 1 ? curl_easy_init() : NULL;
    if (!curl) { failure(error, G_IO_ERROR_FAILED, "Could not initialize account lookup."); return NULL; }
    g_autoptr(GByteArray) bytes = g_byte_array_new();
    Download state = {bytes, cancellable};
    g_autofree char *header = cookie ? g_strconcat(".ROBLOSECURITY=", cookie, NULL) : NULL;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "RelapseGtk");
    curl_easy_setopt(curl, CURLOPT_COOKIE, header);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancellable);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (g_cancellable_set_error_if_cancelled(cancellable, error)) return NULL;
    if (result != CURLE_OK) failure(error, G_IO_ERROR_FAILED, "Could not reach Roblox. Try refreshing again.");
    else if (status == 401 || status == 403) failure(error, G_IO_ERROR_PERMISSION_DENIED, "Login expired or invalid. Load a new cookie.");
    else if (status == 429) failure(error, G_IO_ERROR_BUSY, "Roblox is limiting requests. Try again later.");
    else if (status != 200) failure(error, G_IO_ERROR_FAILED, "Roblox could not complete the account lookup.");
    else return g_byte_array_free_to_bytes(g_steal_pointer(&bytes));
    return NULL;
}

static JsonObject *parse_object(GBytes *bytes, JsonParser *parser)
{
    gsize size;
    const char *data = g_bytes_get_data(bytes, &size);
    if (!json_parser_load_from_data(parser, data, size, NULL)) return NULL;
    JsonNode *root = json_parser_get_root(parser);
    return root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
}

static GBytes *fetch_avatar(gint64 id, GCancellable *cancellable)
{
    g_autofree char *url = g_strdup_printf("https://thumbnails.roblox.com/v1/users/avatar-headshot?userIds=%" G_GINT64_FORMAT "&size=150x150&format=Png&isCircular=false", id);
    g_autoptr(GBytes) response = download(url, NULL, cancellable, NULL);
    if (!response) return NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *object = parse_object(response, parser);
    JsonNode *data = object ? json_object_get_member(object, "data") : NULL;
    if (!data || !JSON_NODE_HOLDS_ARRAY(data)) return NULL;
    JsonArray *items = json_node_get_array(data);
    if (!json_array_get_length(items)) return NULL;
    JsonNode *item = json_array_get_element(items, 0);
    if (!JSON_NODE_HOLDS_OBJECT(item)) return NULL;
    const char *image = json_string(json_node_get_object(item), "imageUrl");
    g_autoptr(GUri) uri = image ? g_uri_parse(image, G_URI_FLAGS_NONE, NULL) : NULL;
    const char *host = uri ? g_uri_get_host(uri) : NULL;
    if (!host || g_strcmp0(g_uri_get_scheme(uri), "https") != 0 ||
        g_uri_get_userinfo(uri) || !(g_str_equal(host, "rbxcdn.com") || g_str_has_suffix(host, ".rbxcdn.com"))) return NULL;
    GBytes *avatar = download(image, NULL, cancellable, NULL);
    if (avatar && g_bytes_get_size(avatar) > 256 * 1024) g_clear_pointer(&avatar, g_bytes_unref);
    return avatar;
}

static void lookup_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    (void)source;
    if (g_task_return_error_if_cancelled(task)) return;
    const char *cookie = task_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) response = download("https://users.roblox.com/v1/users/authenticated", cookie, cancellable, &error);
    if (!response) { g_task_return_error(task, g_steal_pointer(&error)); return; }
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *object = parse_object(response, parser);
    gint64 id = object ? json_integer(object, "id") : 0;
    const char *username = object ? json_string(object, "name") : NULL;
    const char *display = object ? json_string(object, "displayName") : NULL;
    if (id <= 0 || !valid_label(username) || !valid_label(display)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Roblox returned an invalid account response.");
        return;
    }
    ProfileAccount *account = g_new0(ProfileAccount, 1);
    account->user_id = id;
    account->username = g_strdup(username);
    account->display_name = g_strdup(display);
    account->cookie_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, cookie, -1);
    account->checked_at = g_get_real_time() / G_USEC_PER_SEC;
    account->avatar = fetch_avatar(id, cancellable);
    g_task_return_pointer(task, account, (GDestroyNotify)profile_account_free);
}

void profile_account_lookup_async(const char *cookie, GCancellable *cancellable,
                                  GAsyncReadyCallback callback, gpointer user_data)
{
    g_autoptr(GTask) task = g_task_new(NULL, cancellable, callback, user_data);
    g_autoptr(GError) error = NULL;
    char *value = profile_cookie_parse(cookie, &error);
    if (!value) { g_task_return_error(task, g_steal_pointer(&error)); return; }
    g_task_set_task_data(task, value, g_free);
    g_task_run_in_thread(task, lookup_worker);
}

ProfileAccount *profile_account_lookup_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_pointer(G_TASK(result), error);
}
