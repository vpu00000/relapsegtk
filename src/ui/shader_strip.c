#include "shader_strip.h"

#define SHADER_STRIP_TYPE (shader_strip_get_type())
G_DECLARE_FINAL_TYPE(ShaderStrip, shader_strip, RELAPSE, SHADER_STRIP, GtkWidget)

struct _ShaderStrip {
    GtkWidget parent_instance;
    guint timer;
    float phase;
};

G_DEFINE_FINAL_TYPE(ShaderStrip, shader_strip, GTK_TYPE_WIDGET)

static gboolean advance(gpointer data)
{
    ShaderStrip *strip = data;
    /* Reduce the integer clock before converting to a render coordinate. */
    const gint64 period_us = 10472000;
    strip->phase = (float)(g_get_monotonic_time() % period_us) / period_us;
    gtk_widget_queue_draw(GTK_WIDGET(strip));
    return G_SOURCE_CONTINUE;
}

static void sync_animation(ShaderStrip *strip)
{
    GtkWidget *widget = GTK_WIDGET(strip);
    GtkRoot *root = gtk_widget_get_root(widget);
    gboolean animations = FALSE;
    g_object_get(gtk_widget_get_settings(widget), "gtk-enable-animations", &animations, NULL);
    if (animations && GTK_IS_WINDOW(root) && gtk_window_is_active(GTK_WINDOW(root))) {
        if (!strip->timer) {
            advance(strip);
            strip->timer = g_timeout_add(33, advance, strip);
        }
    } else {
        g_clear_handle_id(&strip->timer, g_source_remove);
    }
}

static void mapped(GtkWidget *widget)
{
    GTK_WIDGET_CLASS(shader_strip_parent_class)->map(widget);
    GtkRoot *root = gtk_widget_get_root(widget);
    if (GTK_IS_WINDOW(root))
        g_signal_connect_swapped(root, "notify::is-active", G_CALLBACK(sync_animation), widget);
    g_signal_connect_swapped(gtk_widget_get_settings(widget), "notify::gtk-enable-animations",
                             G_CALLBACK(sync_animation), widget);
    sync_animation(RELAPSE_SHADER_STRIP(widget));
}

static void unmapped(GtkWidget *widget)
{
    ShaderStrip *strip = RELAPSE_SHADER_STRIP(widget);
    g_clear_handle_id(&strip->timer, g_source_remove);
    g_signal_handlers_disconnect_by_data(gtk_widget_get_root(widget), strip);
    g_signal_handlers_disconnect_by_data(gtk_widget_get_settings(widget), strip);
    GTK_WIDGET_CLASS(shader_strip_parent_class)->unmap(widget);
}

static void snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    static const GskColorStop stops[] = {
        {0.0f, {0.34f, 0.34f, 0.34f, 1}},
        {0.3f, {0.44f, 0.44f, 0.44f, 1}},
        {0.5f, {0.72f, 0.72f, 0.72f, 1}},
        {0.7f, {0.44f, 0.44f, 0.44f, 1}},
        {1.0f, {0.34f, 0.34f, 0.34f, 1}}
    };
    float width = gtk_widget_get_width(widget);
    if (width <= 0 || gtk_widget_get_height(widget) <= 0) return;
    float wave = 1.4f * width;
    float start = (RELAPSE_SHADER_STRIP(widget)->phase - 1.0f) * wave;
    gtk_snapshot_append_repeating_linear_gradient(snapshot,
        &GRAPHENE_RECT_INIT(0, 0, width, gtk_widget_get_height(widget)),
        &GRAPHENE_POINT_INIT(start, 0), &GRAPHENE_POINT_INIT(start + wave, 0),
        stops, G_N_ELEMENTS(stops));
}

static void shader_strip_init(ShaderStrip *strip)
{
    gtk_widget_set_size_request(GTK_WIDGET(strip), -1, 2);
    gtk_widget_set_can_target(GTK_WIDGET(strip), FALSE);
}

static void shader_strip_class_init(ShaderStripClass *klass)
{
    GtkWidgetClass *widget = GTK_WIDGET_CLASS(klass);
    widget->snapshot = snapshot;
    widget->map = mapped;
    widget->unmap = unmapped;
    gtk_widget_class_set_accessible_role(widget, GTK_ACCESSIBLE_ROLE_PRESENTATION);
}

GtkWidget *shader_strip_new(void)
{
    return g_object_new(SHADER_STRIP_TYPE, NULL);
}
