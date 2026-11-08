#include "bridge.h"
#include "socket/transport.h"

#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

struct UiBridge {
    SocketClient *client;
    RelapseSocket *channel;
    GMainContext *context;
    GSource *timer;
    UiBridgeHandlers handlers;
    gpointer user_data;
    GHashTable *requests;
    gint64 request_id, session, deadline;
    OutputState state;
    gboolean enabled, ready;
};

static void set_state(UiBridge *bridge, OutputState state, const char *detail)
{
    bridge->state = state;
    if (bridge->handlers.state_changed)
        bridge->handlers.state_changed(state, detail, bridge->user_data);
}

static void start_timer(UiBridge *bridge);

static void cancel_timer(UiBridge *bridge)
{
    if (bridge->timer) {
        g_source_destroy(bridge->timer);
        g_clear_pointer(&bridge->timer, g_source_unref);
    }
}

static void stop(UiBridge *bridge)
{
    cancel_timer(bridge);
    g_clear_pointer(&bridge->channel, relapse_socket_free);
    bridge->request_id = 0;
    bridge->ready = FALSE;
}

static void fail(UiBridge *bridge, const char *detail)
{
    stop(bridge);
    set_state(bridge, OUTPUT_FAILED, detail);
}

static void configure(UiBridge *bridge)
{
    g_autofree char *json = g_strdup_printf("{\"op\":\"configure\",\"version\":2,\"redirect\":%s}",
                                          bridge->enabled ? "true" : "false");
    g_autoptr(GBytes) message = g_bytes_new(json, strlen(json));
    bridge->ready = FALSE;
    if (relapse_socket_send(bridge->channel, message) != RELAPSE_SOCKET_QUEUED) {
        fail(bridge, "Could not configure the UI channel. Toggle redirection to retry.");
        return;
    }
    bridge->deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    set_state(bridge, OUTPUT_CONNECTING, "Connecting…");
    start_timer(bridge);
}

static void connected(RelapseSocket *socket, gpointer user_data)
{
    (void)socket;
    configure(user_data);
}

static void closed(RelapseSocket *socket, RelapseSocketCloseReason reason, const char *detail, gpointer user_data)
{
    (void)socket; (void)reason; (void)detail;
    UiBridge *bridge = user_data;
    if (bridge->ready) {
        bridge->ready = FALSE;
        bridge->deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
        set_state(bridge, OUTPUT_CONNECTING, "Reconnecting…");
        start_timer(bridge);
    }
}

static gboolean member_is(JsonObject *object, const char *key, GType type)
{
    JsonNode *node = json_object_get_member(object, key);
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == type;
}

static const char *string_member(JsonObject *object, const char *key)
{
    return member_is(object, key, G_TYPE_STRING) ? json_object_get_string_member(object, key) : NULL;
}

static gboolean integer_member(JsonObject *object, const char *key, gint32 minimum,
                               gint32 maximum, gint64 *result)
{
    gint64 value;
    if (member_is(object, key, G_TYPE_INT64)) {
        value = json_object_get_int_member(object, key);
    } else if (member_is(object, key, G_TYPE_DOUBLE)) {
        double number = json_object_get_double_member(object, key);
        if (!isfinite(number) || number < minimum || number > maximum)
            return FALSE;
        value = (gint64)number;
        if (number != (double)value)
            return FALSE;
    } else {
        return FALSE;
    }
    if (value < minimum || value > maximum)
        return FALSE;
    if (result)
        *result = value;
    return TRUE;
}

static gboolean valid_level(const char *level)
{
    return level && (g_str_equal(level, "print") || g_str_equal(level, "warn") ||
                     g_str_equal(level, "error") || g_str_equal(level, "info"));
}

static void dropped(UiBridge *bridge, gint64 count)
{
    if (count > 0 && bridge->handlers.entry) {
        g_autofree char *text = g_strdup_printf("%" G_GINT64_FORMAT " messages dropped: output arrived too quickly.", count);
        bridge->handlers.entry("warn", text, bridge->user_data);
    }
}

