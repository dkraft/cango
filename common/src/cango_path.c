#include "cango_path.h"

#include <string.h>

bool cango_path_name_ok(const char *name)
{
    size_t i;
    size_t n;
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    n = strlen(name);
    if (n > 47) {
        return false;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c > 0x7e || c == '"' || c == '\\' || c == '/' || c == ':') {
            return false;
        }
    }
    return true;
}

bool cango_path_under(char *dst, size_t cap, const char *root, const char *rel)
{
    size_t used;
    if (dst == NULL || root == NULL || cap < 2) {
        return false;
    }
    if (rel == NULL) {
        rel = "";
    }
    if (rel[0] == '/' || strchr(rel, '\\') != NULL) {
        return false;
    }
    used = strlen(root);
    if (used + 1 > cap) {
        return false;
    }
    memcpy(dst, root, used + 1);
    while (*rel != '\0') {
        char name[48];
        size_t n = 0;
        while (*rel == '/') {
            rel++;
        }
        if (*rel == '\0') {
            break;
        }
        while (*rel != '\0' && *rel != '/') {
            if (n + 1 >= sizeof(name)) {
                return false;
            }
            name[n++] = *rel++;
        }
        name[n] = '\0';
        if (!cango_path_name_ok(name)) {
            return false;
        }
        if (used + 1 + n + 1 > cap) {
            return false;
        }
        dst[used++] = '/';
        memcpy(dst + used, name, n + 1);
        used += n;
    }
    return true;
}

bool cango_path_hits(const char *open_dir, const char *target)
{
    size_t a;
    size_t b;
    if (open_dir == NULL || target == NULL || open_dir[0] == '\0' || target[0] == '\0') {
        return false;
    }
    a = strlen(open_dir);
    b = strlen(target);
    if (a == b && strcmp(open_dir, target) == 0) {
        return true;
    }
    if (b > a && strncmp(target, open_dir, a) == 0 && target[a] == '/') {
        return true;
    }
    if (a > b && strncmp(open_dir, target, b) == 0 && open_dir[b] == '/') {
        return true;
    }
    return false;
}
