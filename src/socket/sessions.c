#include "sessions.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

struct Sessions {
    GHashTable *clients;
    SessionHandlers handlers;
    gpointer user_data;
    gboolean output_enabled;
};

typedef struct {
    Sessions *owner;
    char *path;
    SocketClient *client;
    UiBridge *bridge;
    ConsoleBridge *console;
    SocketClientState state;
    OutputState output_state;
    GPid pid;
    int pid_fd;
    gboolean available;
} Session;

static gboolean process_running(Session *session)
{
    if (session->pid_fd >= 0) {
        struct pollfd fd = {.fd = session->pid_fd, .events = POLLIN};
        return poll(&fd, 1, 0) <= 0;
    }
    /* Older kernels can still distinguish a missing socket from an exited process. */
    return session->pid > 0 && (kill(session->pid, 0) == 0 || errno == EPERM);
}

static void remember_process(Session *session)
{
    GPid pid = socket_client_get_peer_pid(session->client);
    if (!pid || (pid == session->pid && process_running(session))) return;
    if (session->pid_fd >= 0) close(session->pid_fd);
    session->pid = pid;
    session->pid_fd = syscall(SYS_pidfd_open, pid, 0);
}

static void output_changed(OutputState state, const char *detail, gpointer user_data)
{
    Session *session = user_data;
    Sessions *owner = session->owner;
    session->output_state = state;
    if (owner->handlers.output_changed)
        owner->handlers.output_changed(session->path, state, detail, owner->user_data);
}

static void entry(const char *level, const char *text, gpointer user_data)
{
    Session *session = user_data;
    Sessions *owner = session->owner;
    if (owner->handlers.entry)
        owner->handlers.entry(session->path, level, text, owner->user_data);
}

static void console_show(gboolean visible, gpointer user_data)
{
    Session *session = user_data;
    if (session->owner->handlers.console_show)
        session->owner->handlers.console_show(session->path, visible, session->owner->user_data);
}

static void console_title(const char *title, gpointer user_data)
{
    Session *session = user_data;
    if (session->owner->handlers.console_title)
        session->owner->handlers.console_title(session->path, title, session->owner->user_data);
}

static void console_clear(gpointer user_data)
{
    Session *session = user_data;
    if (session->owner->handlers.console_clear)
        session->owner->handlers.console_clear(session->path, session->owner->user_data);
}

static void console_entry(const char *level, const char *text, gpointer user_data)
{
    Session *session = user_data;
    if (session->owner->handlers.console_entry)
        session->owner->handlers.console_entry(session->path, level, text, session->owner->user_data);
}

static void console_input(gint64 id, gpointer user_data)
{
    Session *session = user_data;
    if (session->owner->handlers.console_input)
        session->owner->handlers.console_input(session->path, id, session->owner->user_data);
}

static void state_changed(SocketClientState state, const char *detail, gpointer user_data)
{
    Session *session = user_data;
    Sessions *owner = session->owner;
    session->state = state;
    remember_process(session);
    ui_bridge_sync(session->bridge);
    console_bridge_sync(session->console);
    if (owner->handlers.state_changed)
        owner->handlers.state_changed(session->path, state, detail, owner->user_data);
}

static void result(gint64 id, gboolean submitted, const char *detail, gpointer user_data)
{
    Session *session = user_data;
    Sessions *owner = session->owner;
    if (ui_bridge_result(session->bridge, id, submitted, detail)) return;
    if (console_bridge_result(session->console, id, submitted, detail)) return;
    if (owner->handlers.result)
        owner->handlers.result(session->path, id, submitted, detail, owner->user_data);
}

static void error(const char *detail, gpointer user_data)
{
    entry("error", detail, user_data);
}

static void free_session(gpointer user_data)
{
    Session *session = user_data;
    console_bridge_free(session->console);
    ui_bridge_free(session->bridge);
    socket_client_free(session->client);
    if (session->pid_fd >= 0) close(session->pid_fd);
    g_free(session->path);
    g_free(session);
}

Sessions *sessions_new(const SessionHandlers *handlers, gpointer user_data)
{
    Sessions *sessions = g_new0(Sessions, 1);
    sessions->clients = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, free_session);
    if (handlers) sessions->handlers = *handlers;
    sessions->user_data = user_data;
    return sessions;
}

