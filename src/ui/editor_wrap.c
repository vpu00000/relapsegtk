#include "editor_wrap.h"

#include <string.h>

/* GtkTextView shapes one paragraph at a time. Give it bounded paragraphs,
 * keeping every display-only newline out of the document and its undo history. */
enum { ParagraphBytes = 1024 };

typedef struct {
    GtkTextView *view;
    GtkTextBuffer *document;
    GtkTextBuffer *display;
    GtkTextTag *soft_break;
    GArray *breaks;  /* Sorted source character offsets of display-only newlines. */
    GtkAdjustment *scroll;
    GtkEventController *thumb_events;
    guint commit, idle, jump_idle;
    double last_scroll, last_range, jump_fraction;
    int jump_cursor;
    gboolean syncing, dragging_thumb;
} WrappedEditor;

static WrappedEditor *wrapped_editor(GtkTextView *view)
{
    return g_object_get_data(G_OBJECT(view), "wrapped-editor");
}

GtkTextBuffer *editor_document(GtkTextView *view)
{
    WrappedEditor *w = wrapped_editor(view);
    return w ? w->document : gtk_text_view_get_buffer(view);
}

void editor_wrap_set_preference(GtkTextView *view, GtkWrapMode mode)
{
    g_object_set_data(G_OBJECT(view), "preferred-wrap", GINT_TO_POINTER(mode));
    WrappedEditor *w = wrapped_editor(view);
    gtk_text_view_set_wrap_mode(view, w && w->display ? GTK_WRAP_CHAR : mode);
}

static int offset_of(GtkTextBuffer *buffer, GtkTextMark *mark)
{
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_mark(buffer, &at, mark);
    return gtk_text_iter_get_offset(&at);
}

/* Display offset of break i is its source offset plus i earlier newlines. */
static guint breaks_before(WrappedEditor *w, int offset, gboolean display)
{
    guint low = 0, high = w->breaks->len;
    while (low < high) {
        guint mid = low + (high - low) / 2;
        int pos = g_array_index(w->breaks, int, mid);
        if (pos + (display ? (int)mid : 0) < offset) low = mid + 1;
        else high = mid;
    }
    return low;
}

static int source_offset(WrappedEditor *w, int offset)
{
    return offset - (int)breaks_before(w, offset, TRUE);
}

static int display_offset(WrappedEditor *w, int offset)
{
    return offset + (int)breaks_before(w, offset + 1, FALSE);
}

static void sync_selection(WrappedEditor *w, gboolean to_display)
{
    if (w->syncing || !w->display) return;
    GtkTextBuffer *from = to_display ? w->document : w->display;
    GtkTextBuffer *to = to_display ? w->display : w->document;
    int a = offset_of(from, gtk_text_buffer_get_insert(from));
    int b = offset_of(from, gtk_text_buffer_get_selection_bound(from));
    int current_a = offset_of(to, gtk_text_buffer_get_insert(to));
    int current_b = offset_of(to, gtk_text_buffer_get_selection_bound(to));
    /* Either side of a visual break represents the same source position. */
    if (to_display && source_offset(w, current_a) == a && source_offset(w, current_b) == b)
        return;
    a = to_display ? display_offset(w, a) : source_offset(w, a);
    b = to_display ? display_offset(w, b) : source_offset(w, b);
    if (current_a == a && current_b == b) return;
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_offset(to, &start, a);
    gtk_text_buffer_get_iter_at_offset(to, &end, b);
    w->syncing = TRUE;
    gtk_text_buffer_select_range(to, &start, &end);
    w->syncing = FALSE;
}

static gboolean finish_edit(gpointer data)
{
    WrappedEditor *w = data;
    w->idle = 0;
    sync_selection(w, TRUE);
    gtk_widget_action_set_enabled(GTK_WIDGET(w->view), "text.undo", gtk_text_buffer_get_can_undo(w->document));
    gtk_widget_action_set_enabled(GTK_WIDGET(w->view), "text.redo", gtk_text_buffer_get_can_redo(w->document));
    return G_SOURCE_REMOVE;
}

