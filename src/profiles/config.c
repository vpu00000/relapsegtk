#include "config.h"
#include "store.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

static char *global_config_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "relapse", "config.json", NULL);
}

static char *profile_config_path(const char *name)
{
    g_autofree char *directory = g_build_filename(
        g_get_user_config_dir(), "relapse", "profiles", name, NULL);
    return g_build_filename(directory, "config.json", NULL);
}

static JsonNode *read_object(const char *path, GError **error)
{
    g_autofree char *contents = NULL;
    gsize size = 0;
    g_autoptr(GError) failure = NULL;
    if (!g_file_get_contents(path, &contents, &size, &failure)) {
        if (g_error_matches(failure, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            JsonNode *empty = json_node_new(JSON_NODE_OBJECT);
            json_node_take_object(empty, json_object_new());
            return empty;
        }
        g_propagate_error(error, g_steal_pointer(&failure));
        return NULL;
    }
    if (size > 1024 * 1024 || (size && memchr(contents, 0, size))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "Configuration files must be UTF-8 JSON smaller than 1 MiB.");
        return NULL;
    }
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, contents, size, error)) return NULL;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "Relapse configuration must be a JSON object.");
        return NULL;
    }
    return json_node_copy(root);
}

static gboolean save_object(const char *path, JsonObject *object, GError **error)
{
    g_autofree char *directory = g_path_get_dirname(path);
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        int failure = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(failure),
            "Could not create Relapse configuration directory: %s", g_strerror(failure));
        return FALSE;
    }
    g_autoptr(JsonGenerator) generator = json_generator_new();
    json_generator_set_pretty(generator, TRUE);
    json_generator_set_indent(generator, 2);
    json_generator_set_indent_char(generator, ' ');
    JsonNode *root = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(root, object);
    json_generator_set_root(generator, root);
    gsize size = 0;
    g_autofree char *contents = json_generator_to_data(generator, &size);
    json_node_free(root);
    return g_file_set_contents_full(path, contents, size,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
}

static gboolean read_path(JsonObject *object, const char *key, const char *fallback,
                          char **value, GError **error)
{
    JsonNode *member = json_object_get_member(object, key);
    if (!member) {
        *value = g_strdup(fallback);
        return TRUE;
    }
    if (!JSON_NODE_HOLDS_VALUE(member) || json_node_get_value_type(member) != G_TYPE_STRING) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s must be a string.", key);
        return FALSE;
    }
    const char *path = json_node_get_string(member);
    if (!path || !*path || !g_path_is_absolute(path)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "%s must be an absolute path.", key);
        return FALSE;
    }
    *value = g_strdup(path);
    return TRUE;
}

static gboolean read_raknet(JsonObject *object, gboolean *enabled, GError **error)
{
    JsonNode *member = json_object_get_member(object, "raknet");
    if (!member) return TRUE;
    if (!JSON_NODE_HOLDS_OBJECT(member)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "raknet must be an object to edit its enabled setting.");
        return FALSE;
    }
    JsonNode *value = json_object_get_member(json_node_get_object(member), "enabled");
    if (!value) return TRUE;
    if (!JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_BOOLEAN) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "raknet.enabled must be a boolean.");
        return FALSE;
    }
    *enabled = json_node_get_boolean(value);
    return TRUE;
}

static gboolean read_autolaunch(JsonObject *object, char ***command, GError **error)
{
    JsonNode *member = json_object_get_member(object, "autolaunch");
    if (!member || JSON_NODE_HOLDS_NULL(member)) {
        *command = NULL;
        return TRUE;
    }
    if (!JSON_NODE_HOLDS_ARRAY(member)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "autolaunch must be null or an argument array.");
        return FALSE;
    }
    JsonArray *array = json_node_get_array(member);
    guint length = json_array_get_length(array);
    if (!length) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "autolaunch must not be empty.");
        return FALSE;
    }
    char **result = g_new0(char *, length + 1);
    for (guint i = 0; i < length; ++i) {
        JsonNode *argument = json_array_get_element(array, i);
        if (!JSON_NODE_HOLDS_VALUE(argument) || json_node_get_value_type(argument) != G_TYPE_STRING) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "autolaunch arguments must be strings.");
            g_strfreev(result);
            return FALSE;
        }
        result[i] = g_strdup(json_node_get_string(argument));
    }
    if (!*result[0]) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "autolaunch program cannot be empty.");
        g_strfreev(result);
        return FALSE;
    }
    *command = result;
    return TRUE;
}

