#include "tab_strip.h"
#include <math.h>

typedef struct {
    GtkBox *content;
    GWeakRef followed;
    guint tick;
    guint frames;
} Strip;

static void free_strip(gpointer data)
{
    Strip *strip = data;
    g_weak_ref_clear(&strip->followed);
    g_free(strip);
}

static gboolean scroll_tabs(GtkEventControllerScroll *controller, double dx, double dy, gpointer data)
{
    GtkWidget *widget = data;
    Strip *strip = g_object_get_data(G_OBJECT(widget), "tab-strip");
    if (strip->tick) gtk_widget_remove_tick_callback(widget, strip->tick);
    strip->tick = 0;
    g_weak_ref_set(&strip->followed, NULL);
    GtkAdjustment *adjustment = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(widget));
    double delta = fabs(dx) > fabs(dy) ? dx : dy;
    if (gtk_event_controller_scroll_get_unit(controller) == GDK_SCROLL_UNIT_WHEEL) delta *= 48;
    double maximum = MAX(0, gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment));
    gtk_adjustment_set_value(adjustment, CLAMP(gtk_adjustment_get_value(adjustment) + delta, 0, maximum));
    return TRUE;
}

static gboolean reveal_tab(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    (void)clock;
    Strip *strip = data;
    /* Give newly added tabs a layout before measuring them. */
    if (strip->frames++ == 0) return G_SOURCE_CONTINUE;
    g_autoptr(GtkWidget) tab = g_weak_ref_get(&strip->followed);
    graphene_rect_t bounds;
    if (tab && gtk_widget_compute_bounds(tab, GTK_WIDGET(strip->content), &bounds)) {
        GtkAdjustment *adjustment = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(widget));
        gtk_adjustment_clamp_page(adjustment, bounds.origin.x, bounds.origin.x + bounds.size.width);
    }
    strip->tick = 0;
    return G_SOURCE_REMOVE;
}

void tab_strip_reveal(GtkWidget *widget, GtkWidget *tab)
{
    Strip *strip = g_object_get_data(G_OBJECT(widget), "tab-strip");
    g_weak_ref_set(&strip->followed, tab);
    strip->frames = 0;
    if (!strip->tick) strip->tick = gtk_widget_add_tick_callback(widget, reveal_tab, strip, NULL);
}

static void viewport_resized(GtkAdjustment *adjustment, GParamSpec *property, GtkWidget *widget)
{
    (void)adjustment;
    (void)property;
    Strip *strip = g_object_get_data(G_OBJECT(widget), "tab-strip");
    g_autoptr(GtkWidget) tab = g_weak_ref_get(&strip->followed);
    if (tab && !strip->tick) tab_strip_reveal(widget, tab);
}

GtkBox *tab_strip_content(GtkWidget *widget)
{
    Strip *strip = g_object_get_data(G_OBJECT(widget), "tab-strip");
    return strip->content;
}

GtkWidget *tab_strip_new(void)
{
    GtkWidget *widget = gtk_scrolled_window_new();
    Strip *strip = g_new0(Strip, 1);
    g_weak_ref_init(&strip->followed, NULL);
    g_object_set_data_full(G_OBJECT(widget), "tab-strip", strip, free_strip);
    gtk_widget_add_css_class(widget, "tab-strip");
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    strip->content = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(widget), GTK_WIDGET(strip->content));
    GtkWidget *viewport = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(widget));
    gtk_scrollable_set_hscroll_policy(GTK_SCROLLABLE(viewport), GTK_SCROLL_NATURAL);
    GtkAdjustment *adjustment = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(widget));
    g_signal_connect_object(adjustment, "notify::page-size", G_CALLBACK(viewport_resized), widget, 0);
    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
    g_signal_connect(scroll, "scroll", G_CALLBACK(scroll_tabs), widget);
    gtk_widget_add_controller(widget, scroll);
    return widget;
}
