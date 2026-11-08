#pragma once

#include "socket/client.h"

typedef struct ConsoleBridge ConsoleBridge;
typedef struct {
    void (*show)(gboolean visible, gpointer user_data);
    void (*title)(const char *title, gpointer user_data);
    void (*clear)(gpointer user_data);
    void (*entry)(const char *level, const char *text, gpointer user_data);
    void (*input)(gint64 id, gpointer user_data);
    void (*error)(const char *detail, gpointer user_data);
} ConsoleBridgeHandlers;

ConsoleBridge *console_bridge_new(SocketClient *client, const ConsoleBridgeHandlers *handlers, gpointer user_data);
void console_bridge_sync(ConsoleBridge *bridge);
gboolean console_bridge_result(ConsoleBridge *bridge, gint64 id, gboolean submitted, const char *detail);
void console_bridge_input(ConsoleBridge *bridge, gint64 id, const char *text);
void console_bridge_free(ConsoleBridge *bridge);