static gboolean load_global(ProfileConfig *config, GError **error)
{
    g_autofree char *workspace = g_build_filename(g_get_user_data_dir(), "relapse", "workspace", NULL);
    g_autofree char *autoexec = g_build_filename(g_get_user_config_dir(), "relapse", "autoexec", NULL);
    g_autofree char *path = global_config_file();
    g_autoptr(JsonNode) root = read_object(path, error);
    if (!root) return FALSE;
    JsonObject *object = json_node_get_object(root);
    JsonNode *selected = json_object_get_member(object, "defaultProfile");
    if (selected) {
        if (!JSON_NODE_HOLDS_VALUE(selected) || json_node_get_value_type(selected) != G_TYPE_STRING) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "defaultProfile must be a string.");
            return FALSE;
        }
        const char *name = json_node_get_string(selected);
        if (!profile_name_valid(name)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "defaultProfile is not a valid profile name.");
            return FALSE;
        }
        config->default_profile = g_strdup(name);
    } else config->default_profile = g_strdup("Default");
    if (!read_path(object, "workspace", workspace, &config->workspace, error) ||
        !read_path(object, "autoexec", autoexec, &config->autoexec, error) ||
        !read_autolaunch(object, &config->autolaunch, error) ||
        !read_raknet(object, &config->raknet_enabled, error)) return FALSE;
    return TRUE;
}

ProfileConfig *profile_config_load(const char *profile, GError **error)
{
    if (profile && !profile_name_valid(profile)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid profile name.");
        return NULL;
    }
    g_autoptr(ProfileConfig) config = g_new0(ProfileConfig, 1);
    if (!load_global(config, error)) return NULL;
    if (!profile) return g_steal_pointer(&config);
    g_autofree char *path = profile_config_path(profile);
    g_autoptr(JsonNode) root = read_object(path, error);
    if (!root) return NULL;
    JsonObject *object = json_node_get_object(root);
    JsonNode *member = json_object_get_member(object, "override");
    if (!member) return g_steal_pointer(&config);
    if (!JSON_NODE_HOLDS_OBJECT(member)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "Profile override must be an object.");
        return NULL;
    }
    JsonObject *overrides = json_node_get_object(member);
    if (json_object_has_member(overrides, "workspace")) {
        config->workspace_override = TRUE;
        g_free(config->workspace);
        config->workspace = NULL;
        if (!read_path(overrides, "workspace", NULL, &config->workspace, error)) return NULL;
    }
    if (json_object_has_member(overrides, "autoexec")) {
        config->autoexec_override = TRUE;
        g_free(config->autoexec);
        config->autoexec = NULL;
        if (!read_path(overrides, "autoexec", NULL, &config->autoexec, error)) return NULL;
    }
    if (json_object_has_member(overrides, "autolaunch")) {
        config->autolaunch_override = TRUE;
        g_strfreev(config->autolaunch);
        config->autolaunch = NULL;
        if (!read_autolaunch(overrides, &config->autolaunch, error)) return NULL;
    }
    JsonNode *raknet = json_object_get_member(overrides, "raknet");
    if (raknet) {
        if (!JSON_NODE_HOLDS_OBJECT(raknet)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Profile raknet override must be an object.");
            return NULL;
        }
        if (json_object_has_member(json_node_get_object(raknet), "enabled")) {
            config->raknet_override = TRUE;
            if (!read_raknet(overrides, &config->raknet_enabled, error)) return NULL;
        }
    }
    return g_steal_pointer(&config);
}

static gboolean config_valid(const char *profile, const ProfileConfig *config, GError **error)
{
    if (!config || !config->workspace || !g_path_is_absolute(config->workspace) ||
        !config->autoexec || !g_path_is_absolute(config->autoexec)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Workspace and Autoexec must be absolute paths.");
        return FALSE;
    }
    if (!profile && !profile_name_valid(config->default_profile)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Choose a valid default profile.");
        return FALSE;
    }
    if (config->autolaunch && (!config->autolaunch[0] || !*config->autolaunch[0])) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Choose an autolaunch executable or disable autolaunch.");
        return FALSE;
    }
    return TRUE;
}