static void mark_set(GtkTextBuffer *buffer, GtkTextIter *at, GtkTextMark *mark, WrappedEditor *w)
{
    (void)at;
    if (mark == gtk_text_buffer_get_insert(buffer) || mark == gtk_text_buffer_get_selection_bound(buffer)) {
        if (!w->syncing && buffer == w->display) g_clear_handle_id(&w->jump_idle, g_source_remove);
        sync_selection(w, buffer == w->document);
    }
}

/* GtkTextView initially estimates unmeasured paragraphs at zero height.
 * Remember an early scrollbar or caret jump until GTK has its final range. */
static gboolean layout_pending(WrappedEditor *w)
{
    GtkTextIter end;
    int height;
    gtk_text_buffer_get_end_iter(w->display, &end);
    gtk_text_view_get_line_yrange(w->view, &end, NULL, &height);
    return height == 0;
}

static gboolean finish_jump(gpointer data)
{
    WrappedEditor *w = data;
    w->jump_idle = 0;
    if (w->jump_cursor >= 0) {
        GtkTextMark *cursor = gtk_text_buffer_get_insert(w->display);
        if (offset_of(w->display, cursor) == w->jump_cursor)
            gtk_text_view_scroll_mark_onscreen(w->view, cursor);
    } else {
        double range = gtk_adjustment_get_upper(w->scroll) - gtk_adjustment_get_page_size(w->scroll);
        gtk_adjustment_set_value(w->scroll, w->jump_fraction * range);
    }
    return G_SOURCE_REMOVE;
}

static void defer_caret_jump(WrappedEditor *w)
{
    if (!w->display || !gtk_widget_get_mapped(GTK_WIDGET(w->view)) || !layout_pending(w)) return;
    w->jump_cursor = offset_of(w->display, gtk_text_buffer_get_insert(w->display));
    if (!w->jump_idle) w->jump_idle = g_idle_add_full(G_PRIORITY_LOW, finish_jump, w, NULL);
}

static void moved_cursor(GtkTextView *view, GtkMovementStep step, int count, gboolean extend, WrappedEditor *w)
{
    (void)view; (void)count; (void)extend;
    if (step == GTK_MOVEMENT_BUFFER_ENDS) defer_caret_jump(w);
}

static void range_changed(GtkAdjustment *scroll, WrappedEditor *w)
{
    /* A jump may validate more paragraphs inside value-changed itself.
     * Translate the thumb against the range the user actually dragged. */
    if (gtk_adjustment_get_value(scroll) == w->last_scroll)
        w->last_range = gtk_adjustment_get_upper(scroll) - gtk_adjustment_get_page_size(scroll);
}

static gboolean thumb_event(GtkEventControllerLegacy *controller, GdkEvent *event, WrappedEditor *w)
{
    (void)controller;
    switch (gdk_event_get_event_type(event)) {
    case GDK_BUTTON_PRESS: case GDK_TOUCH_BEGIN:
        w->dragging_thumb = TRUE;
        g_clear_handle_id(&w->jump_idle, g_source_remove);
        break;
    case GDK_BUTTON_RELEASE: case GDK_TOUCH_END: case GDK_TOUCH_CANCEL: case GDK_GRAB_BROKEN:
        w->dragging_thumb = FALSE;
        break;
    default:
        break;
    }
    return GDK_EVENT_PROPAGATE;
}

static void scrolled(GtkAdjustment *scroll, WrappedEditor *w)
{
    double value = gtk_adjustment_get_value(scroll);
    double distance = ABS(value - w->last_scroll);
    double range = w->last_range;
    w->last_scroll = value;
    range_changed(scroll, w);
    if (w->syncing || (w->jump_idle && w->jump_cursor >= 0)) return;
    double page = gtk_adjustment_get_page_size(scroll);
    if (distance < page && !w->dragging_thumb) {
        g_clear_handle_id(&w->jump_idle, g_source_remove);
        return;
    }
    if (range <= 0 || !layout_pending(w)) return;
    w->jump_cursor = -1;
    w->jump_fraction = CLAMP(value / range, 0, 1);
    if (!w->jump_idle) w->jump_idle = g_idle_add_full(G_PRIORITY_LOW, finish_jump, w, NULL);
}

