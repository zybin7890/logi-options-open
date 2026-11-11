/* SPDX-License-Identifier: MIT */
#pragma once
#include <meta/meta-selection-source.h>

G_BEGIN_DECLS
#define LOGI_CLIPBOARD_TYPE_SOURCE (logi_clipboard_source_get_type())
G_DECLARE_FINAL_TYPE(LogiClipboardSource, logi_clipboard_source,
                     LOGI_CLIPBOARD, SOURCE, MetaSelectionSource)

/**
 * logi_clipboard_source_new:
 * @text: immutable UTF-8 text offered to native clipboard readers
 *
 * Creates a source whose consumed signal reports a real data-stream request.
 * This does not assert that the intended application inserted the text.
 *
 * Returns: (transfer full): a new native selection source
 */
LogiClipboardSource *logi_clipboard_source_new(const char *text);
G_END_DECLS