static void set_autolaunch(JsonObject *object, char *const *command)
{
    if (!command) {
        json_object_set_null_member(object, "autolaunch");
        return;
    }
    g_autoptr(JsonArray) array = json_array_new();
    for (guint i = 0; command[i]; ++i) json_array_add_string_element(array, command[i]);
    json_object_set_array_member(object, "autolaunch", g_steal_pointer(&array));
}

static void set_raknet(JsonObject *object, gboolean enabled)
{
    JsonObject *raknet = NULL;
    JsonNode *member = json_object_get_member(object, "raknet");
    if (member && JSON_NODE_HOLDS_OBJECT(member)) raknet = json_node_get_object(member);
    if (!raknet) {
        json_object_set_object_member(object, "raknet", json_object_new());
        raknet = json_object_get_object_member(object, "raknet");
    }
    json_object_set_boolean_member(raknet, "enabled", enabled);
}

static gboolean config_save(const char *profile, const ProfileConfig *config,
                             const char *data_dir, GError **error)
{
    if ((profile && !profile_name_valid(profile)) || !config_valid(profile, config, error)) {
        if (profile && !profile_name_valid(profile) && error && !*error)
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid profile name.");
        return FALSE;
    }
    g_autofree char *path = profile ? profile_config_path(profile) : global_config_file();
    g_autoptr(JsonNode) root = read_object(path, error);
    if (!root) return FALSE;
    JsonObject *object = json_node_get_object(root);
    if (!profile) {
        json_object_set_string_member(object, "defaultProfile", config->default_profile);
        json_object_set_string_member(object, "workspace", config->workspace);
        json_object_set_string_member(object, "autoexec", config->autoexec);
        set_autolaunch(object, config->autolaunch);
        set_raknet(object, config->raknet_enabled);
        return save_object(path, object, error);
    }
    JsonNode *member = json_object_get_member(object, "override");
    if (member && !JSON_NODE_HOLDS_OBJECT(member)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "Profile override must be an object.");
        return FALSE;
    }
    if (!member) json_object_set_object_member(object, "override", json_object_new());
    JsonObject *overrides = json_object_get_object_member(object, "override");
    if (config->workspace_override)
        json_object_set_string_member(overrides, "workspace", config->workspace);
    else json_object_remove_member(overrides, "workspace");
    if (config->autoexec_override)
        json_object_set_string_member(overrides, "autoexec", config->autoexec);
    else json_object_remove_member(overrides, "autoexec");
    if (config->autolaunch_override) set_autolaunch(overrides, config->autolaunch);
    else json_object_remove_member(overrides, "autolaunch");
    JsonNode *raknet = json_object_get_member(overrides, "raknet");
    if (raknet && !JSON_NODE_HOLDS_OBJECT(raknet)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "Profile raknet override must be an object.");
        return FALSE;
    }
    if (config->raknet_override) set_raknet(overrides, config->raknet_enabled);
    else if (raknet) json_object_remove_member(json_node_get_object(raknet), "enabled");
    if (data_dir) json_object_set_string_member(object, "dataDir", data_dir);
    return save_object(path, object, error);
}

gboolean profile_config_save(const char *profile, const ProfileConfig *config, GError **error)
{
    return config_save(profile, config, NULL, error);
}

void profile_config_free(ProfileConfig *config)
{
    if (!config) return;
    g_free(config->default_profile);
    g_free(config->workspace);
    g_free(config->autoexec);
    g_strfreev(config->autolaunch);
    g_free(config);
}

gboolean profile_settings_save(const char *name, const ProfileConfig *config,
                               const char *data_dir, GError **error)
{
    if (!profile_name_valid(name) || !data_dir || !*data_dir || !g_path_is_absolute(data_dir)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "The profile data directory must be an absolute path.");
        return FALSE;
    }
    return config_save(name, config, data_dir, error);
}
