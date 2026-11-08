#include "update.h"

#include <json-glib/json-glib.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "relapsegtk-update-key.h"

static const char *appimage_name = "RelapseGtk-x86_64.AppImage";

const char *update_current_version(void)
{
    return RELAPSEGTK_VERSION;
}

const char *update_repository(void)
{
    return RELAPSEGTK_UPDATE_REPOSITORY;
}

gboolean update_is_configured(void)
{
    const char *image = g_getenv("APPIMAGE");
    return image && image[0] == '/';
}

UpdateInfo *update_info_copy(const UpdateInfo *info)
{
    if (!info) return NULL;
    UpdateInfo *copy = g_new0(UpdateInfo, 1);
    copy->version = g_strdup(info->version);
    copy->appimage_url = g_strdup(info->appimage_url);
    copy->signature_url = g_strdup(info->signature_url);
    return copy;
}

void update_info_free(UpdateInfo *info)
{
    if (!info) return;
    g_free(info->version);
    g_free(info->appimage_url);
    g_free(info->signature_url);
    g_free(info);
}

static const char *tag_digits(const char *version)
{
    while (*version && !g_ascii_isdigit(*version)) ++version;
    return version;
}

static gboolean version_newer(const char *candidate, const char *current)
{
    if (!candidate || !current) return FALSE;
    const char *left = tag_digits(candidate);
    const char *right = tag_digits(current);
    for (int part = 0; part < 4; ++part) {
        char *left_end = NULL, *right_end = NULL;
        guint64 a = g_ascii_strtoull(left, &left_end, 10);
        guint64 b = g_ascii_strtoull(right, &right_end, 10);
        if (a != b) return a > b;
        left = left_end;
        right = right_end;
        if (*left == '.') ++left;
        if (*right == '.') ++right;
    }
    return FALSE;
}

static const char *string_member(JsonObject *object, const char *name)
{
    JsonNode *node = json_object_get_member(object, name);
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
        ? json_node_get_string(node) : NULL;
}

static UpdateInfo *parse_release(const char *payload, GError **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, payload, -1, error)) return NULL;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid release response");
        return NULL;
    }
    JsonObject *release = json_node_get_object(root);
    const char *version = string_member(release, "tag_name");
    JsonNode *assets_node = json_object_get_member(release, "assets");
    if (!version || !*version || strlen(version) > 64 || !assets_node || !JSON_NODE_HOLDS_ARRAY(assets_node)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Release is missing its version or assets");
        return NULL;
    }
    UpdateInfo *info = g_new0(UpdateInfo, 1);
    info->version = g_strdup(version);
    JsonArray *assets = json_node_get_array(assets_node);
    g_autofree char *signature_name = g_strconcat(appimage_name, ".sig", NULL);
    guint count = json_array_get_length(assets);
    for (guint i = 0; i < count; ++i) {
        JsonNode *node = json_array_get_element(assets, i);
        if (!JSON_NODE_HOLDS_OBJECT(node)) continue;
        JsonObject *asset = json_node_get_object(node);
        const char *name = string_member(asset, "name");
        const char *url = string_member(asset, "browser_download_url");
        if (!name || !url || strlen(url) > 4096 || !g_str_has_prefix(url, "https://")) continue;
        if (g_str_equal(name, appimage_name))
            info->appimage_url = g_strdup(url);
        else if (g_str_equal(name, signature_name))
            info->signature_url = g_strdup(url);
    }
    if (!version_newer(info->version, update_current_version())) {
        update_info_free(info);
        return NULL;
    }
    if (!info->appimage_url || !info->signature_url) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Release is missing its AppImage or signature");
        update_info_free(info);
        return NULL;
    }
    return info;
}

typedef struct {
    UpdateCheckFunc callback;
    gpointer user_data;
} CheckRequest;

static void check_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    CheckRequest *request = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree char *payload = NULL;
    if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &payload, NULL, &error)) {
        request->callback(NULL, error->message, request->user_data);
    } else if (!g_subprocess_get_successful(G_SUBPROCESS(source))) {
        request->callback(NULL, "Could not retrieve the latest release", request->user_data);
    } else {
        UpdateInfo *info = parse_release(payload, &error);
        request->callback(info, info ? NULL : error ? error->message : NULL, request->user_data);
        update_info_free(info);
    }
    g_free(request);
}

