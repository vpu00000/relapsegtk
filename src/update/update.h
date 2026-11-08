#pragma once

#include <gio/gio.h>

typedef struct {
    char *version;
    char *appimage_url;
    char *signature_url;
} UpdateInfo;

typedef void (*UpdateCheckFunc)(const UpdateInfo *info, const char *error, gpointer user_data);
typedef void (*UpdateInstallFunc)(const char *error, gpointer user_data);

gboolean update_is_configured(void);
const char *update_current_version(void);
const char *update_repository(void);
UpdateInfo *update_info_copy(const UpdateInfo *info);
void update_info_free(UpdateInfo *info);
void update_check_async(UpdateCheckFunc callback, gpointer user_data);
void update_install_async(const UpdateInfo *info, UpdateInstallFunc callback, gpointer user_data);
