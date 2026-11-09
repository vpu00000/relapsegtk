#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <linux/fs.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

gboolean profile_name_valid(const char *name)
{
    if (!name || !*name || strlen(name) > 64 || g_str_equal(name, ".") || g_str_equal(name, ".."))
        return FALSE;
    for (const char *p = name; *p; ++p)
        if (!g_ascii_isalnum(*p) && *p != '.' && *p != '_' && *p != '-')
            return FALSE;
    return TRUE;
}

static char *profiles_directory(void)
{
    return g_build_filename(g_get_home_dir(), ".local", "share", "relapse", "profiles", NULL);
}

gboolean profile_create(const char *name, GError **error)
{
    if (!profile_name_valid(name)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Use 1–64 letters, digits, dots, underscores or hyphens (excluding . and ..).");
        return FALSE;
    }
    g_autofree char *directory = profiles_directory();
    g_autofree char *path = g_build_filename(directory, name, NULL);
    if (g_mkdir_with_parents(directory, 0700) != 0 || g_mkdir(path, 0700) != 0) {
        int failure = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(failure),
            "Could not add Profile: %s", failure == EEXIST ? "this name already exists" : g_strerror(failure));
        return FALSE;
    }
    return TRUE;
}

gboolean profile_rename(const char *name, const char *replacement, GError **error)
{
    if (!profile_name_valid(name) || !profile_name_valid(replacement)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Use 1–64 letters, digits, dots, underscores or hyphens (excluding . and ..).");
        return FALSE;
    }
    g_autofree char *directory = profiles_directory();
    int parent = g_open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
    int source = parent < 0 ? -1 : openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int lock = source < 0 ? -1 : openat(source, ".lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    int failure = errno;
    gboolean renamed = FALSE;
    struct stat info;
    if (lock >= 0) {
        if (fstat(lock, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid()) {
            failure = EACCES;
        } else if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
            failure = errno;
        } else if (fstat(source, &info) != 0) {
            failure = errno;
        } else {
            struct stat current;
            if (fstatat(parent, name, &current, AT_SYMLINK_NOFOLLOW) != 0) {
                failure = errno;
            } else if (current.st_dev != info.st_dev || current.st_ino != info.st_ino) {
                failure = ESTALE;
            } else {
                /* The lock moves with its directory. Never replace a name created
                 * by another UI or launcher, even if it appears during this call. */
                renamed = g_str_equal(name, replacement) ||
                    syscall(SYS_renameat2, parent, name, parent, replacement, RENAME_NOREPLACE) == 0;
                failure = errno;
            }
        }
    }
    if (lock >= 0) close(lock);
    if (source >= 0) close(source);
    if (parent >= 0) close(parent);
    if (renamed) return TRUE;
    if (failure == EWOULDBLOCK || failure == EAGAIN)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Close this Profile before renaming it.");
    else
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(failure), "Could not rename Profile: %s",
            failure == EEXIST ? "this name already exists" : g_strerror(failure));
    return FALSE;
}

static gint compare_names(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(char *const *)a, *(char *const *)b);
}

static char **finish_list(GPtrArray *items)
{
    g_ptr_array_sort(items, compare_names);
    g_ptr_array_add(items, NULL);
    return (char **)g_ptr_array_free(items, FALSE);
}

char **profiles_list(void)
{
    g_autofree char *directory = profiles_directory();
    g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    const char *name;
    while (dir && (name = g_dir_read_name(dir))) {
        if (!profile_name_valid(name)) continue;
        g_autofree char *path = g_build_filename(directory, name, NULL);
        struct stat info;
        if (g_lstat(path, &info) == 0 && S_ISDIR(info.st_mode))
            g_ptr_array_add(items, g_strdup(name));
    }
    return finish_list(items);
}

static gboolean socket_is_listening(const char *path)
{
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (g_strlcpy(address.sun_path, path, sizeof address.sun_path) >= sizeof address.sun_path)
        return FALSE;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return FALSE;
    int result = connect(fd, (struct sockaddr *)&address, sizeof address);
    /* A full Unix accept queue is still a live listener. Never block discovery. */
    gboolean listening = result == 0 || errno == EAGAIN || errno == EINPROGRESS;
    close(fd);
    return listening;
}

char *profile_socket_name(const char *path)
{
    g_autofree char *name = g_path_get_basename(path);
    return g_str_has_prefix(name, "relapse-") && g_str_has_suffix(name, ".sock")
        ? g_strndup(name + 8, strlen(name) - 13) : g_strdup("No Profile");
}

char **profile_sockets_list(const char *directory)
{
    g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    const char *name;
    while (dir && (name = g_dir_read_name(dir))) {
        if (!g_str_equal(name, "relapse.sock")) {
            if (!g_str_has_prefix(name, "relapse-") || !g_str_has_suffix(name, ".sock")) continue;
            g_autofree char *profile = g_strndup(name + 8, strlen(name) - 13);
            if (!profile_name_valid(profile)) continue;
        }
        g_autofree char *path = g_build_filename(directory, name, NULL);
        struct stat info;
        if (g_lstat(path, &info) == 0 && S_ISSOCK(info.st_mode) && info.st_uid == getuid() &&
            socket_is_listening(path))
            g_ptr_array_add(items, g_steal_pointer(&path));
    }
    return finish_list(items);
}

gboolean profile_is_running(const char *name)
{
    if (!profile_name_valid(name)) return FALSE;
    g_autofree char *directory = profiles_directory();
    g_autofree char *path = g_build_filename(directory, name, ".lock", NULL);
    int fd = g_open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
    if (fd < 0) return FALSE;
    gboolean busy = flock(fd, LOCK_EX | LOCK_NB) != 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
    close(fd);
    return busy;
}

static void detach_launcher(gpointer user_data)
{
    (void)user_data;
    setsid();
}

GSubprocess *profile_launch(const char *launcher, const char *name, GError **error)
{
    if (!profile_name_valid(name)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Use 1–64 letters, digits, dots, underscores or hyphens for the Profile name (excluding . and ..).");
        return NULL;
    }
    if (!launcher || !g_path_is_absolute(launcher) ||
        !g_file_test(launcher, G_FILE_TEST_IS_REGULAR) || g_access(launcher, X_OK) != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Select an executable launcher in Settings first.");
        return NULL;
    }
    if (profile_is_running(name)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "This Profile is already running or being maintained.");
        return NULL;
    }
    g_autoptr(GSubprocessLauncher) spawn = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_unsetenv(spawn, "LD_PRELOAD");
    g_subprocess_launcher_unsetenv(spawn, "LD_AUDIT");
    g_subprocess_launcher_unsetenv(spawn, "LD_LIBRARY_PATH");
    g_subprocess_launcher_set_child_setup(spawn, detach_launcher, NULL, NULL);
    return g_subprocess_launcher_spawn(spawn, error, launcher, "--profile", name, "--no-launch-ui", NULL);
}