void editor_reveal_cursor(GtkTextView *view)
{
    WrappedEditor *w = wrapped_editor(view);
    if (w) {
        sync_selection(w, TRUE);
        defer_caret_jump(w);
    }
    gtk_text_view_scroll_mark_onscreen(view, gtk_text_buffer_get_insert(gtk_text_view_get_buffer(view)));
}

/* Preserve real paragraph delimiters, including CRLF and U+2029. Use byte
 * bounds for Pango, but character offsets for GTK selections and edits. */
static GString *wrap_text(const char *text, int len, int offset, gboolean ascii, GArray *new_breaks)
{
    GString *wrapped = g_string_sized_new(len);
    while (len > 0) {
        int end, next;
        pango_find_paragraph_boundary(text, len, &end, &next);
        int consumed = 0;
        while (end - consumed > ParagraphBytes * 2) {
            int n = ParagraphBytes;
            while ((text[consumed + n] & 0xc0) == 0x80) --n;
            g_string_append_len(wrapped, text + consumed, n);
            offset += ascii ? n : g_utf8_strlen(text + consumed, n);
            g_array_append_val(new_breaks, offset);
            g_string_append_c(wrapped, '\n');
            consumed += n;
        }
        g_string_append_len(wrapped, text + consumed, next - consumed);
        offset += ascii ? next - consumed : g_utf8_strlen(text + consumed, next - consumed);
        text += next;
        len -= next;
    }
    return wrapped;
}

/* Native text drags export visible characters. An invisible delimiter still
 * bounds the GTK paragraph, but contributes no character to the transfer. */
static void tag_breaks(WrappedEditor *w, guint first, guint count)
{
    for (guint i = first; i < first + count; ++i) {
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(w->display, &a, g_array_index(w->breaks, int, i) + (int)i);
        b = a;
        gtk_text_iter_forward_char(&b);
        gtk_text_buffer_apply_tag(w->display, w->soft_break, &a, &b);
    }
}

/* Reflow only the display paragraphs touched by an edit. Most keystrokes
 * fit their existing paragraph and can follow the native small-edit path. */
