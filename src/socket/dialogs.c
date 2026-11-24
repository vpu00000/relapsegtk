#include "dialogs.h"
#include "socket/transport.h"

#include <json-glib/json-glib.h>
#include <string.h>

struct DialogBridge {
    SocketClient *client;
    RelapseSocket *channel;
    GMainContext *context;
    GSource *timer;
    DialogBridgeHandlers handlers;
    gpointer user_data;
    GHashTable *requests, *dialogs;
    gint64 request_id, session, deadline;
    gboolean ready;
};

static void set_ready(DialogBridge *bridge, gboolean ready)
{
    if (bridge->ready == ready) return;
    bridge->ready = ready;
    if (bridge->handlers.changed) bridge->handlers.changed(bridge->user_data);
}

static void reset(DialogBridge *bridge)
{
    g_hash_table_remove_all(bridge->dialogs);
    if (bridge->handlers.reset) bridge->handlers.reset(bridge->user_data);
}

static void cancel_timer(DialogBridge *bridge)
{
    if (bridge->timer) {
        g_source_destroy(bridge->timer);
        g_clear_pointer(&bridge->timer, g_source_unref);
    }
}

static void stop(DialogBridge *bridge)
{
    cancel_timer(bridge);
    g_clear_pointer(&bridge->channel, relapse_socket_free);
    bridge->request_id = 0;
    reset(bridge);
    set_ready(bridge, FALSE);
}

static void fail(DialogBridge *bridge, const char *detail)
{
    stop(bridge);
    if (bridge->handlers.error) bridge->handlers.error(detail, bridge->user_data);
}

static const char *string_member(JsonObject *object, const char *key, gsize limit)
{
    JsonNode *node = json_object_get_member(object, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) return NULL;
    const char *text = json_node_get_string(node);
    return text && strlen(text) <= limit ? text : NULL;
}

static gboolean integer_member(JsonObject *object, const char *key, gint64 minimum, gint64 maximum, gint64 *result)
{
    JsonNode *node = json_object_get_member(object, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node)) return FALSE;
    GType type = json_node_get_value_type(node);
    if (type != G_TYPE_INT64 && type != G_TYPE_DOUBLE) return FALSE;
    double number = json_node_get_double(node);
    if (number != number || number < minimum || number > maximum) return FALSE;
    gint64 value = (gint64)number;
    if (number != (double)value) return FALSE;
    *result = value;
    return TRUE;
}

static gboolean valid_icon(const char *icon)
{
    return icon && (g_str_equal(icon, "none") || g_str_equal(icon, "error") || g_str_equal(icon, "question") ||
                    g_str_equal(icon, "warning") || g_str_equal(icon, "information"));
}

static void message_received(RelapseSocket *socket, GBytes *message, gpointer user_data)
{
    (void)socket;
    DialogBridge *bridge = user_data;
    gsize size;
    const char *data = g_bytes_get_data(message, &size);
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!size || size > 65536 || memchr(data, 0, size) || !g_utf8_validate(data, size, NULL) ||
        !json_parser_load_from_data(parser, data, size, NULL) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) goto invalid;
    JsonObject *object = json_node_get_object(json_parser_get_root(parser));
    const char *type = string_member(object, "type", 16);
    gint64 id;
    if (!type || !integer_member(object, "id", 1, G_MAXINT32, &id)) goto invalid;
    if (g_str_equal(type, "close")) {
        if (g_hash_table_remove(bridge->dialogs, &id) && bridge->handlers.close)
            bridge->handlers.close(id, bridge->user_data);
    } else if (g_str_equal(type, "open")) {
        const char *title = string_member(object, "title", 255);
        const char *text = string_member(object, "text", 4096);
        const char *icon = string_member(object, "icon", 16);
        JsonNode *node = json_object_get_member(object, "buttons");
        if (!title || !text || !valid_icon(icon) || !node || !JSON_NODE_HOLDS_ARRAY(node) ||
            g_hash_table_contains(bridge->dialogs, &id) || g_hash_table_size(bridge->dialogs) >= 64) goto invalid;
        JsonArray *buttons = json_node_get_array(node);
        guint count = json_array_get_length(buttons);
        gint64 default_button;
        if (!count || count > 16 || !integer_member(object, "default", 1, count, &default_button)) goto invalid;
        const char *labels[17] = {0};
        for (guint i = 0; i < count; ++i) {
            JsonNode *label = json_array_get_element(buttons, i);
            if (!JSON_NODE_HOLDS_VALUE(label) || json_node_get_value_type(label) != G_TYPE_STRING) goto invalid;
            labels[i] = json_node_get_string(label);
            if (!labels[i] || !*labels[i] || strlen(labels[i]) > 128) goto invalid;
        }
        gint64 *key = g_new(gint64, 1);
        *key = id;
        g_hash_table_insert(bridge->dialogs, key, GUINT_TO_POINTER(count));
        if (bridge->handlers.open)
            bridge->handlers.open(id, title, text, labels, icon, (guint)default_button, bridge->user_data);
    } else goto invalid;
    return;
