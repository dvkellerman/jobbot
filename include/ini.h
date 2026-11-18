#ifndef JOBBOT_INI_H
#define JOBBOT_INI_H

#include <stddef.h>

/* Minimal INI reader.
 *
 *   # full-line comment (also ';')
 *   [section]
 *   key = value
 *
 * Values run to the end of the line (no inline comments, so URLs containing
 * '#' or ';' are safe). A value wrapped in double quotes has them removed.
 * Duplicate sections and duplicate keys within a section are errors. */

typedef struct {
    char *key;
    char *val;
    int line;
} ini_kv;

typedef struct {
    char *name;
    ini_kv *kv;
    size_t n;
    int line;
} ini_section;

typedef struct {
    ini_section *sections;
    size_t n;
} ini_file;

/* Returns 0 on success. On failure returns -1 and writes a message that
 * includes the path and line number to `err`. */
int ini_load(ini_file *f, const char *path, char *err, size_t errlen);
int ini_parse(ini_file *f, const char *text, const char *name, char *err, size_t errlen);
void ini_free(ini_file *f);

const ini_section *ini_find(const ini_file *f, const char *name);
/* Returns the value, or `def` when the key is absent. */
const char *ini_get(const ini_section *s, const char *key, const char *def);

/* Typed getters: return 0 on success (with `*out` set to the value or to
 * `def` when absent), -1 with a message in `err` when the value is invalid. */
int ini_get_long(const ini_section *s, const char *key, long def, long min, long max,
                 long *out, char *err, size_t errlen);
int ini_get_bool(const ini_section *s, const char *key, int def, int *out, char *err,
                 size_t errlen);

/* Returns the first key in `s` that is not in the NULL-terminated `allowed`
 * list, or NULL when every key is allowed. */
const ini_kv *ini_unknown_key(const ini_section *s, const char *const *allowed);

#endif
