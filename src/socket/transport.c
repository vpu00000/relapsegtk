#include "transport.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/un.h>

enum {
    ProtocolVersion = 1,
    MaxOpeningBytes = 4096,
    MaxMessageBytes = 8 * 1024 * 1024,
    MaxQueuedBytes = 16 * 1024 * 1024,
    MaxQueuedMessages = 256,
    IoBytesPerTurn = 256 * 1024,
    FramesPerTurn = 64,
    OpeningTimeoutMs = 5000
};

struct RelapseSocket {
    guint references;
    guint64 generation;
    GMainContext *context;
    RelapseSocketHandlers handlers;
    gpointer user_data;
    RelapseSocketState state;
    GSocket *connection;
    GSource *io_source;
    GSource *timeout_source;
    gsize max_message_bytes;
    GQueue output;
    gsize queued_bytes;
    gsize sent;
    guint8 prefix[4];
    gsize prefix_used;
    guint8 *body;
    gsize body_size;
    gsize body_used;
};

static void update_source(RelapseSocket *socket);

static void clear_source(GSource **source)
{
    if (*source) {
        g_source_destroy(*source);
        g_clear_pointer(source, g_source_unref);
    }
}

static void unref_socket(RelapseSocket *socket)
{
    if (--socket->references == 0) {
        g_main_context_unref(socket->context);
        g_free(socket);
    }
}

static void finish(RelapseSocket *socket, RelapseSocketCloseReason reason, const char *detail)
{
    if (socket->state == RELAPSE_SOCKET_CLOSED)
        return;

    ++socket->generation;
    socket->state = RELAPSE_SOCKET_CLOSED;
    clear_source(&socket->io_source);
    clear_source(&socket->timeout_source);
    g_socket_close(socket->connection, NULL);
    g_clear_object(&socket->connection);
    g_queue_clear_full(&socket->output, (GDestroyNotify)g_bytes_unref);
    socket->queued_bytes = socket->sent = 0;
    g_clear_pointer(&socket->body, g_free);
    socket->prefix_used = socket->body_size = socket->body_used = 0;
    socket->max_message_bytes = 0;

    if (socket->handlers.onClose)
        socket->handlers.onClose(socket, reason, detail, socket->user_data);
}

static void queue_frame(RelapseSocket *socket, const void *data, gsize size)
{
    guint8 *frame = g_malloc(size + 4);
    guint32 prefix = GUINT32_TO_BE((guint32)size);
    memcpy(frame, &prefix, 4);
    if (size)
        memcpy(frame + 4, data, size);
    g_queue_push_tail(&socket->output, g_bytes_new_take(frame, size + 4));
    socket->queued_bytes += size + 4;
}

static gboolean flush_output(RelapseSocket *socket)
{
    gsize budget = IoBytesPerTurn;
    guint frames = 0;
    while (budget && frames < FramesPerTurn && !g_queue_is_empty(&socket->output)) {
        GBytes *frame = g_queue_peek_head(&socket->output);
        gsize size;
        const char *data = g_bytes_get_data(frame, &size);
        g_autoptr(GError) error = NULL;
        gssize count = g_socket_send(socket->connection, data + socket->sent,
                                    MIN(size - socket->sent, budget), NULL, &error);
        if (count < 0 && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
            return TRUE;
        if (count <= 0) {
            finish(socket, RELAPSE_SOCKET_IO_ERROR, error ? error->message : "socket write failed");
            return FALSE;
        }
        socket->sent += count;
        budget -= count;
        if (socket->sent == size) {
            socket->queued_bytes -= size;
            socket->sent = 0;
            g_bytes_unref(g_queue_pop_head(&socket->output));
            ++frames;
        }
    }
    return TRUE;
}

static gboolean has_type(JsonObject *object, const char *name, GType type)
{
    JsonNode *node = json_object_get_member(object, name);
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == type;
}

static void open_channel(RelapseSocket *socket, GBytes *message)
{
    gsize size;
    const char *data = g_bytes_get_data(message, &size);
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!size || memchr(data, '\0', size) || !g_utf8_validate(data, size, NULL) ||
        !json_parser_load_from_data(parser, data, size, NULL)) {
        finish(socket, RELAPSE_SOCKET_PROTOCOL_ERROR, "expected a JSON channel response");
        return;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        finish(socket, RELAPSE_SOCKET_PROTOCOL_ERROR, "expected a channel response object");
        return;
    }
    JsonObject *object = json_node_get_object(root);
    if (!has_type(object, "ok", G_TYPE_BOOLEAN)) {
        finish(socket, RELAPSE_SOCKET_PROTOCOL_ERROR, "missing channel response status");
        return;
    }
    if (!json_object_get_boolean_member(object, "ok")) {
        const char *detail = has_type(object, "error", G_TYPE_STRING)
            ? json_object_get_string_member(object, "error") : "channel rejected";
        finish(socket, RELAPSE_SOCKET_PROTOCOL_ERROR, detail);
        return;
    }
    if (!has_type(object, "version", G_TYPE_INT64) ||
        json_object_get_int_member(object, "version") != ProtocolVersion ||
        !has_type(object, "max_message_bytes", G_TYPE_INT64)) {
        finish(socket, RELAPSE_SOCKET_PROTOCOL_ERROR, "invalid channel version or message limit");
        return;
    }
    gint64 limit = json_object_get_int_member(object, "max_message_bytes");
    if (limit <= 0 || limit > G_MAXUINT32) {
        finish(socket, RELAPSE_SOCKET_PROTOCOL_ERROR, "invalid channel message limit");
        return;
    }
    socket->max_message_bytes = MIN(limit, MaxMessageBytes);
    socket->state = RELAPSE_SOCKET_OPEN;
    clear_source(&socket->timeout_source);
    if (socket->handlers.onConnect)
        socket->handlers.onConnect(socket, socket->user_data);
}

