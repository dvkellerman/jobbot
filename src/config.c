#include "config.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

#define SOURCE_PREFIX "source:"
#define DEFAULT_USER_AGENT "jobbot/" JOBBOT_VERSION " (personal job alerts)"

static const char *const GENERAL_KEYS[] = {"db_path",     "user_agent",           "poll_interval_minutes",
                                           "dedupe_days", "alert_after_failures", "log_level",
                                           NULL};
static const char *const TELEGRAM_KEYS[] = {"bot_token", "chat_id", "api_base", NULL};
static const char *const MATCH_KEYS[] = {"include", "exclude", "match_tags", NULL};
static const char *const SOURCE_COMMON_KEYS[] = {"type",        "enabled",          "label", "country",
                                                 "allow_empty", "interval_minutes", NULL};

/* Formats "PATH: MESSAGE" into `err` and returns -1. */
static int fail(char *err, size_t errlen, const char *path, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int fail(char *err, size_t errlen, const char *path, const char *fmt, ...)
{
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(err, errlen, "%s: %s", path, msg);
    return -1;
}

static int check_keys(const ini_section *sec, const char *const *allowed, const char *path,
                      char *err, size_t errlen)
{
    if (!sec)
        return 0;
    const ini_kv *bad = ini_unknown_key(sec, allowed);
    if (bad)
        return fail(err, errlen, path, "line %d: unknown setting '%s' in [%s]", bad->line, bad->key,
                    sec->name);
    return 0;
}

static int parse_log_level(const char *name, int *level)
{
    static const char *const names[] = {"debug", "info", "warn", "error"};
    for (int i = 0; i < 4; i++) {
        if (strcasecmp(name, names[i]) == 0) {
            *level = i;
            return 0;
        }
    }
    return -1;
}

/* A bot token looks like "123456789:AAH...": digits, a colon, then letters,
 * digits, '-' and '_'. Checking the shape also keeps anything odd out of
 * the request URL the token is placed in. */
static int valid_token(const char *t)
{
    const char *colon = strchr(t, ':');
    if (!colon || colon == t || colon[1] == '\0')
        return 0;
    for (const char *p = t; p < colon; p++)
        if (!isdigit((unsigned char)*p))
            return 0;
    for (const char *p = colon + 1; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
            return 0;
    return 1;
}

/* A chat id is a signed integer, or "@channelname". */
static int valid_chat_id(const char *c)
{
    if (*c == '@') {
        if (c[1] == '\0')
            return 0;
        for (const char *p = c + 1; *p; p++)
            if (!isalnum((unsigned char)*p) && *p != '_')
                return 0;
        return 1;
    }
    const char *p = c;
    if (*p == '-')
        p++;
    if (*p == '\0')
        return 0;
    for (; *p; p++)
        if (!isdigit((unsigned char)*p))
            return 0;
    return 1;
}

static int valid_source_name(const char *n)
{
    if (*n == '\0')
        return 0;
    for (; *n; n++)
        if (!isalnum((unsigned char)*n) && *n != '-' && *n != '_')
            return 0;
    return 1;
}

static int load_general(settings *s, const char *path, char *err, size_t errlen)
{
    const ini_section *g = ini_find(&s->ini, "general");
    char msg[300];
    long minutes, days;

    if (check_keys(g, GENERAL_KEYS, path, err, errlen) != 0)
        return -1;
    if (ini_get_long(g, "poll_interval_minutes", 20, 5, 1440, &minutes, msg, sizeof msg) != 0 ||
        ini_get_long(g, "dedupe_days", 30, 0, 365, &days, msg, sizeof msg) != 0 ||
        ini_get_long(g, "alert_after_failures", 3, 1, 100, &s->alert_after_failures, msg,
                     sizeof msg) != 0)
        return fail(err, errlen, path, "%s", msg);
    s->poll_interval_s = minutes * 60;
    s->dedupe_window_s = days * 86400;

    const char *level = ini_get(g, "log_level", "info");
    if (parse_log_level(level, &s->log_level) != 0)
        return fail(err, errlen, path, "'log_level' in [general] must be debug, info, warn or error, got '%s'",
                    level);

    const char *db = ini_get(g, "db_path", "");
    if (*db == '\0')
        return fail(err, errlen, path, "'db_path' in [general] must be set to the database file path");
    s->db_path = path_expand_home(db);
    if (!s->db_path)
        return fail(err, errlen, path,
                    "'db_path' in [general] starts with ~/ but HOME is not set; give the full path");

    const char *ua = ini_get(g, "user_agent", DEFAULT_USER_AGENT);
    if (*ua == '\0')
        return fail(err, errlen, path, "'user_agent' in [general] must not be empty");
    s->user_agent = xstrdup(ua);
    return 0;
}

static int load_telegram(settings *s, int need, const char *path, char *err, size_t errlen)
{
    const ini_section *t = ini_find(&s->ini, "telegram");
    if (check_keys(t, TELEGRAM_KEYS, path, err, errlen) != 0)
        return -1;

    const char *base = ini_get(t, "api_base", "https://api.telegram.org");
    if (strncmp(base, "https://", 8) != 0 && strncmp(base, "http://", 7) != 0)
        return fail(err, errlen, path, "'api_base' in [telegram] must be an http(s) URL");
    s->tg_api_base = xstrdup(base);
    size_t blen = strlen(s->tg_api_base);
    while (blen && s->tg_api_base[blen - 1] == '/')
        s->tg_api_base[--blen] = '\0';

    /* The environment wins, so the token can be kept out of the file. */
    const char *token = getenv(JOBBOT_TOKEN_ENV);
    const char *token_from = "the " JOBBOT_TOKEN_ENV " environment variable";
    if (!token || !*token) {
        token = ini_get(t, "bot_token", "");
        token_from = "'bot_token' in [telegram]";
    }
    const char *chat = ini_get(t, "chat_id", "");

    if (*token) {
        if (!valid_token(token))
            return fail(err, errlen, path,
                        "%s does not look like a Telegram bot token (expected digits, ':', then "
                        "letters/digits)",
                        token_from);
        s->tg_bot_token = xstrdup(token);
    } else if (need) {
        return fail(err, errlen, path,
                    "no Telegram bot token: set 'bot_token' in [telegram] or the " JOBBOT_TOKEN_ENV
                    " environment variable");
    }

    if (*chat) {
        if (!valid_chat_id(chat))
            return fail(err, errlen, path,
                        "'chat_id' in [telegram] must be a number (e.g. 123456789) or @channelname, "
                        "got '%s'",
                        chat);
        s->tg_chat_id = xstrdup(chat);
    } else if (need) {
        return fail(err, errlen, path, "'chat_id' in [telegram] must be set");
    }
    return 0;
}

static int load_match(settings *s, const char *path, char *err, size_t errlen)
{
    const ini_section *m = ini_find(&s->ini, "match");
    char msg[300];
    if (check_keys(m, MATCH_KEYS, path, err, errlen) != 0)
        return -1;

    keywords_parse(&s->match.include, ini_get(m, "include", ""));
    keywords_parse(&s->match.exclude, ini_get(m, "exclude", ""));
    if (s->match.include.n == 0)
        return fail(err, errlen, path,
                    "'include' in [match] must list at least one keyword (e.g. ios, swift)");
    if (ini_get_bool(m, "match_tags", 0, &s->match.match_tags, msg, sizeof msg) != 0)
        return fail(err, errlen, path, "%s", msg);
    return 0;
}

static int load_source(settings *s, const ini_section *sec, const char *path, char *err,
                       size_t errlen)
{
    const char *name = sec->name + strlen(SOURCE_PREFIX);
    char msg[300];

    if (!valid_source_name(name))
        return fail(err, errlen, path,
                    "line %d: source name in [%s] may only use letters, digits, '-' and '_'",
                    sec->line, sec->name);

    const char *type_name = ini_get(sec, "type", "");
    const source_type *type = source_type_find(type_name);
    if (!type) {
        char known[200];
        source_type_names(known, sizeof known);
        return fail(err, errlen, path, "[%s]: 'type' must be one of: %s (got '%s')", sec->name,
                    known, type_name);
    }

    /* Accept the common keys plus the ones this source type declares. */
    size_t ncommon = sizeof SOURCE_COMMON_KEYS / sizeof SOURCE_COMMON_KEYS[0] - 1;
    size_t nown = 0;
    while (type->keys && type->keys[nown])
        nown++;
    const char **allowed = xcalloc(ncommon + nown + 1, sizeof *allowed);
    memcpy(allowed, SOURCE_COMMON_KEYS, ncommon * sizeof *allowed);
    if (nown)
        memcpy(allowed + ncommon, type->keys, nown * sizeof *allowed);
    int rc = check_keys(sec, allowed, path, err, errlen);
    free(allowed);
    if (rc != 0)
        return -1;

    int enabled, allow_empty;
    long interval_min;
    if (ini_get_bool(sec, "enabled", 1, &enabled, msg, sizeof msg) != 0 ||
        ini_get_bool(sec, "allow_empty", 0, &allow_empty, msg, sizeof msg) != 0 ||
        ini_get_long(sec, "interval_minutes", s->poll_interval_s / 60, 5, 1440, &interval_min, msg,
                     sizeof msg) != 0)
        return fail(err, errlen, path, "%s", msg);

    /* A disabled source is still checked, so a typo shows up before it is
     * switched on. */
    if (type->validate && type->validate(sec, msg, sizeof msg) != 0)
        return fail(err, errlen, path, "[%s]: %s", sec->name, msg);
    if (!enabled)
        return 0;

    s->sources = xrealloc(s->sources, (s->nsources + 1) * sizeof *s->sources);
    source_def *def = &s->sources[s->nsources++];
    memset(def, 0, sizeof *def);
    def->name = xstrdup(name);
    def->type = type;
    def->cfg = sec;
    def->allow_empty = allow_empty;
    def->interval_s = interval_min * 60;

    const char *label = ini_get(sec, "label", name);
    const char *country = ini_get(sec, "country", "");
    buf lb;
    buf_init(&lb);
    buf_puts(&lb, *label ? label : name);
    if (*country)
        buf_printf(&lb, " \xC2\xB7 %s", country); /* middle dot */
    def->label = buf_steal(&lb);
    return 0;
}

int settings_load(settings *s, const char *path, int need_telegram, char *err, size_t errlen)
{
    memset(s, 0, sizeof *s);
    if (ini_load(&s->ini, path, err, errlen) != 0)
        return -1;

    for (size_t i = 0; i < s->ini.n; i++) {
        const char *name = s->ini.sections[i].name;
        if (strcmp(name, "general") != 0 && strcmp(name, "telegram") != 0 &&
            strcmp(name, "match") != 0 && strncmp(name, SOURCE_PREFIX, strlen(SOURCE_PREFIX)) != 0) {
            fail(err, errlen, path,
                 "line %d: unknown section [%s] (expected general, telegram, match or source:NAME)",
                 s->ini.sections[i].line, name);
            goto bad;
        }
    }

    if (load_general(s, path, err, errlen) != 0 ||
        load_telegram(s, need_telegram, path, err, errlen) != 0 ||
        load_match(s, path, err, errlen) != 0)
        goto bad;

    for (size_t i = 0; i < s->ini.n; i++) {
        const ini_section *sec = &s->ini.sections[i];
        if (strncmp(sec->name, SOURCE_PREFIX, strlen(SOURCE_PREFIX)) != 0)
            continue;
        if (load_source(s, sec, path, err, errlen) != 0)
            goto bad;
    }
    if (s->nsources == 0) {
        fail(err, errlen, path, "no enabled [source:NAME] sections; there is nothing to monitor");
        goto bad;
    }
    return 0;

bad:
    settings_free(s);
    return -1;
}

void settings_free(settings *s)
{
    for (size_t i = 0; i < s->nsources; i++) {
        free(s->sources[i].name);
        free(s->sources[i].label);
    }
    free(s->sources);
    matcher_free(&s->match);
    free(s->db_path);
    free(s->user_agent);
    free(s->tg_api_base);
    free(s->tg_bot_token);
    free(s->tg_chat_id);
    ini_free(&s->ini);
    memset(s, 0, sizeof *s);
}

const source_def *settings_source(const settings *s, const char *name)
{
    for (size_t i = 0; i < s->nsources; i++)
        if (strcmp(s->sources[i].name, name) == 0)
            return &s->sources[i];
    return NULL;
}
