#include "util.h"

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void oom(void)
{
    fputs("jobbot: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        oom();
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p)
        oom();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        oom();
    return q;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void buf_init(buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(buf *b)
{
    free(b->data);
    buf_init(b);
}

void buf_clear(buf *b)
{
    b->len = 0;
    if (b->data)
        b->data[0] = '\0';
}

static void buf_reserve(buf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need)
        cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_append(buf *b, const void *p, size_t n)
{
    buf_reserve(b, n);
    if (n)
        memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_puts(buf *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

void buf_putc(buf *b, char c)
{
    buf_append(b, &c, 1);
}

void buf_printf(buf *b, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        buf_reserve(b, (size_t)n);
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

char *buf_steal(buf *b)
{
    char *s = b->data ? b->data : xstrdup("");
    buf_init(b);
    return s;
}

static int g_log_level = LOG_INFO;

void log_set_level(int level)
{
    g_log_level = level;
}

void log_msg(int level, const char *fmt, ...)
{
    static const char *names[] = {"debug", "info", "warn", "error"};
    if (level < g_log_level)
        return;
    if (level < LOG_DEBUG || level > LOG_ERROR)
        level = LOG_ERROR;

    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);

    fprintf(stderr, "%s %-5s ", ts, names[level]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static volatile sig_atomic_t g_stop;

void stop_request(void)
{
    g_stop = 1;
}

int stop_requested(void)
{
    return g_stop != 0;
}

int sleep_seconds(long seconds)
{
    while (seconds > 0) {
        if (g_stop)
            return -1;
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL); /* a signal may cut this short; the loop re-checks */
        seconds--;
    }
    return g_stop ? -1 : 0;
}

char *str_trim(char *s)
{
    char *start = s;
    while (*start && isspace((unsigned char)*start))
        start++;
    size_t n = strlen(start);
    while (n && isspace((unsigned char)start[n - 1]))
        n--;
    if (start != s)
        memmove(s, start, n);
    s[n] = '\0';
    return s;
}

int str_blank(const char *s)
{
    if (!s)
        return 1;
    for (; *s; s++)
        if (!isspace((unsigned char)*s))
            return 0;
    return 1;
}

void str_set(char **dst, const char *src)
{
    char *copy = src ? xstrdup(src) : NULL;
    free(*dst);
    *dst = copy;
}

char *path_expand_home(const char *path)
{
    if (path[0] != '~' || path[1] != '/')
        return xstrdup(path);
    const char *home = getenv("HOME");
    if (!home || !*home)
        return NULL;
    buf out;
    buf_init(&out);
    buf_printf(&out, "%s%s", home, path + 1);
    return buf_steal(&out);
}
