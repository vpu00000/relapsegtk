#include "scripts.h"
#include "store.h"

#include <gio/gio.h>

char *scripts_directory_default(void)
{
    return g_build_filename(g_get_home_dir(), "relapse-scripts-tmpdir", NULL);
}

char *scripts_settings_load(GError **error)
{
    g_autoptr(GKeyFile) file = store_load(FALSE, error);
    if (!file) return NULL;
    g_autofree char *path = g_key_file_get_string(file, "Scripts", "directory", NULL);
    if (path && *path && g_path_is_absolute(path)) return g_steal_pointer(&path);
    return scripts_directory_default();
}

gboolean scripts_settings_save(const char *path, GError **error)
{
    if (!path || !g_path_is_absolute(path)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Choose a local script folder.");
        return FALSE;
    }
    g_autoptr(GKeyFile) file = store_load(TRUE, error);
    if (!file) return FALSE;
    g_key_file_set_string(file, "Scripts", "directory", path);
    return store_save(file, error);
}
