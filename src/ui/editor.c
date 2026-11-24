#include "editor.h"
#include "editor_wrap.h"

#include <gtksourceview/gtksource.h>
#include <string.h>

/* GTK's editing menu and keyboard shortcuts both use these widget actions. */
typedef struct { GtkSourceView parent; } ScriptEditor;
typedef GtkSourceViewClass ScriptEditorClass;
G_DEFINE_TYPE(ScriptEditor, script_editor, GTK_SOURCE_TYPE_VIEW)

static void history_action(GtkWidget *widget, const char *name, GVariant *parameter)
{
    (void)parameter;
    GtkTextBuffer *document = editor_document(GTK_TEXT_VIEW(widget));
    if (g_str_equal(name, "text.undo")) gtk_text_buffer_undo(document);
    else gtk_text_buffer_redo(document);
}

typedef struct {
    GWeakRef editor;
    GCancellable *cancel;
} FormatRequest;

static void format_error(GtkWidget *editor, const char *detail)
{
    GtkRoot *root = gtk_widget_get_root(editor);
    if (!GTK_IS_WINDOW(root)) return;
    g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new("Could not format this script");
    gtk_alert_dialog_set_detail(dialog, detail);
    gtk_alert_dialog_show(dialog, GTK_WINDOW(root));
}

static void stop_format(gpointer data)
{
    g_cancellable_cancel(data);
    g_object_unref(data);
}

static void format_finished(GObject *source, GAsyncResult *result, gpointer data)
{
    FormatRequest *request = data;
    g_autoptr(GtkWidget) editor = g_weak_ref_get(&request->editor);
    GSubprocess *process = G_SUBPROCESS(source);
    g_autoptr(GError) error = NULL;
    g_autofree char *formatted = NULL, *diagnostic = NULL;
    gboolean completed = g_subprocess_communicate_utf8_finish(process, result, &formatted, &diagnostic, &error);
    if (editor) {
        GCancellable *pending = g_object_steal_data(G_OBJECT(editor), "format-request");
        g_clear_object(&pending);
    }
    if (editor && !g_cancellable_is_cancelled(request->cancel)) {
        GtkTextBuffer *document = editor_document(GTK_TEXT_VIEW(editor));
        g_signal_handlers_disconnect_by_data(document, request->cancel);
        if (!completed) {
            format_error(editor, error->message);
        } else if (!g_subprocess_get_successful(process)) {
            g_autofree char *detail = diagnostic && *diagnostic ?
                g_utf8_substring(diagnostic, 0, MIN(500, g_utf8_strlen(diagnostic, -1))) : NULL;
            format_error(editor, detail ? g_strstrip(detail) : "StyLua could not parse this script.");
        } else if (strlen(formatted) > EditorFileLimitBytes) {
            format_error(editor, "The formatted script exceeds the 8 MiB file limit.");
        } else {
            GtkTextIter a, b;
            gtk_text_buffer_get_bounds(document, &a, &b);
            gtk_text_buffer_begin_user_action(document);
            gtk_text_buffer_delete(document, &a, &b);
            gtk_text_buffer_insert(document, &a, formatted, -1);
            gtk_text_buffer_get_start_iter(document, &a);
            gtk_text_buffer_place_cursor(document, &a);
            gtk_text_buffer_end_user_action(document);
            editor_reveal_cursor(GTK_TEXT_VIEW(editor));
        }
    }
    g_weak_ref_clear(&request->editor);
    g_object_unref(request->cancel);
    g_free(request);
}

static void format_document(GtkWidget *widget, const char *name, GVariant *parameter)
{
    (void)name; (void)parameter;
    if (g_object_get_data(G_OBJECT(widget), "format-request")) return;
    g_autoptr(GError) error = NULL;
    g_autoptr(GSubprocess) process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error,
        "stylua", "--syntax", "Luau", "--verify", "-", NULL);
    if (!process) {
        format_error(widget, g_error_matches(error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT) ?
            "Install StyLua with Luau support and make stylua available on your PATH." : error->message);
        return;
    }
    FormatRequest *request = g_new0(FormatRequest, 1);
    g_weak_ref_init(&request->editor, widget);
    request->cancel = g_cancellable_new();
    GtkTextBuffer *document = editor_document(GTK_TEXT_VIEW(widget));
    g_signal_connect_object(document, "changed", G_CALLBACK(g_cancellable_cancel), request->cancel, G_CONNECT_SWAPPED);
    g_signal_connect_object(request->cancel, "cancelled", G_CALLBACK(g_subprocess_force_exit), process, G_CONNECT_SWAPPED);
    g_object_set_data_full(G_OBJECT(widget), "format-request", g_object_ref(request->cancel), stop_format);
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(document, &a, &b);
    g_autofree char *input = gtk_text_buffer_get_text(document, &a, &b, TRUE);
    g_subprocess_communicate_utf8_async(process, input, request->cancel, format_finished, request);
}

