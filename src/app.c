#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pipeline.h"
#include "util.h"

/* Postings sent per delivery round; the rest wait for the next round. */
#define DELIVERY_BATCH 100
/* Pause between messages, to stay under Telegram's per-chat rate limit. */
#define DELIVERY_PAUSE_S 1
/* Postings shown in the --test-telegram message. */
#define TEST_POSTINGS 4

/* Longest wait between retries of a failing source. */
#define MAX_SOURCE_BACKOFF_S (6 * 3600L)

static const char EMPTY_RESULT_ERROR[] =
    "the board returned no postings (its page or API may have changed)";

/* Sends a plain-text service message, or prints it in a dry run. */
static tg_result send_notice(app *a, const char *text)
{
    if (a->dry_run) {
        printf("[dry-run] notice:\n%s\n\n", text);
        tg_result ok = {TG_OK, -1, ""};
        return ok;
    }
    return telegram_send(a->http, &a->tg, text, 0, NULL);
}

static void format_time(long long t, char *out, size_t outlen)
{
    if (t <= 0) {
        snprintf(out, outlen, "never");
        return;
    }
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    strftime(out, outlen, "%Y-%m-%d %H:%M UTC", &tm);
}

/* Reports a source that keeps failing, once per outage. */
static void alert_failure(app *a, const source_def *src, int failures, const char *error)
{
    source_state st;
    if (store_source_get(a->db, src->name, &st) != 0)
        return;
    if (!st.alerted) {
        char when[64];
        format_time(st.last_success, when, sizeof when);
        buf text;
        buf_init(&text);
        buf_printf(&text,
                   "jobbot: source %s (%s) has failed %d times in a row, so new postings from it "
                   "are not being seen.\n"
                   "Last error: %s\n"
                   "Last successful check: %s\n"
                   "It will keep being retried; you will get a message when it works again.",
                   src->label, src->name, failures, error, when);
        tg_result r = send_notice(a, text.data);
        buf_free(&text);
        if (r.status == TG_OK) {
            if (!a->dry_run)
                store_source_set_alerted(a->db, src->name, 1);
        } else {
            log_warn("source %s: could not send failure alert: %s", src->name, r.error);
        }
    }
    source_state_free(&st);
}

static void alert_recovery(app *a, const source_def *src)
{
    buf text;
    buf_init(&text);
    buf_printf(&text, "jobbot: source %s (%s) is working again.", src->label, src->name);
    tg_result r = send_notice(a, text.data);
    buf_free(&text);
    if (r.status == TG_OK) {
        if (!a->dry_run)
            store_source_set_alerted(a->db, src->name, 0);
    } else {
        log_warn("source %s: could not send recovery notice: %s", src->name, r.error);
    }
}

/* Runs the source's fetch and applies the "empty result is a failure" rule.
 * Returns 0 on success, -1 with `res->error` set. */
static int fetch_source(app *a, const source_def *src, source_result *res)
{
    source_ctx ctx = {src->name, src->cfg, a->http};
    int rc = src->type->fetch(&ctx, res);
    if (rc != 0 && res->error[0] == '\0')
        snprintf(res->error, sizeof res->error, "unknown error");
    if (rc == 0 && res->jobs.n == 0 && !src->allow_empty) {
        snprintf(res->error, sizeof res->error, "%s", EMPTY_RESULT_ERROR);
        rc = -1;
    }
    return rc;
}

long app_source_delay(const source_def *src, int failures)
{
    long delay = src->interval_s;
    for (int i = 1; i < failures && delay < MAX_SOURCE_BACKOFF_S; i++)
        delay *= 2;
    if (failures > 0 && delay > MAX_SOURCE_BACKOFF_S)
        delay = MAX_SOURCE_BACKOFF_S;
    if (delay < src->interval_s)
        delay = src->interval_s;
    return delay;
}

int app_source_is_due(const source_def *src, const source_state *st, long long now)
{
    /* Never checked, or the clock has been set back since. */
    if (st->last_attempt <= 0 || now < st->last_attempt)
        return 1;
    long delay = app_source_delay(src, st->failures);
    return now - st->last_attempt >= delay - delay / 10;
}

