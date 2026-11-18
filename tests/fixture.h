#ifndef JOBBOT_TEST_FIXTURE_H
#define JOBBOT_TEST_FIXTURE_H

#include <stdio.h>
#include <stdlib.h>

/* Reads a whole file (path relative to the repository root, where `make
 * test` runs). Returns a NUL-terminated malloc'd buffer, or NULL. */
static inline char *fixture_read(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "cannot open fixture %s (run tests from the repository root)\n", path);
        return NULL;
    }
    char *data = NULL;
    size_t used = 0, cap = 0;
    for (;;) {
        if (used + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 65536;
            char *grown = realloc(data, cap);
            if (!grown) {
                free(data);
                fclose(fp);
                return NULL;
            }
            data = grown;
        }
        size_t n = fread(data + used, 1, 4096, fp);
        used += n;
        if (n < 4096)
            break;
    }
    fclose(fp);
    data[used] = '\0';
    if (len)
        *len = used;
    return data;
}

#endif
