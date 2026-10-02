#ifndef IRSSI_FE_COMMON_CORE_FE_EXPANDOS_H
#define IRSSI_FE_COMMON_CORE_FE_EXPANDOS_H

#include <glib.h>

/* Nick hash colour palette (setting nick_hash_colors). */
char *nick_palette_entry_normalize(const char *entry);
gchar **parse_color_palette(const char *colors_str, int *count);

#endif