static void script_editor_class_init(ScriptEditorClass *klass)
{
    gtk_widget_class_install_action(GTK_WIDGET_CLASS(klass), "editor.format", NULL, format_document);
    gtk_widget_class_install_action(GTK_WIDGET_CLASS(klass), "text.undo", NULL, history_action);
    gtk_widget_class_install_action(GTK_WIDGET_CLASS(klass), "text.redo", NULL, history_action);
}

static void script_editor_init(ScriptEditor *editor) { (void)editor; }

enum { LongLineLimit = 16 * 1024 };

static void set_highlighting(GtkSourceBuffer *buffer, gboolean enabled)
{
    GtkSourceLanguage *lua = g_object_get_data(G_OBJECT(buffer), "lua-language");
    gtk_source_buffer_set_highlight_syntax(buffer, enabled);
    gtk_source_buffer_set_language(buffer, enabled ? lua : NULL);
    gtk_source_buffer_set_highlight_matching_brackets(buffer, enabled);
}

static gboolean recheck_lines(gpointer data)
{
    GtkTextView *view = data;
    GtkSourceBuffer *buffer = GTK_SOURCE_BUFFER(editor_document(view));
    g_object_set_data(G_OBJECT(view), "highlight-recheck", NULL);
    GtkTextIter line;
    gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(buffer), &line);
    do {
        int bytes = gtk_text_iter_get_bytes_in_line(&line);
        if (bytes > LongLineLimit) return G_SOURCE_REMOVE;
    } while (gtk_text_iter_forward_line(&line));
    editor_wrap_disable(view);
    set_highlighting(buffer, TRUE);
    return G_SOURCE_REMOVE;
}

static void recheck_after_edit(GtkTextView *view)
{
    if (g_object_get_data(G_OBJECT(view), "highlight-recheck")) return;
    g_object_set_data(G_OBJECT(view), "highlight-recheck", GINT_TO_POINTER(1));
    /* Restore the native buffer before GTK lays out a newly formatted file. */
    g_idle_add_full(G_PRIORITY_HIGH_IDLE, recheck_lines, g_object_ref(view), g_object_unref);
}

/* Check the incoming text before the language engine sees it. Measuring only
 * the resulting lines also catches a paste into the middle of an existing line. */
static void inserting(GtkSourceBuffer *buffer, GtkTextIter *at, const char *text, int len, gpointer data)
{
    GtkTextView *view = data;
    gboolean colored = gtk_source_buffer_get_language(buffer) != NULL;

    int existing = gtk_text_iter_get_bytes_in_line(at);
    if (!colored && (memchr(text, '\n', len) || memchr(text, '\r', len) || g_strstr_len(text, len, "\u2029")))
        recheck_after_edit(view);
    if (!colored || len <= LongLineLimit - existing) return;

    int prefix = gtk_text_iter_get_line_index(at);
    int suffix = existing - prefix;
    gint64 longest = 0;
    while (len > 0) {
        int delimiter, next;
        pango_find_paragraph_boundary(text, len, &delimiter, &next);
        gint64 line = (gint64)prefix + delimiter;
        if (delimiter == len) line += suffix;
        longest = MAX(longest, line);
        if (longest > LongLineLimit) break;
        text += next;
        len -= next;
        prefix = 0;
    }
    if (longest > LongLineLimit) {
        set_highlighting(buffer, FALSE);
        editor_wrap_enable(view);
    }
}