void update_check_async(UpdateCheckFunc callback, gpointer user_data)
{
    if (!update_is_configured()) {
        callback(NULL, NULL, user_data);
        return;
    }
    g_autofree char *curl = g_find_program_in_path("curl");
    if (!curl) {
        callback(NULL, "curl is required to check for updates", user_data);
        return;
    }
    g_autofree char *url = g_strdup_printf("https://api.github.com/repos/%s/releases/latest",
                                           RELAPSEGTK_UPDATE_REPOSITORY);
    const char *argv[] = {curl, "-fsSL", "--max-time", "20", "-H", "Accept: application/vnd.github+json",
                          "-H", "User-Agent: RelapseGTK-Updater", url, NULL};
    g_autoptr(GError) error = NULL;
    GSubprocess *process = g_subprocess_newv(argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    if (!process) {
        callback(NULL, error->message, user_data);
        return;
    }
    CheckRequest *request = g_new0(CheckRequest, 1);
    request->callback = callback;
    request->user_data = user_data;
    g_subprocess_communicate_utf8_async(process, NULL, NULL, check_ready, request);
    g_object_unref(process);
}

typedef struct {
    UpdateInstallFunc callback;
    gpointer user_data;
    char *curl;
    char *gpgv;
    char *directory;
    char *target;
    char *appimage;
    char *signature;
    char *keyring;
    const UpdateInfo *info;
    int stage;
} InstallRequest;

static void cleanup_install(InstallRequest *request)
{
    if (request->appimage) g_unlink(request->appimage);
    if (request->signature) g_unlink(request->signature);
    if (request->keyring) g_unlink(request->keyring);
    if (request->directory) g_rmdir(request->directory);
}

static void finish_install(InstallRequest *request, const char *error)
{
    cleanup_install(request);
    request->callback(error, request->user_data);
    g_free(request->curl);
    g_free(request->gpgv);
    g_free(request->directory);
    g_free(request->target);
    g_free(request->appimage);
    g_free(request->signature);
    g_free(request->keyring);
    g_free(request);
}

static void install_run(InstallRequest *request);

static void install_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    InstallRequest *request = user_data;
    g_autoptr(GError) error = NULL;
    if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error)) {
        finish_install(request, request->stage == 2 ? "Update signature verification failed" : error->message);
        return;
    }
    if (++request->stage < 3) {
        install_run(request);
        return;
    }
    if (g_chmod(request->appimage, 0755) != 0) {
        finish_install(request, g_strerror(errno));
        return;
    }
    int fd = g_open(request->appimage, O_RDONLY, 0);
    if (fd < 0 || fsync(fd) != 0) {
        if (fd >= 0) close(fd);
        finish_install(request, g_strerror(errno));
        return;
    }
    close(fd);
    if (g_rename(request->appimage, request->target) != 0) {
        finish_install(request, g_strerror(errno));
        return;
    }
    finish_install(request, NULL);
}

static void install_run(InstallRequest *request)
{
    const char *argv[16];
    int i = 0;
    if (request->stage < 2) {
        const char *url = request->stage == 0 ? request->info->appimage_url : request->info->signature_url;
        const char *output = request->stage == 0 ? request->appimage : request->signature;
        argv[i++] = request->curl;
        argv[i++] = "-fsSL";
        argv[i++] = "--max-time";
        argv[i++] = "600";
        argv[i++] = "--max-filesize";
        argv[i++] = "134217728";
        argv[i++] = "-o";
        argv[i++] = output;
        argv[i++] = url;
    } else {
        argv[i++] = request->gpgv;
        argv[i++] = "--keyring";
        argv[i++] = request->keyring;
        argv[i++] = request->signature;
        argv[i++] = request->appimage;
    }
    argv[i] = NULL;
    g_autoptr(GError) error = NULL;
    GSubprocess *process = g_subprocess_newv((const gchar * const *)argv,
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    if (!process) {
        finish_install(request, error->message);
        return;
    }
    g_subprocess_wait_check_async(process, NULL, install_ready, request);
    g_object_unref(process);
}

void update_install_async(const UpdateInfo *info, UpdateInstallFunc callback, gpointer user_data)
{
    InstallRequest *request = g_new0(InstallRequest, 1);
    request->callback = callback;
    request->user_data = user_data;
    request->info = info;
    if (!update_is_configured() || !info || !g_str_has_prefix(info->appimage_url, "https://") ||
        !g_str_has_prefix(info->signature_url, "https://")) {
        finish_install(request, "This installation cannot be updated");
        return;
    }
    request->target = g_strdup(g_getenv("APPIMAGE"));
    struct stat status;
    if (g_stat(request->target, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
        g_access(request->target, W_OK) != 0) {
        finish_install(request, "The running AppImage is not a user-owned writable file");
        return;
    }
    request->curl = g_find_program_in_path("curl");
    request->gpgv = g_find_program_in_path("gpgv");
    if (!request->curl || !request->gpgv) {
        finish_install(request, "curl and gpgv are required to install updates");
        return;
    }
    g_autofree char *directory = g_path_get_dirname(request->target);
    g_autofree char *pattern = g_build_filename(directory, ".relapsegtk-update.XXXXXX", NULL);
    if (!g_mkdtemp(pattern)) {
        finish_install(request, g_strerror(errno));
        return;
    }
    request->directory = g_steal_pointer(&pattern);
    request->appimage = g_build_filename(request->directory, appimage_name, NULL);
    g_autofree char *signature_name = g_strconcat(appimage_name, ".sig", NULL);
    request->signature = g_build_filename(request->directory, signature_name, NULL);
    request->keyring = g_build_filename(request->directory, "release-key.gpg", NULL);
    g_autoptr(GError) error = NULL;
    if (!g_file_set_contents(request->keyring, (const char *)relapsegtk_update_public_key,
                             RELAPSEGTK_UPDATE_PUBLIC_KEY_SIZE, &error)) {
        finish_install(request, error->message);
        return;
    }
    install_run(request);
}