invalid:
    fail(bridge, "Invalid dialog channel message");
}

static void connected(RelapseSocket *socket, gpointer user_data)
{
    (void)socket;
    DialogBridge *bridge = user_data;
    cancel_timer(bridge);
    set_ready(bridge, TRUE);
}

static gboolean tick(gpointer user_data)
{
    DialogBridge *bridge = user_data;
    if (g_get_monotonic_time() >= bridge->deadline) {
        fail(bridge, "Dialog channel setup timed out");
        return G_SOURCE_REMOVE;
    }
    if (relapse_socket_get_state(bridge->channel) == RELAPSE_SOCKET_CLOSED)
        relapse_socket_connect(bridge->channel, socket_client_get_path(bridge->client), "relapsegtk.dialogs.v1", NULL);
    return G_SOURCE_CONTINUE;
}

static void reconnect(DialogBridge *bridge)
{
    cancel_timer(bridge);
    bridge->deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    bridge->timer = g_timeout_source_new(250);
    g_source_set_callback(bridge->timer, tick, bridge, NULL);
    g_source_attach(bridge->timer, bridge->context);
}

static void closed(RelapseSocket *socket, RelapseSocketCloseReason reason, const char *detail, gpointer user_data)
{
    (void)socket; (void)reason; (void)detail;
    DialogBridge *bridge = user_data;
    if (!bridge->ready) return;
    reset(bridge);
    set_ready(bridge, FALSE);
    reconnect(bridge);
}

static void start(DialogBridge *bridge)
{
    stop(bridge);
    if (!bridge->session) return;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) resource = g_resources_lookup_data("/org/relapse/RelapseGtk/scripts/dialogs.luau",
                                                       G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
    if (!resource) { fail(bridge, error->message); return; }
    gsize length;
    const char *data = g_bytes_get_data(resource, &length);
    g_autofree char *source = g_strndup(data, length);
    if (socket_client_execute(bridge->client, "relapsegtk-dialogs.luau", source, &bridge->request_id) != SOCKET_CLIENT_QUEUED) {
        fail(bridge, "Could not start the dialog helper");
        return;
    }
    if (g_hash_table_size(bridge->requests) >= 16) g_hash_table_remove_all(bridge->requests);
    gint64 *id = g_new(gint64, 1);
    *id = bridge->request_id;
    g_hash_table_add(bridge->requests, id);
    RelapseSocketHandlers handlers = {.onConnect = connected, .onMessage = message_received, .onClose = closed};
    bridge->channel = relapse_socket_new(&handlers, bridge);
    reconnect(bridge);
    tick(bridge);
}

DialogBridge *dialog_bridge_new(SocketClient *client, const DialogBridgeHandlers *handlers, gpointer user_data)
{
    DialogBridge *bridge = g_new0(DialogBridge, 1);
    bridge->client = client;
    bridge->context = g_main_context_ref_thread_default();
    bridge->requests = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    bridge->dialogs = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    if (handlers) bridge->handlers = *handlers;
    bridge->user_data = user_data;
    return bridge;
}

void dialog_bridge_sync(DialogBridge *bridge)
{
    gint64 session = socket_client_get_session(bridge->client);
    if (bridge->session == session) return;
    bridge->session = session;
    if (session) start(bridge); else stop(bridge);
}

gboolean dialog_bridge_is_ready(DialogBridge *bridge)
{
    return bridge && bridge->ready;
}

gboolean dialog_bridge_result(DialogBridge *bridge, gint64 id, gboolean submitted, const char *detail)
{
    if (!g_hash_table_remove(bridge->requests, &id)) return FALSE;
    if (id == bridge->request_id) {
        bridge->request_id = 0;
        if (!submitted) fail(bridge, detail ? detail : "Dialog helper setup failed");
    }
    return TRUE;
}

void dialog_bridge_choice(DialogBridge *bridge, gint64 id, guint choice)
{
    guint count = GPOINTER_TO_UINT(g_hash_table_lookup(bridge->dialogs, &id));
    if (!bridge->ready || !count || choice > count) return;
    g_hash_table_remove(bridge->dialogs, &id);
    g_autofree char *json = g_strdup_printf("{\"op\":\"choice\",\"id\":%" G_GINT64_FORMAT ",\"choice\":%u}", id, choice);
    g_autoptr(GBytes) message = g_bytes_new(json, strlen(json));
    if (relapse_socket_send(bridge->channel, message) != RELAPSE_SOCKET_QUEUED)
        fail(bridge, "Could not answer the dialog request");
}

void dialog_bridge_free(DialogBridge *bridge)
{
    if (!bridge) return;
    bridge->handlers.changed = NULL;
    stop(bridge);
    g_hash_table_unref(bridge->requests);
    g_hash_table_unref(bridge->dialogs);
    g_main_context_unref(bridge->context);
    g_free(bridge);
}
