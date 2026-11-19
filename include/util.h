#ifndef JOBBOT_UTIL_H
#define JOBBOT_UTIL_H

#include <stddef.h>

/* Allocation helpers. Out of memory is fatal: the process aborts, and the
 * next run starts afresh. */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* Growable byte buffer. `data` is always NUL-terminated once anything has
 * been appended; `len` excludes the terminator. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} buf;

void buf_init(buf *b);
void buf_free(buf *b);
void buf_clear(buf *b);
void buf_append(buf *b, const void *p, size_t n);
void buf_puts(buf *b, const char *s);
void buf_putc(buf *b, char c);
void buf_printf(buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Returns the buffer's string (never NULL) and resets the buffer. */
char *buf_steal(buf *b);

/* Logging to stderr. Each line starts with the local date and time. */
enum { LOG_DEBUG = 0, LOG_INFO = 1, LOG_WARN = 2, LOG_ERROR = 3 };
void log_set_level(int level);
void log_msg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define log_debug(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define log_info(...) log_msg(LOG_INFO, __VA_ARGS__)
#define log_warn(...) log_msg(LOG_WARN, __VA_ARGS__)
#define log_error(...) log_msg(LOG_ERROR, __VA_ARGS__)

/* Cooperative shutdown. stop_request() is async-signal-safe. */
void stop_request(void);
int stop_requested(void);
/* Sleeps for `seconds`. Returns 0 after the full sleep, -1 when cut short
 * because a stop was requested (checked at least once a second). */
int sleep_seconds(long seconds);

/* Trims ASCII whitespace in place and returns `s`. */
char *str_trim(char *s);
/* True when `s` is NULL or contains only ASCII whitespace. */
int str_blank(const char *s);
/* Replaces the string in `*dst` with a copy of `src` (NULL allowed). */
void str_set(char **dst, const char *src);

/* Returns a new copy of `path` with a leading "~/" replaced by the home
 * directory ($HOME). Returns NULL when the path starts with "~/" and HOME is
 * not set. */
char *path_expand_home(const char *path);

#endif
