#pragma once

#include <gio/gio.h>

typedef struct {
    char *default_profile;
    char *workspace;
    char *autoexec;
    char **autolaunch;
    gboolean raknet_enabled;
    gboolean workspace_override;
    gboolean autoexec_override;
    gboolean autolaunch_override;
    gboolean raknet_override;
} ProfileConfig;

/* A NULL profile loads or saves global settings; otherwise it selects overrides. */
ProfileConfig *profile_config_load(const char *profile, GError **error);
gboolean profile_config_save(const char *profile, const ProfileConfig *config, GError **error);
void profile_config_free(ProfileConfig *config);
gboolean profile_settings_save(const char *name, const ProfileConfig *config,
                               const char *data_dir, GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ProfileConfig, profile_config_free)
