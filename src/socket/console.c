#include "console.h"
#include "socket/transport.h"

#include <json-glib/json-glib.h>
#include <string.h>

struct ConsoleBridge {
    SocketClient *client;
    RelapseSocket *channel;
    GMainContext *context;
    GSource *timer;
    ConsoleBridgeHandlers handlers;
    gpointer user_data;
    GHashTable *requests;
    gint64 request_id, session, deadline;
    gboolean ready;
};

static void cancel_timer(ConsoleBridge *bridge)
{
    if (bridge->timer) {
        g_source_destroy(bridge->timer);
        g_clear_pointer(&bridge->timer, g_source_unref);
    }
}

static void stop(ConsoleBridge *bridge)
{
    cancel_timer(bridge);
    g_clear_pointer(&bridge->channel, relapse_socket_free);
    bridge->request_id = 0;
    bridge->ready = FALSE;
}

static void fail(ConsoleBridge *bridge, const char *detail)
{
    stop(bridge);
    if (bridge->handlers.error)
        bridge->handlers.error(detail, bridge->user_data);
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

static gboolean valid_id(JsonObject *object, gint64 *id)
{
    double number;
    if (member_is(object, "id", G_TYPE_INT64))
        number = (double)json_object_get_int_member(object, "id");
    else if (member_is(object, "id", G_TYPE_DOUBLE))
        number = json_object_get_double_member(object, "id");
    else
        return FALSE;
    if (number != number || number < 1 || number > G_MAXINT32) return FALSE;
    gint64 value = (gint64)number;
    if (number != (double)value) return FALSE;
    if (id) *id = value;
    return TRUE;
}

static gboolean valid_level(const char *level)
{
    return level && (g_str_equal(level, "print") || g_str_equal(level, "warn") ||
                     g_str_equal(level, "error") || g_str_equal(level, "info"));
}

static void show(ConsoleBridge *bridge, gboolean visible)
{
    if (bridge->handlers.show) bridge->handlers.show(visible, bridge->user_data);
}

static void message_received(RelapseSocket *socket, GBytes *message, gpointer user_data)
{
    (void)socket;
    ConsoleBridge *bridge = user_data;
    gsize size;
    const char *data = g_bytes_get_data(message, &size);
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!size || size > 65536 || memchr(data, 0, size) || !g_utf8_validate(data, size, NULL) ||
        !json_parser_load_from_data(parser, data, size, NULL) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
        goto invalid;
    JsonObject *object = json_node_get_object(json_parser_get_root(parser));
    const char *type = string_member(object, "type");
    if (!type) goto invalid;
    if (g_str_equal(type, "state")) {
        const char *title = string_member(object, "title");
        if (!title || strlen(title) > 255 || !member_is(object, "visible", G_TYPE_BOOLEAN)) goto invalid;
        if (bridge->handlers.title) bridge->handlers.title(title, bridge->user_data);
        show(bridge, json_object_get_boolean_member(object, "visible"));
    } else if (g_str_equal(type, "show")) {
        show(bridge, TRUE);
    } else if (g_str_equal(type, "hide")) {
        show(bridge, FALSE);
    } else if (g_str_equal(type, "title")) {
        const char *title = string_member(object, "text");
        if (!title || strlen(title) > 255) goto invalid;
        if (bridge->handlers.title) bridge->handlers.title(title, bridge->user_data);
    } else if (g_str_equal(type, "clear")) {
        if (bridge->handlers.clear) bridge->handlers.clear(bridge->user_data);
    } else if (g_str_equal(type, "print")) {
        const char *level = string_member(object, "level");
        const char *text = string_member(object, "text");
        if (!valid_level(level) || !text || strlen(text) > 4096) goto invalid;
        if (bridge->handlers.entry) bridge->handlers.entry(level, text, bridge->user_data);
    } else if (g_str_equal(type, "input")) {
        gint64 id;
        if (!valid_id(object, &id)) goto invalid;
        if (bridge->handlers.input) bridge->handlers.input(id, bridge->user_data);
    } else goto invalid;
    return;
invalid:
    fail(bridge, "Invalid console channel message");
}

static void connected(RelapseSocket *socket, gpointer user_data)
{
    (void)socket;
    ConsoleBridge *bridge = user_data;
    bridge->ready = TRUE;
    cancel_timer(bridge);
}

static gboolean tick(gpointer user_data)
{
    ConsoleBridge *bridge = user_data;
    if (g_get_monotonic_time() >= bridge->deadline) {
        fail(bridge, "Console channel setup timed out");
        return G_SOURCE_REMOVE;
    }
    if (relapse_socket_get_state(bridge->channel) == RELAPSE_SOCKET_CLOSED)
        relapse_socket_connect(bridge->channel, socket_client_get_path(bridge->client), "relapsegtk.console.v1", NULL);
    return G_SOURCE_CONTINUE;
}

static void start_timer(ConsoleBridge *bridge)
{
    cancel_timer(bridge);
    bridge->timer = g_timeout_source_new(250);
    g_source_set_callback(bridge->timer, tick, bridge, NULL);
    g_source_attach(bridge->timer, bridge->context);
}

static void reconnect(ConsoleBridge *bridge)
{
    bridge->deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    start_timer(bridge);
}

static void closed(RelapseSocket *socket, RelapseSocketCloseReason reason, const char *detail, gpointer user_data)
{
    (void)socket;
    (void)reason;
    (void)detail;
    ConsoleBridge *bridge = user_data;
    if (!bridge->ready) return;
    bridge->ready = FALSE;
    reconnect(bridge);
}

static void start(ConsoleBridge *bridge)
{
    stop(bridge);
    if (!bridge->session) return;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) resource = g_resources_lookup_data("/org/relapse/RelapseGtk/scripts/console.luau",
                                                       G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
    if (!resource) {
        fail(bridge, error->message);
        return;
    }
    gsize length;
    const char *data = g_bytes_get_data(resource, &length);
    g_autofree char *source = g_strndup(data, length);
    if (socket_client_execute(bridge->client, "relapsegtk-console.luau", source, &bridge->request_id) != SOCKET_CLIENT_QUEUED) {
        fail(bridge, "Could not start the console helper");
        return;
    }
    if (g_hash_table_size(bridge->requests) >= 16)
        g_hash_table_remove_all(bridge->requests);
    gint64 *id = g_new(gint64, 1);
    *id = bridge->request_id;
    g_hash_table_add(bridge->requests, id);
    RelapseSocketHandlers handlers = {.onConnect = connected, .onMessage = message_received, .onClose = closed};
    bridge->channel = relapse_socket_new(&handlers, bridge);
    reconnect(bridge);
    tick(bridge);
}

ConsoleBridge *console_bridge_new(SocketClient *client, const ConsoleBridgeHandlers *handlers, gpointer user_data)
{
    ConsoleBridge *bridge = g_new0(ConsoleBridge, 1);
    bridge->client = client;
    bridge->context = g_main_context_ref_thread_default();
    bridge->requests = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    if (handlers) bridge->handlers = *handlers;
    bridge->user_data = user_data;
    return bridge;
}

void console_bridge_sync(ConsoleBridge *bridge)
{
    gint64 session = socket_client_get_session(bridge->client);
    if (bridge->session == session) return;
    bridge->session = session;
    if (session) start(bridge);
    else stop(bridge);
}

gboolean console_bridge_result(ConsoleBridge *bridge, gint64 id, gboolean submitted, const char *detail)
{
    if (!g_hash_table_remove(bridge->requests, &id)) return FALSE;
    if (id == bridge->request_id) {
        bridge->request_id = 0;
        if (!submitted) fail(bridge, detail ? detail : "Console helper setup failed");
    }
    return TRUE;
}

void console_bridge_input(ConsoleBridge *bridge, gint64 id, const char *text)
{
    if (!bridge->ready || id < 1 || id > G_MAXINT32 || (text && (!g_utf8_validate(text, -1, NULL) || strlen(text) > 4096))) return;
    JsonObject *object = json_object_new();
    json_object_set_string_member(object, "op", "input_response");
    json_object_set_int_member(object, "id", id);
    json_object_set_string_member(object, "text", text ? text : "");
    g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, object);
    g_autofree char *json = json_to_string(root, FALSE);
    g_autoptr(GBytes) message = g_bytes_new(json, strlen(json));
    if (relapse_socket_send(bridge->channel, message) != RELAPSE_SOCKET_QUEUED)
        fail(bridge, "Could not answer the console input request");
}

void console_bridge_free(ConsoleBridge *bridge)
{
    if (!bridge) return;
    stop(bridge);
    g_hash_table_unref(bridge->requests);
    g_main_context_unref(bridge->context);
    g_free(bridge);
}