static int receive_part(RelapseSocket *socket, void *data, gsize size,
                        gsize *used, gsize *budget)
{
    while (*used < size && *budget) {
        g_autoptr(GError) error = NULL;
        gssize count = g_socket_receive(socket->connection, (char *)data + *used,
                                       MIN(size - *used, *budget), NULL, &error);
        if (count < 0 && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
            return 0;
        if (count < 0) {
            finish(socket, RELAPSE_SOCKET_IO_ERROR, error->message);
            return -1;
        }
        if (count == 0) {
            gboolean partial = socket->prefix_used != 0;
            finish(socket, partial ? RELAPSE_SOCKET_PROTOCOL_ERROR : RELAPSE_SOCKET_PEER_CLOSED,
                   partial ? "connection closed mid-frame" : "peer closed the connection");
            return -1;
        }
        *used += count;
        *budget -= count;
    }
    return *used == size ? 1 : 0;
}

static void read_input(RelapseSocket *socket)
{
    gsize budget = IoBytesPerTurn;
    guint64 generation = socket->generation;
    for (guint frames = 0; budget && frames < FramesPerTurn; ++frames) {
        if (receive_part(socket, socket->prefix, 4, &socket->prefix_used, &budget) != 1)
            return;
        if (!socket->body) {
            guint32 prefix;
            memcpy(&prefix, socket->prefix, 4);
            socket->body_size = GUINT32_FROM_BE(prefix);
            gsize limit = socket->state == RELAPSE_SOCKET_OPENING
                ? MaxOpeningBytes : socket->max_message_bytes;
            if (socket->body_size > limit) {
                finish(socket, RELAPSE_SOCKET_PROTOCOL_ERROR, "frame exceeds the message limit");
                return;
            }
            socket->body = g_malloc(MAX(socket->body_size, 1));
        }
        if (receive_part(socket, socket->body, socket->body_size, &socket->body_used, &budget) != 1)
            return;

        g_autoptr(GBytes) message = g_bytes_new_take(g_steal_pointer(&socket->body), socket->body_size);
        socket->prefix_used = socket->body_size = socket->body_used = 0;
        if (socket->state == RELAPSE_SOCKET_OPENING)
            open_channel(socket, message);
        else if (socket->handlers.onMessage)
            socket->handlers.onMessage(socket, message, socket->user_data);

        if (socket->generation != generation)
            return;
    }
}

static gboolean dispatch(GSocket *connection, GIOCondition condition, gpointer data)
{
    (void)connection;
    RelapseSocket *socket = data;
    ++socket->references;
    guint64 generation = socket->generation;

    if (socket->state == RELAPSE_SOCKET_CONNECTING) {
        g_autoptr(GError) error = NULL;
        if (!g_socket_check_connect_result(socket->connection, &error))
            finish(socket, RELAPSE_SOCKET_IO_ERROR, error->message);
        else
            socket->state = RELAPSE_SOCKET_OPENING;
    }
    if (socket->generation == generation && (condition & G_IO_OUT))
        flush_output(socket);
    if (socket->generation == generation && (condition & (G_IO_IN | G_IO_HUP | G_IO_ERR)))
        read_input(socket);
    if (socket->generation == generation)
        update_source(socket);

    unref_socket(socket);
    return G_SOURCE_REMOVE;
}

static void update_source(RelapseSocket *socket)
{
    clear_source(&socket->io_source);
    if (socket->state == RELAPSE_SOCKET_CLOSED)
        return;
    GIOCondition condition = socket->state == RELAPSE_SOCKET_CONNECTING ? G_IO_OUT : G_IO_IN;
    if (!g_queue_is_empty(&socket->output))
        condition |= G_IO_OUT;
    socket->io_source = g_socket_create_source(socket->connection, condition, NULL);
    g_source_set_callback(socket->io_source, G_SOURCE_FUNC(dispatch), socket, NULL);
    g_source_attach(socket->io_source, socket->context);
}

static gboolean opening_timeout(gpointer data)
{
    RelapseSocket *socket = data;
    ++socket->references;
    finish(socket, RELAPSE_SOCKET_TIMEOUT, "channel handshake timed out");
    unref_socket(socket);
    return G_SOURCE_REMOVE;
}

RelapseSocket *relapse_socket_new(const RelapseSocketHandlers *handlers, gpointer user_data)
{
    RelapseSocket *socket = g_new0(RelapseSocket, 1);
    socket->references = 1;
    socket->context = g_main_context_ref_thread_default();
    if (handlers)
        socket->handlers = *handlers;
    socket->user_data = user_data;
    return socket;
}

gboolean relapse_socket_connect(RelapseSocket *socket, const char *path,
                                const char *channel, GError **error)
{
    g_return_val_if_fail(socket != NULL, FALSE);
    if (socket->state != RELAPSE_SOCKET_CLOSED) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "socket is already active");
        return FALSE;
    }
    if (!path)
        path = RELAPSE_SOCKET_DEFAULT_PATH;
    if (path[0] != '/' || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid Unix socket pathname");
        return FALSE;
    }
    gboolean valid = channel && *channel && strlen(channel) <= 255 && g_utf8_validate(channel, -1, NULL);
    if (valid) {
        for (const unsigned char *c = (const unsigned char *)channel; *c; ++c)
            if (*c < 0x20 || *c == 0x7f)
                valid = FALSE;
    }
    if (!valid) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid channel name");
        return FALSE;
    }

    g_autoptr(GSocket) connection = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                                                G_SOCKET_PROTOCOL_DEFAULT, error);
    if (!connection)
        return FALSE;
    g_socket_set_blocking(connection, FALSE);
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(path);
    g_autoptr(GError) failure = NULL;
    gboolean connected = g_socket_connect(connection, address, NULL, &failure);
    if (!connected && !g_error_matches(failure, G_IO_ERROR, G_IO_ERROR_PENDING)) {
        g_propagate_error(error, g_steal_pointer(&failure));
        return FALSE;
    }

    socket->connection = g_steal_pointer(&connection);
    socket->state = connected ? RELAPSE_SOCKET_OPENING : RELAPSE_SOCKET_CONNECTING;
    ++socket->generation;
    JsonObject *object = json_object_new();
    json_object_set_int_member(object, "version", ProtocolVersion);
    json_object_set_string_member(object, "channel", channel);
    g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, object);
    g_autofree char *opening = json_to_string(root, FALSE);
    queue_frame(socket, opening, strlen(opening));

    socket->timeout_source = g_timeout_source_new(OpeningTimeoutMs);
    g_source_set_callback(socket->timeout_source, opening_timeout, socket, NULL);
    g_source_attach(socket->timeout_source, socket->context);
    update_source(socket);
    return TRUE;
}

