#ifndef CANGO_PATH_H
#define CANGO_PATH_H

#include <stdbool.h>
#include <stddef.h>

/* One card name: no slash, dot-dot, quote, or control character. */
bool cango_path_name_ok(const char *name);

/* Join rel under root. Empty rel is the root itself. Rejects escape. */
bool cango_path_under(char *dst, size_t cap, const char *root, const char *rel);

/* True when target is open_dir, inside it, or a parent of it. */
bool cango_path_hits(const char *open_dir, const char *target);

#endif
