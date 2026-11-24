#pragma once

#include "socket/client.h"

typedef struct DialogBridge DialogBridge;
typedef struct {
    void (*open)(gint64 id, const char *title, const char *text, const char *const *buttons,
                 const char *icon, guint default_button, gpointer user_data);
    void (*close)(gint64 id, gpointer user_data);
    void (*reset)(gpointer user_data);
    void (*changed)(gpointer user_data);
    void (*error)(const char *detail, gpointer user_data);
} DialogBridgeHandlers;

DialogBridge *dialog_bridge_new(SocketClient *client, const DialogBridgeHandlers *handlers, gpointer user_data);
void dialog_bridge_sync(DialogBridge *bridge);
gboolean dialog_bridge_is_ready(DialogBridge *bridge);
gboolean dialog_bridge_result(DialogBridge *bridge, gint64 id, gboolean submitted, const char *detail);
void dialog_bridge_choice(DialogBridge *bridge, gint64 id, guint choice);
void dialog_bridge_free(DialogBridge *bridge);
