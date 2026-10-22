#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "terminal.h"

/*
 * Sending paste data (clipboard, primary selection, drag-and-drop) to
 * the client.
 *
 * With paste protection enabled, the data is buffered instead, and
 * multi-line text is only sent once the user has confirmed it.
 */
void paste_begin(struct terminal *term);
void paste_data(struct terminal *term, const char *data, size_t size);
void paste_end(struct terminal *term);

/* True while a paste is being sent, or waiting for confirmation */
bool paste_in_progress(const struct terminal *term);

/* Whether a paste beginning now would need confirmation, if multi-line */
bool paste_confirm_wanted(const struct terminal *term);

void paste_confirm_accept(struct terminal *term);
void paste_confirm_cancel(struct terminal *term);
void paste_confirm_input(struct seat *seat, struct terminal *term,
                         xkb_keysym_t sym);

/* The text shown in the confirmation prompt */
void paste_confirm_prompt(const struct terminal *term, char *buf, size_t size);

/* Number of line breaks (\n, \r or \r\n) in the data */
size_t paste_count_line_breaks(const char *data, size_t len);