int app_poll_source(app *a, const source_def *src)
{
    /* The check is recorded as made when it started, so a slow board does
     * not push its own next check back. */
    long long now = (long long)time(NULL);
    source_result res;
    source_result_init(&res);
    int rc = fetch_source(a, src, &res);

    if (rc != 0) {
        /* A fetch cut short by shutdown is not the board's fault. */
        if (stop_requested()) {
            source_result_free(&res);
            return 1;
        }
        int failures = 0;
        if (store_source_failure(a->db, src->name, res.error, now, &failures) != 0) {
            source_result_free(&res);
            return -1;
        }
        log_warn("source %s: check failed (%d in a row): %s", src->name, failures, res.error);
        if (failures >= a->cfg.alert_after_failures)
            alert_failure(a, src, failures, res.error);
        source_result_free(&res);
        return failures;
    }

    source_state before;
    if (store_source_get(a->db, src->name, &before) != 0) {
        source_result_free(&res);
        return -1;
    }
    int was_alerted = before.alerted;
    int first_poll = !before.seeded;
    source_state_free(&before);

    ingest_stats st;
    rc = pipeline_ingest(a->db, &a->cfg.match, src->name, &res.jobs, now, a->cfg.dedupe_window_s,
                         &st);
    source_result_free(&res);
    if (rc != 0) {
        log_error("source %s: could not record the result", src->name);
        return -1;
    }

    if (first_poll)
        log_info("source %s: first check, recorded %zu existing postings without notifying",
                 src->name, st.seeded);
    else if (st.fresh)
        log_info("source %s: %zu new (%zu queued, %zu filtered out, %zu duplicates) of %zu listed",
                 src->name, st.fresh, st.queued, st.unmatched, st.duplicates, st.fetched);
    else
        log_debug("source %s: nothing new among %zu listed", src->name, st.fetched);

    if (was_alerted)
        alert_recovery(a, src);
    return 0;
}

static const char *label_for(const app *a, const char *source)
{
    const source_def *def = settings_source(&a->cfg, source);
    return def ? def->label : source; /* the source may since have been removed from the config */
}

/* Telegram refused both forms of a posting's message. That is either this
 * posting's content or a chat that cannot be written to at all; a fixed
 * notice tells the two apart. Returns 0 when the posting was set aside and
 * delivery can continue, -1 when nothing can be sent. */
static int handle_rejected(app *a, const queued_job *q, const tg_result *rejection)
{
    buf text;
    buf_init(&text);
    buf_printf(&text,
               "jobbot: a matching posting from %s could not be shown because Telegram rejected "
               "the message. It is job id %lld in the database.",
               label_for(a, q->source), q->id);
    tg_result probe = telegram_send(a->http, &a->tg, text.data, 0, NULL);
    buf_free(&text);

    if (probe.status != TG_OK) {
        log_error("telegram: cannot send to chat %s: %s", a->tg.chat_id, rejection->error);
        return -1;
    }
    log_error("job %lld from %s set aside as undeliverable: %s", q->id, q->source,
              rejection->error);
    return store_job_mark_undeliverable(a->db, q->id);
}

int app_deliver(app *a)
{
    queued_job *queue = NULL;
    size_t n = 0;
    if (store_queue_load(a->db, DELIVERY_BATCH, &queue, &n) != 0)
        return 1;

    int blocked = 0;
    size_t sent = 0;
    for (size_t i = 0; i < n && !blocked; i++) {
        const queued_job *q = &queue[i];
        const char *label = label_for(a, q->source);

        if (a->dry_run) {
            char *text = telegram_format_job_plain(&q->j, label);
            printf("[dry-run] would send:\n%s\n\n", text);
            free(text);
            continue;
        }
        if (stop_requested() || (sent > 0 && sleep_seconds(DELIVERY_PAUSE_S) != 0)) {
            blocked = 1;
            break;
        }

        tg_result r = telegram_send_job(a->http, &a->tg, &q->j, label);
        switch (r.status) {
        case TG_OK:
            /* If this cannot be recorded, stop: carrying on would send the
             * same posting again on every round. */
            if (store_job_mark_notified(a->db, q->id, (long long)time(NULL)) != 0)
                blocked = 1;
            else
                log_info("notified: %s [%s]", q->j.title, q->source);
            sent++;
            break;
        case TG_REJECTED:
            if (handle_rejected(a, q, &r) != 0)
                blocked = 1;
            sent++;
            break;
        case TG_RETRY:
            log_warn("telegram: delivery paused, will retry: %s", r.error);
            blocked = 1;
            break;
        case TG_FATAL:
            log_error("telegram: %s. Check bot_token and chat_id, and that you have pressed Start "
                      "in a chat with the bot.",
                      r.error);
            blocked = 1;
            break;
        }
    }
    /* A full batch means there may be more waiting. */
    if (!blocked && !a->dry_run && n == DELIVERY_BATCH)
        blocked = 1;

    store_queue_free(queue, n);
    return blocked;
}