static void message_received(RelapseSocket *socket, GBytes *message, gpointer user_data)
{
    (void)socket;
    UiBridge *bridge = user_data;
    gsize size;
    const char *data = g_bytes_get_data(message, &size);
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!size || size > 1024 * 1024 || memchr(data, 0, size) || !g_utf8_validate(data, size, NULL) ||
        !json_parser_load_from_data(parser, data, size, NULL) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
        goto invalid;
    JsonObject *object = json_node_get_object(json_parser_get_root(parser));
    const char *type = string_member(object, "type");
    if (!type) goto invalid;
    if (g_str_equal(type, "configured")) {
        if (!integer_member(object, "version", 2, 2, NULL) || !member_is(object, "redirect", G_TYPE_BOOLEAN)) goto invalid;
        if (json_object_get_boolean_member(object, "redirect") != bridge->enabled) return;
        cancel_timer(bridge);
        bridge->ready = TRUE;
        set_state(bridge, bridge->enabled ? OUTPUT_ACTIVE : OUTPUT_OFF, bridge->enabled ? "Capturing" : "Output off");
    } else if (g_str_equal(type, "failed")) {
        const char *detail = string_member(object, "detail");
        if (!detail || strlen(detail) > 4096) goto invalid;
        fail(bridge, detail);
    } else if (g_str_equal(type, "output")) {
        JsonNode *node = json_object_get_member(object, "entries");
        gint64 lost;
        if (!node || !JSON_NODE_HOLDS_ARRAY(node) ||
            !integer_member(object, "dropped", 0, G_MAXINT32, &lost)) goto invalid;
        JsonArray *entries = json_node_get_array(node);
        guint length = json_array_get_length(entries);
        if (!length || length > 32) goto invalid;
        for (guint i = 0; i < length; ++i) {
            JsonNode *entry = json_array_get_element(entries, i);
            if (!JSON_NODE_HOLDS_OBJECT(entry)) goto invalid;
            JsonObject *row = json_node_get_object(entry);
            const char *text = string_member(row, "text");
            if (!valid_level(string_member(row, "level")) || !text || strlen(text) > 4096) goto invalid;
        }
        if (!bridge->enabled) return;
        for (guint i = 0; i < length; ++i) {
            JsonObject *row = json_array_get_object_element(entries, i);
            if (bridge->handlers.entry)
                bridge->handlers.entry(string_member(row, "level"), string_member(row, "text"), bridge->user_data);
        }
        dropped(bridge, lost);
    } else if (g_str_equal(type, "dropped")) {
        gint64 count;
        if (!integer_member(object, "count", 1, G_MAXINT32, &count)) goto invalid;
        if (bridge->enabled) dropped(bridge, count);
    } else goto invalid;
    return;
invalid:
    fail(bridge, "Invalid UI channel message. Toggle redirection to retry.");
}

static gboolean tick(gpointer user_data)
{
    UiBridge *bridge = user_data;
    if (bridge->ready)
        return G_SOURCE_CONTINUE;
    if (g_get_monotonic_time() >= bridge->deadline) {
        fail(bridge, "UI channel setup timed out. Toggle redirection to retry.");
        return G_SOURCE_REMOVE;
    }
    if (relapse_socket_get_state(bridge->channel) == RELAPSE_SOCKET_CLOSED)
        relapse_socket_connect(bridge->channel, socket_client_get_path(bridge->client), "relapsegtk.session.v2", NULL);
    return G_SOURCE_CONTINUE;
}

static void start_timer(UiBridge *bridge)
{
    cancel_timer(bridge);
    bridge->timer = g_timeout_source_new(250);
    g_source_set_callback(bridge->timer, tick, bridge, NULL);
    g_source_attach(bridge->timer, bridge->context);
}

