#ifndef JOBBOT_APP_H
#define JOBBOT_APP_H

#include "config.h"
#include "http.h"
#include "store.h"
#include "telegram.h"

typedef struct {
    settings cfg;
    store *db;        /* NULL in --check-sources mode, and in --test-telegram mode
                         when there is no database yet */
    http_client *http;
    telegram_cfg tg;
    int dry_run;      /* print messages instead of sending them; leave the queue as it is */
} app;

/* Polls one source and records the outcome: new postings are stored and
 * matching ones queued; failures are counted and, once there are enough in
 * a row, reported to the chat (and again when the source recovers).
 *
 * Returns 0 on success, the number of consecutive failures (1 or more) when
 * the poll failed, or -1 when the result could not be recorded. */
int app_poll_source(app *a, const source_def *src);

/* Seconds to leave between two checks of a source: its interval, doubling
 * with each consecutive failure after the first, up to six hours. */
long app_source_delay(const source_def *src, int failures);

/* Whether a source with the recorded state `st` should be checked at time
 * `now`: it has never been checked, or its delay has passed. A check that
 * comes up to a tenth of the delay early still counts, because a scheduler
 * that runs jobbot "every 15 minutes" does so only approximately. */
int app_source_is_due(const source_def *src, const source_state *st, long long now);

/* Sends every queued posting, oldest first. Returns 0 when the queue was
 * worked through, 1 when delivery had to stop and should be retried later. */
int app_deliver(app *a);

/* Tells the chat the bot has started, which also proves the token and chat
 * id work. */
tg_result app_send_startup(app *a);

/* --test-telegram: sends one message, marked as a test, that shows the
 * newest postings in the database that pass the keyword filter, formatted as
 * real notifications are. The database is only read (`a->db` is opened read
 * only, or is NULL), so nothing is queued, marked as sent or sent again
 * later. Prints the outcome and returns the exit status: 0 when the message
 * was sent, 1 on a failure that may pass, 2 when Telegram refused it. */
int app_test_telegram(app *a);

/* --check-sources: fetches every enabled source once and prints what came
 * back and how it would be filtered, without touching the database or
 * Telegram. Returns the number of sources that failed. */
int app_check_sources(app *a);

#endif