RelapseSocketState relapse_socket_get_state(const RelapseSocket *socket)
{
    g_return_val_if_fail(socket != NULL, RELAPSE_SOCKET_CLOSED);
    return socket->state;
}

GPid relapse_socket_get_peer_pid(const RelapseSocket *socket)
{
    if (!socket || !socket->connection) return 0;
    g_autoptr(GCredentials) credentials = g_socket_get_credentials(socket->connection, NULL);
    if (!credentials) return 0;
    GPid pid = g_credentials_get_unix_pid(credentials, NULL);
    return MAX(0, pid);
}

RelapseSocketSendResult relapse_socket_send(RelapseSocket *socket, GBytes *message)
{
    g_return_val_if_fail(socket != NULL && message != NULL, RELAPSE_SOCKET_SEND_CLOSED);
    if (socket->state != RELAPSE_SOCKET_OPEN)
        return RELAPSE_SOCKET_SEND_CLOSED;
    gsize size;
    const void *data = g_bytes_get_data(message, &size);
    if (size > socket->max_message_bytes)
        return RELAPSE_SOCKET_TOO_LARGE;
    if (socket->output.length >= MaxQueuedMessages || size + 4 > MaxQueuedBytes - socket->queued_bytes)
        return RELAPSE_SOCKET_BACKPRESSURE;
    queue_frame(socket, data, size);
    update_source(socket);
    return RELAPSE_SOCKET_QUEUED;
}

void relapse_socket_close(RelapseSocket *socket)
{
    g_return_if_fail(socket != NULL);
    ++socket->references;
    finish(socket, RELAPSE_SOCKET_LOCAL_CLOSED, "connection closed");
    unref_socket(socket);
}

void relapse_socket_free(RelapseSocket *socket)
{
    if (!socket)
        return;
    memset(&socket->handlers, 0, sizeof(socket->handlers));
    finish(socket, RELAPSE_SOCKET_LOCAL_CLOSED, "connection closed");
    unref_socket(socket);
}
