#include "ini.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

static ini_section *add_section(ini_file *f, const char *name, int line)
{
    f->sections = xrealloc(f->sections, (f->n + 1) * sizeof *f->sections);
    ini_section *s = &f->sections[f->n++];
    s->name = xstrdup(name);
    s->kv = NULL;
    s->n = 0;
    s->line = line;
    return s;
}

static void add_kv(ini_section *s, const char *key, const char *val, int line)
{
    s->kv = xrealloc(s->kv, (s->n + 1) * sizeof *s->kv);
    ini_kv *kv = &s->kv[s->n++];
    kv->key = xstrdup(key);
    kv->val = xstrdup(val);
    kv->line = line;
}

int ini_parse(ini_file *f, const char *text, const char *name, char *err, size_t errlen)
{
    f->sections = NULL;
    f->n = 0;

    /* The section being filled is tracked by index: add_section() may move
     * the array. */
    size_t cur = (size_t)-1;
    int line = 0;
    const char *p = text;

    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char *raw = xstrndup(p, len);
        p += len + (eol ? 1 : 0);
        line++;

        char *s = str_trim(raw);
        if (*s == '\0' || *s == '#' || *s == ';') {
            free(raw);
            continue;
        }

        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end || end[1] != '\0') {
                snprintf(err, errlen, "%s:%d: malformed section header", name, line);
                free(raw);
                goto fail;
            }
            *end = '\0';
            char *sec = str_trim(s + 1);
            if (*sec == '\0') {
                snprintf(err, errlen, "%s:%d: empty section name", name, line);
                free(raw);
                goto fail;
            }
            if (ini_find(f, sec)) {
                snprintf(err, errlen, "%s:%d: duplicate section [%s]", name, line, sec);
                free(raw);
                goto fail;
            }
            add_section(f, sec, line);
            cur = f->n - 1;
            free(raw);
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            snprintf(err, errlen, "%s:%d: expected 'key = value'", name, line);
            free(raw);
            goto fail;
        }
        if (cur == (size_t)-1) {
            snprintf(err, errlen, "%s:%d: setting appears before any [section]", name, line);
            free(raw);
            goto fail;
        }
        *eq = '\0';
        char *key = str_trim(s);
        char *val = str_trim(eq + 1);
        if (*key == '\0') {
            snprintf(err, errlen, "%s:%d: missing key before '='", name, line);
            free(raw);
            goto fail;
        }
        size_t vlen = strlen(val);
        if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
            val[vlen - 1] = '\0';
            val++;
        }

        ini_section *sec = &f->sections[cur];
        for (size_t i = 0; i < sec->n; i++) {
            if (strcmp(sec->kv[i].key, key) == 0) {
                snprintf(err, errlen, "%s:%d: duplicate key '%s' in [%s] (first set on line %d)",
                         name, line, key, sec->name, sec->kv[i].line);
                free(raw);
                goto fail;
            }
        }
        add_kv(sec, key, val, line);
        free(raw);
    }
    return 0;

fail:
    ini_free(f);
    return -1;
}

int ini_load(ini_file *f, const char *path, char *err, size_t errlen)
{
    f->sections = NULL;
    f->n = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    buf text;
    buf_init(&text);
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, fp)) > 0)
        buf_append(&text, chunk, n);
    int read_failed = ferror(fp);
    fclose(fp);
    if (read_failed) {
        snprintf(err, errlen, "cannot read %s", path);
        buf_free(&text);
        return -1;
    }
    if (text.len != strlen(text.data ? text.data : "")) {
        snprintf(err, errlen, "%s: file contains NUL bytes", path);
        buf_free(&text);
        return -1;
    }

    int rc = ini_parse(f, text.data ? text.data : "", path, err, errlen);
    buf_free(&text);
    return rc;
}

void ini_free(ini_file *f)
{
    for (size_t i = 0; i < f->n; i++) {
        ini_section *s = &f->sections[i];
        for (size_t j = 0; j < s->n; j++) {
            free(s->kv[j].key);
            free(s->kv[j].val);
        }
        free(s->kv);
        free(s->name);
    }
    free(f->sections);
    f->sections = NULL;
    f->n = 0;
}

const ini_section *ini_find(const ini_file *f, const char *name)
{
    for (size_t i = 0; i < f->n; i++)
        if (strcmp(f->sections[i].name, name) == 0)
            return &f->sections[i];
    return NULL;
}

static const ini_kv *find_kv(const ini_section *s, const char *key)
{
    if (!s)
        return NULL;
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->kv[i].key, key) == 0)
            return &s->kv[i];
    return NULL;
}

const char *ini_get(const ini_section *s, const char *key, const char *def)
{
    const ini_kv *kv = find_kv(s, key);
    return kv ? kv->val : def;
}

int ini_get_long(const ini_section *s, const char *key, long def, long min, long max,
                 long *out, char *err, size_t errlen)
{
    const ini_kv *kv = find_kv(s, key);
    if (!kv) {
        *out = def;
        return 0;
    }
    char *end;
    errno = 0;
    long v = strtol(kv->val, &end, 10);
    if (errno || end == kv->val || *end != '\0') {
        snprintf(err, errlen, "line %d: '%s' in [%s] must be a whole number, got '%s'", kv->line,
                 key, s->name, kv->val);
        return -1;
    }
    if (v < min || v > max) {
        snprintf(err, errlen, "line %d: '%s' in [%s] must be between %ld and %ld, got %ld",
                 kv->line, key, s->name, min, max, v);
        return -1;
    }
    *out = v;
    return 0;
}

int ini_get_bool(const ini_section *s, const char *key, int def, int *out, char *err,
                 size_t errlen)
{
    static const char *const yes[] = {"true", "yes", "on", "1", NULL};
    static const char *const no[] = {"false", "no", "off", "0", NULL};

    const ini_kv *kv = find_kv(s, key);
    if (!kv) {
        *out = def;
        return 0;
    }
    for (size_t i = 0; yes[i]; i++) {
        if (strcasecmp(kv->val, yes[i]) == 0) {
            *out = 1;
            return 0;
        }
    }
    for (size_t i = 0; no[i]; i++) {
        if (strcasecmp(kv->val, no[i]) == 0) {
            *out = 0;
            return 0;
        }
    }
    snprintf(err, errlen, "line %d: '%s' in [%s] must be true or false, got '%s'", kv->line, key,
             s->name, kv->val);
    return -1;
}

const ini_kv *ini_unknown_key(const ini_section *s, const char *const *allowed)
{
    for (size_t i = 0; i < s->n; i++) {
        int ok = 0;
        for (size_t j = 0; allowed[j]; j++) {
            if (strcmp(s->kv[i].key, allowed[j]) == 0) {
                ok = 1;
                break;
            }
        }
        if (!ok)
            return &s->kv[i];
    }
    return NULL;
}