static void mirror_edit(WrappedEditor *w, int first, int last, const char *text, int bytes, int chars)
{
    GtkTextIter a, b;
    g_clear_handle_id(&w->jump_idle, g_source_remove);
    int start = display_offset(w, first), end = display_offset(w, last);
    gtk_text_buffer_get_iter_at_offset(w->display, &a, start);
    gtk_text_buffer_get_iter_at_offset(w->display, &b, end);
    int delta = chars - (last - first);
    guint lo = breaks_before(w, start, TRUE), hi = breaks_before(w, end, TRUE);
    g_autoptr(GArray) added = NULL;
    g_autoptr(GString) wrapped = NULL;
    /* A break at either edge may have become redundant after a deletion. */
    gboolean at_break = first < last && (gtk_text_iter_starts_line(&a) || gtk_text_iter_ends_line(&b));
    if (lo != hi || at_break || gtk_text_iter_get_bytes_in_line(&a) + (gint64)bytes > 2 * ParagraphBytes ||
        gtk_text_iter_get_line(&a) != gtk_text_iter_get_line(&b)) {
        GtkTextIter head = a, tail = b;
        gtk_text_iter_set_line_offset(&head, 0);
        if (!gtk_text_iter_ends_line(&tail)) gtk_text_iter_forward_to_line_end(&tail);
        int head_pos = gtk_text_iter_get_offset(&head), tail_pos = gtk_text_iter_get_offset(&tail);
        lo = breaks_before(w, head_pos, TRUE);
        hi = breaks_before(w, tail_pos, TRUE);
        g_autofree char *prefix = gtk_text_buffer_get_text(w->display, &head, &a, TRUE);
        g_autofree char *suffix = gtk_text_buffer_get_text(w->display, &b, &tail, TRUE);
        gboolean ascii = bytes == chars;
        g_autoptr(GString) raw = NULL;
        if (*prefix || *suffix) {
            raw = g_string_new(prefix);
            g_string_append_len(raw, text, bytes);
            g_string_append(raw, suffix);
            text = raw->str;
            bytes = raw->len;
            ascii = ascii && g_str_is_ascii(prefix) && g_str_is_ascii(suffix);
        }
        /* Absorb neighbouring synthetic delimiters if the edit empties a
         * paragraph; real blank lines always belong to the source. */
        if (bytes == 0 && hi < w->breaks->len &&
            g_array_index(w->breaks, int, hi) + (int)hi == tail_pos) {
            gtk_text_iter_forward_char(&tail);
            ++hi;
        } else if (bytes == 0 && lo > 0 &&
                   g_array_index(w->breaks, int, lo - 1) + (int)lo == head_pos) {
            gtk_text_iter_backward_char(&head);
            --lo;
        }
        added = g_array_new(FALSE, FALSE, sizeof(int));
        wrapped = wrap_text(text, bytes, source_offset(w, gtk_text_iter_get_offset(&head)), ascii, added);
        a = head;
        b = tail;
        text = wrapped->str;
        bytes = wrapped->len;
    }
    if (hi > lo) g_array_remove_range(w->breaks, lo, hi - lo);
    if (delta)
        for (guint i = lo; i < w->breaks->len; ++i) g_array_index(w->breaks, int, i) += delta;
    if (added && added->len) g_array_insert_vals(w->breaks, lo, added->data, added->len);

    w->syncing = TRUE;
    if (!gtk_text_iter_equal(&a, &b)) gtk_text_buffer_delete(w->display, &a, &b);
    if (bytes) {
        int offset = gtk_text_iter_get_offset(&a);
        gtk_text_buffer_insert(w->display, &a, text, bytes);
        /* GTK can inherit the invisible tag when replacing the text between
         * two breaks. Only this edit's synthetic newlines should keep it. */
        gtk_text_buffer_get_iter_at_offset(w->display, &b, offset);
        gtk_text_buffer_remove_tag(w->display, w->soft_break, &b, &a);
    }
    if (added) tag_breaks(w, lo, added->len);
    w->syncing = FALSE;
}

static void document_commit(GtkTextBuffer *buffer, GtkTextBufferNotifyFlags flags, guint pos,
                            guint length, gpointer data)
{
    WrappedEditor *w = data;
    if (flags == GTK_TEXT_BUFFER_NOTIFY_AFTER_INSERT) {
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buffer, &a, pos);
        gtk_text_buffer_get_iter_at_offset(buffer, &b, pos + length);
        g_autofree char *text = gtk_text_buffer_get_text(buffer, &a, &b, TRUE);
        mirror_edit(w, pos, pos, text, strlen(text), length);
    } else {
        mirror_edit(w, pos, pos + length, "", 0, 0);
    }
    if (!w->idle) w->idle = g_idle_add_full(G_PRIORITY_HIGH_IDLE, finish_edit, w, NULL);
}

static void display_insert(GtkTextBuffer *buffer, GtkTextIter *at, const char *text, int len, WrappedEditor *w)
{
    if (w->syncing) return;
    g_signal_stop_emission_by_name(buffer, "insert-text");
    GtkTextIter dest;
    gtk_text_buffer_get_iter_at_offset(w->document, &dest, source_offset(w, gtk_text_iter_get_offset(at)));
    gtk_text_buffer_insert(w->document, &dest, text, len);
    gtk_text_buffer_get_iter_at_offset(buffer, at, display_offset(w, gtk_text_iter_get_offset(&dest)));
    sync_selection(w, TRUE);
}

