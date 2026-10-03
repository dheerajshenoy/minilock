#pragma once

#include "config.h"

struct image;

/* The processed image (decoded, tinted, blurred) is cached on disk so the
 * next start can skip decoding and the effects. An entry is keyed on the
 * image's real path, size and modification time, plus every setting that
 * changes the pixels, so editing the file or the config picks a new entry.
 *
 * Entries live in $XDG_CACHE_HOME/minilock (or ~/.cache/minilock). */

/* Returns a newly allocated image, or NULL if there is no valid entry. */
struct image *
cache_load(const char *path, const struct Config *cfg);

/* Saves the processed image; failures are reported but not fatal. */
void
cache_store(const char *path, const struct Config *cfg,
            const struct image *img);