static int passes_filter(const job *j, void *ctx)
{
    const matcher *m = ctx;
    return match_job(m, j).matched;
}

int app_test_telegram(app *a)
{
    queued_job *found = NULL;
    size_t n = 0;
    /* The filter is applied afresh instead of going by the recorded status,
     * so postings recorded on a source's first check, which were never
     * sent, can serve as examples too. */
    if (a->db &&
        store_jobs_latest(a->db, passes_filter, &a->cfg.match, TEST_POSTINGS, &found, &n) != 0)
        return 1; /* the store has logged why */

    tg_sample samples[TEST_POSTINGS];
    for (size_t i = 0; i < n; i++) {
        samples[i].j = &found[i].j;
        samples[i].source_label = label_for(a, found[i].source);
    }

    int rc = 0;
    if (a->dry_run) {
        char *text = telegram_format_test(samples, n, 0);
        printf("[dry-run] would send:\n%s\n\n", text);
        free(text);
    } else {
        tg_result r = telegram_send_test(a->http, &a->tg, samples, n);
        switch (r.status) {
        case TG_OK:
            printf("Test message sent to chat %s.%s\n", a->tg.chat_id,
                   n ? "" : " The database has no matching postings yet, so it shows none.");
            break;
        case TG_RETRY:
            fprintf(stderr, "jobbot: test message not sent: %s\n", r.error);
            rc = 1;
            break;
        case TG_REJECTED:
        case TG_FATAL:
            fprintf(stderr,
                    "jobbot: Telegram refused the test message: %s\n"
                    "Check bot_token and chat_id, and that you have pressed Start in a chat with "
                    "the bot.\n",
                    r.error);
            rc = 2;
            break;
        }
    }

    store_queue_free(found, n);
    return rc;
}

tg_result app_send_startup(app *a)
{
    buf text;
    buf_init(&text);
    buf_printf(&text,
               "jobbot started. Watching %zu source%s; new matching postings will appear here.",
               a->cfg.nsources, a->cfg.nsources == 1 ? "" : "s");
    tg_result r = send_notice(a, text.data);
    buf_free(&text);
    return r;
}

int app_check_sources(app *a)
{
    int failed = 0;
    for (size_t i = 0; i < a->cfg.nsources && !stop_requested(); i++) {
        const source_def *src = &a->cfg.sources[i];
        source_result res;
        source_result_init(&res);

        if (fetch_source(a, src, &res) != 0) {
            printf("%-14s FAILED  %s\n", src->name, res.error);
            failed++;
            source_result_free(&res);
            continue;
        }

        size_t matched = 0, shown_yes = 0, shown_no = 0;
        buf detail;
        buf_init(&detail);
        for (size_t k = 0; k < res.jobs.n; k++) {
            job *j = &res.jobs.items[k];
            job_clean(j);
            if (!job_is_complete(j))
                continue;
            match_result m = match_job(&a->cfg.match, j);
            if (m.matched) {
                matched++;
                if (shown_yes++ < 5)
                    buf_printf(&detail, "    + %s%s%s\n", j->title, j->company ? " | " : "",
                               j->company ? j->company : "");
            } else if (shown_no++ < 3) {
                if (m.excluded_by)
                    buf_printf(&detail, "    - %s  (excluded by \"%s\")\n", j->title, m.excluded_by);
                else
                    buf_printf(&detail, "    - %s  (no include keyword)\n", j->title);
            }
        }
        printf("%-14s OK      %zu postings, %zu match the filter\n%s", src->name, res.jobs.n,
               matched, detail.data ? detail.data : "");
        buf_free(&detail);
        source_result_free(&res);
    }
    return failed;
}
