#pragma once

#include <stdbool.h>

#include "config.h"

/*
 * Prints the OpenType features of the font matching 'pattern' (or the
 * primary font, if NULL), and whether they are enabled with the
 * current configuration.
 */
bool fontinfo_list_features(const struct config *conf, const char *pattern);