static void start(UiBridge *bridge)
{
    stop(bridge);
    if (!bridge->enabled) {
        set_state(bridge, OUTPUT_OFF, "Output off");
        return;
    }
    if (!bridge->session) {
        set_state(bridge, OUTPUT_WAITING, "Waiting for a game");
        return;
    }
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) resource = g_resources_lookup_data("/org/relapse/RelapseGtk/scripts/session.luau",
                                                       G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
    if (!resource) {
        fail(bridge, error->message);
        return;
    }
    gsize length;
    const char *data = g_bytes_get_data(resource, &length);
    g_autofree char *source = g_strndup(data, length);
    if (socket_client_execute(bridge->client, "relapsegtk-session.luau", source, &bridge->request_id) != SOCKET_CLIENT_QUEUED) {
        fail(bridge, "Could not start the UI channel. Toggle redirection to retry.");
        return;
    }
    if (g_hash_table_size(bridge->requests) >= 256)
        g_hash_table_remove_all(bridge->requests);
    gint64 *id = g_new(gint64, 1);
    *id = bridge->request_id;
    g_hash_table_add(bridge->requests, id);
    RelapseSocketHandlers handlers = {.onConnect = connected, .onMessage = message_received, .onClose = closed};
    bridge->channel = relapse_socket_new(&handlers, bridge);
    bridge->deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    set_state(bridge, OUTPUT_CONNECTING, "Starting UI channel…");
    start_timer(bridge);
    tick(bridge);
}

UiBridge *ui_bridge_new(SocketClient *client, const UiBridgeHandlers *handlers, gpointer user_data)
{
    UiBridge *bridge = g_new0(UiBridge, 1);
    bridge->client = client;
    bridge->context = g_main_context_ref_thread_default();
    bridge->requests = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    if (handlers) bridge->handlers = *handlers;
    bridge->user_data = user_data;
    return bridge;
}

void ui_bridge_set_output_enabled(UiBridge *bridge, gboolean enabled)
{
    enabled = !!enabled;
    if (bridge->enabled == enabled) return;
    bridge->enabled = enabled;
    bridge->session = socket_client_get_session(bridge->client);
    start(bridge);
}

void ui_bridge_sync(UiBridge *bridge)
{
    gint64 session = socket_client_get_session(bridge->client);
    if (bridge->session == session) return;
    bridge->session = session;
    start(bridge);
}

gboolean ui_bridge_result(UiBridge *bridge, gint64 id, gboolean submitted, const char *detail)
{
    if (!g_hash_table_remove(bridge->requests, &id)) return FALSE;
    if (id == bridge->request_id && !submitted)
        fail(bridge, detail ? detail : "UI channel setup failed.");
    return TRUE;
}

gboolean ui_bridge_is_ready(UiBridge *bridge)
{
    return bridge && socket_client_is_ready(bridge->client) &&
        (!bridge->enabled || (bridge->ready &&
         relapse_socket_get_state(bridge->channel) == RELAPSE_SOCKET_OPEN));
}

static void append_lua_string(GString *script, const char *text)
{
    g_string_append_c(script, '"');
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            g_string_append_c(script, '\\');
            g_string_append_c(script, *p);
        } else if (*p < 32 || *p == 127) {
            g_string_append_printf(script, "\\%03u", *p);
        } else {
            g_string_append_c(script, *p);
        }
    }
    g_string_append_c(script, '"');
}

SocketClientSendResult ui_bridge_execute(UiBridge *bridge, const char *name, const char *source)
{
    if (!ui_bridge_is_ready(bridge)) return SOCKET_CLIENT_NOT_READY;
    if (!bridge->enabled)
        return socket_client_execute(bridge->client, name, source, NULL);
    g_autoptr(GString) script = g_string_new("getgenv().__relapsegtk_session_v2.execute(");
    append_lua_string(script, source);
    g_string_append_c(script, ',');
    append_lua_string(script, name && strlen(name) <= 255 ? name : "editor.luau");
    g_string_append_c(script, ')');
    return socket_client_execute(bridge->client, "relapsegtk-execute.luau", script->str, NULL);
}

void ui_bridge_free(UiBridge *bridge)
{
    if (!bridge) return;
    stop(bridge);
    g_hash_table_unref(bridge->requests);
    g_main_context_unref(bridge->context);
    g_free(bridge);
}