static void display_delete(GtkTextBuffer *buffer, GtkTextIter *start, GtkTextIter *end, WrappedEditor *w)
{
    if (w->syncing) return;
    g_signal_stop_emission_by_name(buffer, "delete-range");
    int first = source_offset(w, gtk_text_iter_get_offset(start));
    int last = source_offset(w, gtk_text_iter_get_offset(end));
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(w->document, &a, first);
    gtk_text_buffer_get_iter_at_offset(w->document, &b, last);
    if (first == last && !gtk_text_buffer_get_has_selection(buffer) && !gtk_text_iter_equal(start, end)) {
        if (gtk_text_iter_get_offset(start) == offset_of(buffer, gtk_text_buffer_get_insert(buffer)))
            gtk_text_iter_forward_cursor_position(&b);
        else
            gtk_text_iter_backward_cursor_position(&a);
    }
    gtk_text_buffer_delete(w->document, &a, &b);
    gtk_text_buffer_get_iter_at_offset(buffer, start, display_offset(w, gtk_text_iter_get_offset(&a)));
    *end = *start;
    sync_selection(w, TRUE);
}

static void copy_selection(GtkTextView *view, WrappedEditor *w)
{
    if (!w->display) return;
    g_signal_stop_emission_by_name(view, "copy-clipboard");
    gtk_text_buffer_copy_clipboard(w->document, gtk_widget_get_clipboard(GTK_WIDGET(view)));
}

static void cut_selection(GtkTextView *view, WrappedEditor *w)
{
    if (!w->display) return;
    g_signal_stop_emission_by_name(view, "cut-clipboard");
    gtk_text_buffer_cut_clipboard(w->document, gtk_widget_get_clipboard(GTK_WIDGET(view)), TRUE);
}

/* GtkTextView registers its visible buffer with the primary clipboard when
 * realized. Let middle-click paste read from the unwrapped selection too. */
static void primary_to_document(GtkWidget *view, WrappedEditor *w)
{
    if (!w->display) return;
    GdkClipboard *primary = gtk_widget_get_primary_clipboard(view);
    gtk_text_buffer_remove_selection_clipboard(w->display, primary);
    gtk_text_buffer_add_selection_clipboard(w->document, primary);
}

static void primary_to_view(GtkWidget *view, WrappedEditor *w)
{
    if (!w->display) return;
    GdkClipboard *primary = gtk_widget_get_primary_clipboard(view);
    gtk_text_buffer_remove_selection_clipboard(w->document, primary);
    gtk_text_buffer_add_selection_clipboard(w->display, primary);
}

static void release_display(WrappedEditor *w)
{
    g_clear_handle_id(&w->idle, g_source_remove);
    g_clear_handle_id(&w->jump_idle, g_source_remove);
    if (w->thumb_events) {
        GtkWidget *bar = gtk_event_controller_get_widget(w->thumb_events);
        if (bar) gtk_widget_remove_controller(bar, w->thumb_events);
        g_clear_object(&w->thumb_events);
        w->dragging_thumb = FALSE;
    }
    if (w->scroll) {
        g_signal_handlers_disconnect_by_data(w->scroll, w);
        g_clear_object(&w->scroll);
    }
    gtk_text_buffer_remove_commit_notify(w->document, w->commit);
    g_signal_handlers_disconnect_by_data(w->display, w);
    g_signal_handlers_disconnect_by_data(w->display, w->document);
    g_clear_object(&w->display);
    g_clear_pointer(&w->breaks, g_array_unref);
}

static void free_wrapped_editor(gpointer data)
{
    WrappedEditor *w = data;
    if (w->display) release_display(w);
    g_signal_handlers_disconnect_by_data(w->document, w);
    g_object_unref(w->document);
    g_free(w);
}

