#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

#include "key-binding.h"
#include "terminal.h"

void vimode_begin(struct terminal *term);
void vimode_cancel(struct terminal *term);

void vimode_input(struct seat *seat, struct terminal *term,
                  const struct key_binding_set *bindings, uint32_t key,
                  xkb_keysym_t sym, xkb_mod_mask_t mods, xkb_mod_mask_t consumed,
                  const xkb_keysym_t *raw_syms, size_t raw_count,
                  uint32_t serial);

/* View-relative position of the vi mode cursor, or {-1, -1} if vi
 * mode isn't active, or the cursor isn't in the view */
struct coord vimode_cursor_in_view(const struct terminal *term);