void sessions_update(Sessions *sessions, const char *const *paths)
{
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, sessions->clients);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Session *session = value;
        session->available = g_strv_contains(paths, session->path);
        if (session->available || process_running(session)) continue;
        session->state = SOCKET_CLIENT_DISCONNECTED;
        if (sessions->handlers.state_changed)
            sessions->handlers.state_changed(session->path, SOCKET_CLIENT_DISCONNECTED, "Disconnected", sessions->user_data);
        g_hash_table_iter_remove(&iter);
    }
    for (guint i = 0; paths[i]; ++i) {
        if (g_hash_table_contains(sessions->clients, paths[i])) continue;
        Session *session = g_new0(Session, 1);
        session->owner = sessions;
        session->pid_fd = -1;
        session->available = TRUE;
        session->path = g_strdup(paths[i]);
        session->state = SOCKET_CLIENT_DISCONNECTED;
        SocketClientHandlers client_handlers = {
            .state_changed = state_changed, .result = result, .log = entry, .error = error
        };
        session->client = socket_client_new(session->path, &client_handlers, session);
        UiBridgeHandlers output_handlers = {.state_changed = output_changed, .entry = entry};
        session->bridge = ui_bridge_new(session->client, &output_handlers, session);
        ConsoleBridgeHandlers console_handlers = {
            .show = console_show, .title = console_title, .clear = console_clear,
            .entry = console_entry, .input = console_input, .error = error
        };
        session->console = console_bridge_new(session->client, &console_handlers, session);
        g_hash_table_insert(sessions->clients, session->path, session);
        ui_bridge_set_output_enabled(session->bridge, sessions->output_enabled);
        socket_client_start(session->client);
    }
}

void sessions_set_output_enabled(Sessions *sessions, gboolean enabled)
{
    sessions->output_enabled = enabled;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, sessions->clients);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        ui_bridge_set_output_enabled(((Session *)value)->bridge, enabled);
}

static gint compare_paths(gconstpointer left, gconstpointer right)
{
    return g_strcmp0(*(const char *const *)left, *(const char *const *)right);
}

char **sessions_list_active(Sessions *sessions)
{
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, sessions->clients);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Session *session = value;
        if (session->available || process_running(session)) g_ptr_array_add(paths, g_strdup(session->path));
    }
    g_ptr_array_sort(paths, compare_paths);
    g_ptr_array_add(paths, NULL);
    return (char **)g_ptr_array_free(paths, FALSE);
}

static Session *lookup(Sessions *sessions, const char *path)
{
    return sessions && path ? g_hash_table_lookup(sessions->clients, path) : NULL;
}

gboolean sessions_is_active(Sessions *sessions, const char *path)
{
    Session *session = lookup(sessions, path);
    return session && (session->available || process_running(session));
}

SocketClient *sessions_get_client(Sessions *sessions, const char *path)
{
    Session *session = lookup(sessions, path);
    return session ? session->client : NULL;
}

SocketClientState sessions_get_state(Sessions *sessions, const char *path)
{
    Session *session = lookup(sessions, path);
    return session ? session->state : SOCKET_CLIENT_DISCONNECTED;
}

OutputState sessions_get_output_state(Sessions *sessions, const char *path)
{
    Session *session = lookup(sessions, path);
    return session ? session->output_state : OUTPUT_OFF;
}

gboolean sessions_can_execute(Sessions *sessions, const char *path)
{
    Session *session = lookup(sessions, path);
    return session && ui_bridge_is_ready(session->bridge);
}

SocketClientSendResult sessions_execute(Sessions *sessions, const char *path, const char *name, const char *source)
{
    Session *session = lookup(sessions, path);
    return session ? ui_bridge_execute(session->bridge, name, source) : SOCKET_CLIENT_NOT_READY;
}

void sessions_console_input(Sessions *sessions, const char *path, gint64 id, const char *text)
{
    Session *session = lookup(sessions, path);
    if (session) console_bridge_input(session->console, id, text);
}

void sessions_free(Sessions *sessions)
{
    if (!sessions) return;
    g_hash_table_unref(sessions->clients);
    g_free(sessions);
}
