#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define RELAPSE_SOCKET_DEFAULT_PATH "/tmp/relapse.sock"

typedef struct RelapseSocket RelapseSocket;

typedef enum {
    RELAPSE_SOCKET_CLOSED,
    RELAPSE_SOCKET_CONNECTING,
    RELAPSE_SOCKET_OPENING,
    RELAPSE_SOCKET_OPEN
} RelapseSocketState;

typedef enum {
    RELAPSE_SOCKET_QUEUED,
    RELAPSE_SOCKET_SEND_CLOSED,
    RELAPSE_SOCKET_TOO_LARGE,
    RELAPSE_SOCKET_BACKPRESSURE
} RelapseSocketSendResult;

typedef enum {
    RELAPSE_SOCKET_PEER_CLOSED,
    RELAPSE_SOCKET_LOCAL_CLOSED,
    RELAPSE_SOCKET_PROTOCOL_ERROR,
    RELAPSE_SOCKET_IO_ERROR,
    RELAPSE_SOCKET_TIMEOUT
} RelapseSocketCloseReason;

typedef struct {
    void (*onConnect)(RelapseSocket *socket, gpointer user_data);
    void (*onMessage)(RelapseSocket *socket, GBytes *message, gpointer user_data);
    void (*onClose)(RelapseSocket *socket, RelapseSocketCloseReason reason,
                    const char *detail, gpointer user_data);
} RelapseSocketHandlers;

RelapseSocket *relapse_socket_new(const RelapseSocketHandlers *handlers, gpointer user_data);

gboolean relapse_socket_connect(RelapseSocket *socket, const char *path,
                                const char *channel, GError **error);
RelapseSocketState relapse_socket_get_state(const RelapseSocket *socket);
GPid relapse_socket_get_peer_pid(const RelapseSocket *socket);

RelapseSocketSendResult relapse_socket_send(RelapseSocket *socket, GBytes *message);

void relapse_socket_close(RelapseSocket *socket);
void relapse_socket_free(RelapseSocket *socket);

G_END_DECLS
