#include "picker.h"

typedef struct {
    PickerFinished callback;
    gpointer user_data;
    gboolean folder;
} PickerRequest;

static void picker_finished(GObject *dialog, GAsyncResult *result, gpointer user_data)
{
    PickerRequest *request = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = request->folder
        ? gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(dialog), result, &error)
        : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(dialog), result, &error);
    request->callback(file, error, request->user_data);
    g_free(request);
}

static void picker_start(GtkWindow *parent, const char *title, const char *accept_label,
                         GFile *initial_folder, GCancellable *cancellable,
                         PickerFinished callback, gpointer user_data, gboolean folder)
{
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, title);
    gtk_file_dialog_set_accept_label(dialog, accept_label);
    gtk_file_dialog_set_modal(dialog, TRUE);
    if (initial_folder)
        gtk_file_dialog_set_initial_folder(dialog, initial_folder);
    PickerRequest *request = g_new0(PickerRequest, 1);
    request->callback = callback;
    request->user_data = user_data;
    request->folder = folder;
    if (folder)
        gtk_file_dialog_select_folder(dialog, parent, cancellable, picker_finished, request);
    else
        gtk_file_dialog_open(dialog, parent, cancellable, picker_finished, request);
}

void picker_open_file(GtkWindow *parent, const char *title, const char *accept_label,
                      GFile *initial_folder, GCancellable *cancellable,
                      PickerFinished callback, gpointer user_data)
{
    picker_start(parent, title, accept_label, initial_folder, cancellable, callback, user_data, FALSE);
}

void picker_select_folder(GtkWindow *parent, const char *title, const char *accept_label,
                          GFile *initial_folder, GCancellable *cancellable,
                          PickerFinished callback, gpointer user_data)
{
    picker_start(parent, title, accept_label, initial_folder, cancellable, callback, user_data, TRUE);
}