void editor_wrap_enable(GtkTextView *view)
{
    WrappedEditor *w = wrapped_editor(view);
    if (!w) {
        w = g_new0(WrappedEditor, 1);
        w->view = view;
        w->document = g_object_ref(gtk_text_view_get_buffer(view));
        g_object_set_data_full(G_OBJECT(view), "wrapped-editor", w, free_wrapped_editor);
        g_signal_connect(w->document, "mark-set", G_CALLBACK(mark_set), w);
        g_signal_connect_after(view, "move-cursor", G_CALLBACK(moved_cursor), w);
        g_signal_connect(view, "copy-clipboard", G_CALLBACK(copy_selection), w);
        g_signal_connect(view, "cut-clipboard", G_CALLBACK(cut_selection), w);
        g_signal_connect_after(view, "realize", G_CALLBACK(primary_to_document), w);
        g_signal_connect(view, "unrealize", G_CALLBACK(primary_to_view), w);
    }
    if (w->display) return;
    GtkSourceBuffer *display = gtk_source_buffer_new(NULL);
    w->display = GTK_TEXT_BUFFER(display);
    w->breaks = g_array_new(FALSE, FALSE, sizeof(int));
    gtk_source_buffer_set_style_scheme(display, gtk_source_buffer_get_style_scheme(GTK_SOURCE_BUFFER(w->document)));
    gtk_source_buffer_set_highlight_syntax(display, FALSE);
    gtk_source_buffer_set_highlight_matching_brackets(display, FALSE);
    gtk_text_buffer_set_enable_undo(w->display, FALSE);
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(w->document, &a, &b);
    g_autofree char *original = gtk_text_buffer_get_text(w->document, &a, &b, TRUE);
    g_autoptr(GString) visual = wrap_text(original, strlen(original), 0, g_str_is_ascii(original), w->breaks);
    gtk_text_buffer_set_text(w->display, visual->str, visual->len);
    w->soft_break = gtk_text_buffer_create_tag(w->display, NULL, "invisible", TRUE, NULL);
    tag_breaks(w, 0, w->breaks->len);
    gtk_widget_action_set_enabled(GTK_WIDGET(view), "editor.format", TRUE);
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(view), FALSE);
    gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(view), FALSE);
    gtk_text_view_set_buffer(view, w->display);
    gtk_text_view_set_wrap_mode(view, GTK_WRAP_CHAR);
    GtkAdjustment *scroll = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view));
    if (scroll) {
        w->scroll = g_object_ref(scroll);
        w->last_scroll = gtk_adjustment_get_value(scroll);
        range_changed(scroll, w);
        g_signal_connect(scroll, "changed", G_CALLBACK(range_changed), w);
        g_signal_connect(scroll, "value-changed", G_CALLBACK(scrolled), w);
    }
    GtkWidget *container = gtk_widget_get_ancestor(GTK_WIDGET(view), GTK_TYPE_SCROLLED_WINDOW);
    if (container) {
        GtkWidget *bar = gtk_scrolled_window_get_vscrollbar(GTK_SCROLLED_WINDOW(container));
        w->thumb_events = gtk_event_controller_legacy_new();
        gtk_event_controller_set_propagation_phase(w->thumb_events, GTK_PHASE_CAPTURE);
        g_signal_connect(w->thumb_events, "event", G_CALLBACK(thumb_event), w);
        gtk_widget_add_controller(bar, g_object_ref(w->thumb_events));
    }
    if (gtk_widget_get_realized(GTK_WIDGET(view))) primary_to_document(GTK_WIDGET(view), w);
    w->commit = gtk_text_buffer_add_commit_notify(w->document,
        GTK_TEXT_BUFFER_NOTIFY_AFTER_INSERT | GTK_TEXT_BUFFER_NOTIFY_BEFORE_DELETE, document_commit, w, NULL);
    g_signal_connect(w->display, "mark-set", G_CALLBACK(mark_set), w);
    g_signal_connect(w->display, "insert-text", G_CALLBACK(display_insert), w);
    g_signal_connect(w->display, "delete-range", G_CALLBACK(display_delete), w);
    g_signal_connect_swapped(w->display, "begin-user-action", G_CALLBACK(gtk_text_buffer_begin_user_action), w->document);
    g_signal_connect_swapped(w->display, "end-user-action", G_CALLBACK(gtk_text_buffer_end_user_action), w->document);
    sync_selection(w, TRUE);
}

void editor_wrap_disable(GtkTextView *view)
{
    WrappedEditor *w = wrapped_editor(view);
    if (!w || !w->display) return;
    if (gtk_widget_get_realized(GTK_WIDGET(view))) primary_to_view(GTK_WIDGET(view), w);
    release_display(w);
    gtk_widget_action_set_enabled(GTK_WIDGET(view), "editor.format", FALSE);
    gtk_text_view_set_buffer(view, w->document);
    gtk_text_view_set_wrap_mode(view, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "preferred-wrap")));
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(view), TRUE);
    gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(view), TRUE);
}