static void deleting(GtkSourceBuffer *buffer, GtkTextIter *start, GtkTextIter *end, gpointer data)
{
    if (!gtk_source_buffer_get_language(buffer)) recheck_after_edit(GTK_TEXT_VIEW(data));
    if (gtk_text_iter_get_line(start) == gtk_text_iter_get_line(end)) return;
    gint64 merged = (gint64)gtk_text_iter_get_line_index(start) + gtk_text_iter_get_bytes_in_line(end)
        - gtk_text_iter_get_line_index(end);
    if (merged > LongLineLimit) {
        set_highlighting(buffer, FALSE);
        editor_wrap_enable(GTK_TEXT_VIEW(data));
    }
}

void editor_apply_settings(GtkWidget *editor, const EditorSettings *settings)
{
    editor_wrap_set_preference(GTK_TEXT_VIEW(editor),
        settings->word_wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(editor), settings->indent_width);
    gtk_source_view_set_indent_width(GTK_SOURCE_VIEW(editor), settings->indent_width);
}

GtkWidget *editor_new(const EditorSettings *settings)
{
    gtk_source_init();
    GtkSourceBuffer *buffer = gtk_source_buffer_new(NULL);

    GtkSourceLanguageManager *languages = gtk_source_language_manager_new();
    const char *language_paths[] = {"resource:///org/gnome/gtksourceview/language-specs", NULL};
    gtk_source_language_manager_set_search_path(languages, language_paths);
    GtkSourceLanguage *language = gtk_source_language_manager_get_language(languages, "lua");
    if (!language)
        g_error("GtkSourceView's bundled Lua language is unavailable");
    gtk_source_buffer_set_language(buffer, language);
    g_object_set_data(G_OBJECT(buffer), "lua-language", language);
    g_object_set_data_full(G_OBJECT(buffer), "languages", languages, g_object_unref);

    GtkSourceStyleSchemeManager *schemes = gtk_source_style_scheme_manager_new();
    const char *style_paths[] = {"resource:///org/relapse/RelapseGtk/styles", NULL};
    gtk_source_style_scheme_manager_set_search_path(schemes, style_paths);
    GtkSourceStyleScheme *scheme = gtk_source_style_scheme_manager_get_scheme(schemes, "relapse");
    if (!scheme)
        g_error("The bundled editor color scheme is unavailable");
    gtk_source_buffer_set_style_scheme(buffer, scheme);
    g_object_set_data_full(G_OBJECT(buffer), "styles", schemes, g_object_unref);
    gtk_source_buffer_set_highlight_syntax(buffer, TRUE);
    gtk_source_buffer_set_highlight_matching_brackets(buffer, TRUE);
    gtk_text_buffer_set_enable_undo(GTK_TEXT_BUFFER(buffer), TRUE);

    GtkWidget *editor = g_object_new(script_editor_get_type(), "buffer", buffer, NULL);
    g_signal_connect_object(buffer, "insert-text", G_CALLBACK(inserting), editor, 0);
    g_signal_connect_object(buffer, "delete-range", G_CALLBACK(deleting), editor, 0);
    g_autoptr(GMenu) menu = g_menu_new();
    GMenuModel *extra = gtk_text_view_get_extra_menu(GTK_TEXT_VIEW(editor));
    if (extra) g_menu_append_section(menu, NULL, extra);
    g_menu_append(menu, "Find and Replace…", "win.replace");
    g_autoptr(GMenuItem) format = g_menu_item_new("Format with StyLua", "editor.format");
    g_menu_item_set_attribute(format, "hidden-when", "s", "action-disabled");
    g_menu_append_item(menu, format);
    gtk_widget_action_set_enabled(editor, "editor.format", FALSE);
    gtk_text_view_set_extra_menu(GTK_TEXT_VIEW(editor), G_MENU_MODEL(menu));
    gtk_widget_add_css_class(editor, "code-editor");
    g_object_unref(buffer);
    GtkSourceView *view = GTK_SOURCE_VIEW(editor);
    gtk_source_view_set_show_line_numbers(view, TRUE);
    gtk_source_view_set_highlight_current_line(view, TRUE);
    gtk_source_view_set_auto_indent(view, TRUE);
    editor_apply_settings(editor, settings);
    gtk_source_view_set_insert_spaces_instead_of_tabs(view, TRUE);
    gtk_source_view_set_indent_on_tab(view, TRUE);
    gtk_source_view_set_smart_backspace(view, TRUE);
    gtk_source_view_set_smart_home_end(view, GTK_SOURCE_SMART_HOME_END_BEFORE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 10);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 10);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 10);
    return editor;
}
