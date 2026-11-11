/* SPDX-License-Identifier: MIT */
#include "clipboard-source.h"
#include <string.h>

struct _LogiClipboardSource {
    MetaSelectionSource parent_instance;
    GBytes *content;
};

G_DEFINE_TYPE(LogiClipboardSource, logi_clipboard_source, META_TYPE_SELECTION_SOURCE)
static guint consumed_signal;

static GList *source_mimetypes(MetaSelectionSource *source) {
    (void)source;
    GList *types = NULL;
    types = g_list_append(types, g_strdup("text/plain;charset=utf-8"));
    types = g_list_append(types, g_strdup("text/plain"));
    return g_list_append(types, g_strdup("UTF8_STRING"));
}

static void source_read_async(MetaSelectionSource *source, const char *mimetype,
                              GCancellable *cancellable, GAsyncReadyCallback callback,
                              gpointer user_data) {
    GTask *task = g_task_new(source, cancellable, callback, user_data);
    if (g_strcmp0(mimetype, "text/plain;charset=utf-8") != 0 &&
        g_strcmp0(mimetype, "text/plain") != 0 && g_strcmp0(mimetype, "UTF8_STRING") != 0) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "Unsupported clipboard MIME type");
    } else {
        LogiClipboardSource *self = LOGI_CLIPBOARD_SOURCE(source);
        GInputStream *stream = g_memory_input_stream_new_from_bytes(self->content);
        g_task_return_pointer(task, stream, g_object_unref);
    }
    g_object_unref(task);
}

static GInputStream *source_read_finish(MetaSelectionSource *source, GAsyncResult *result,
                                       GError **error) {
    g_return_val_if_fail(g_task_is_valid(result, source), NULL);
    GInputStream *stream = g_task_propagate_pointer(G_TASK(result), error);
    if (stream) g_signal_emit(source, consumed_signal, 0);
    return stream;
}

static void source_finalize(GObject *object) {
    LogiClipboardSource *self = LOGI_CLIPBOARD_SOURCE(object);
    g_clear_pointer(&self->content, g_bytes_unref);
    G_OBJECT_CLASS(logi_clipboard_source_parent_class)->finalize(object);
}

static void logi_clipboard_source_class_init(LogiClipboardSourceClass *klass) {
    MetaSelectionSourceClass *source_class = META_SELECTION_SOURCE_CLASS(klass);
    source_class->get_mimetypes = source_mimetypes;
    source_class->read_async = source_read_async;
    source_class->read_finish = source_read_finish;
    G_OBJECT_CLASS(klass)->finalize = source_finalize;
    /**
     * LogiClipboardSource::consumed:
     *
     * A reader requested the actual immutable stream. Clipboard managers may
     * also read it; use a receiver test to prove target-application insertion.
     */
    consumed_signal = g_signal_new("consumed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                   0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void logi_clipboard_source_init(LogiClipboardSource *self) { self->content = NULL; }

LogiClipboardSource *logi_clipboard_source_new(const char *text) {
    g_return_val_if_fail(text != NULL, NULL);
    LogiClipboardSource *self = g_object_new(LOGI_CLIPBOARD_TYPE_SOURCE, NULL);
    self->content = g_bytes_new(text, strlen(text));
    return self;
}
