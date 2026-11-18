#ifndef JOBBOT_CONFIG_H
#define JOBBOT_CONFIG_H

#include <stddef.h>

#include "ini.h"
#include "match.h"
#include "sources/source.h"

#define JOBBOT_VERSION "1.0.0"
/* Settings, database and log live together in one directory in the user's
 * home. "~/" is resolved with path_expand_home(). */
#define JOBBOT_DEFAULT_CONFIG "~/.jobbot/jobbot.conf"
#define JOBBOT_TOKEN_ENV "JOBBOT_TELEGRAM_TOKEN"

/* One enabled [source:NAME] section. */
typedef struct {
    char *name;              /* NAME: letters, digits, '-' and '_' */
    char *label;             /* shown in notifications, e.g. "DOU · UA" */
    const source_type *type;
    const ini_section *cfg;  /* points into settings.ini */
    int allow_empty;         /* an empty result is normal for this source */
    long interval_s;         /* seconds between polls */
} source_def;

typedef struct {
    ini_file ini;

    char *db_path;
    char *user_agent;
    long poll_interval_s;
    long dedupe_window_s;      /* 0 disables cross-board duplicate detection */
    long alert_after_failures; /* consecutive failed polls before a Telegram alert */
    int log_level;

    char *tg_api_base;
    char *tg_bot_token; /* NULL when not configured */
    char *tg_chat_id;   /* NULL when not configured */

    matcher match;

    source_def *sources;
    size_t nsources;
} settings;

/* Loads and validates the configuration file. When `need_telegram` is set
 * the bot token and chat id are required. Returns 0, or -1 with a message
 * in `err` that names the file and the offending setting. */
int settings_load(settings *s, const char *path, int need_telegram, char *err, size_t errlen);
void settings_free(settings *s);

/* Returns the source called `name`, or NULL. */
const source_def *settings_source(const settings *s, const char *name);

#endif
