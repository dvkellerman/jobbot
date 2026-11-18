#include "pipeline.h"

#include <stdlib.h>
#include <string.h>

#include "text.h"
#include "util.h"

/* Field size limits, in bytes. A Telegram message holds 4096 characters, so
 * the one-line fields are kept short enough to always fit together. */
#define MAX_TITLE 300
#define MAX_FIELD 200
#define MAX_TAGS 500
#define MAX_URL 2000
#define MAX_DESCRIPTION 16000

/* Cleans one single-line field; a field that ends up empty becomes NULL. */
static void clean_line(char **field, size_t max)
{
    if (!*field)
        return;
    text_sanitize(*field);
    text_squash(*field);
    text_truncate(*field, max);
    if (**field == '\0') {
        free(*field);
        *field = NULL;
    }
}

void job_clean(job *j)
{
    clean_line(&j->ext_id, MAX_URL);
    clean_line(&j->url, MAX_URL);
    clean_line(&j->title, MAX_TITLE);
    clean_line(&j->company, MAX_FIELD);
    clean_line(&j->location, MAX_FIELD);
    clean_line(&j->salary, MAX_FIELD);
    clean_line(&j->tags, MAX_TAGS);
    clean_line(&j->posted_at, MAX_FIELD);
    if (j->description) {
        text_sanitize(j->description);
        text_truncate(j->description, MAX_DESCRIPTION);
    }
}

/* Decides the status of a posting not seen before. Returns NULL on a
 * database error. `*fingerprint` receives a new string or NULL. */
static const char *classify(store *s, const matcher *m, const job *j, int seeded, long long now,
                            long long dedupe_window_s, char **fingerprint)
{
    *fingerprint = job_fingerprint(j);
    if (!seeded)
        return JOB_SEEDED;
    if (!match_job(m, j).matched)
        return JOB_UNMATCHED;
    if (*fingerprint && dedupe_window_s > 0) {
        int recent = store_fingerprint_recent(s, *fingerprint, now - dedupe_window_s);
        if (recent < 0)
            return NULL;
        if (recent)
            return JOB_DUPLICATE;
    }
    return JOB_PENDING;
}

int pipeline_ingest(store *s, const matcher *m, const char *source, job_list *jobs, long long now,
                    long long dedupe_window_s, ingest_stats *stats)
{
    memset(stats, 0, sizeof *stats);
    stats->fetched = jobs->n;

    source_state state;
    if (store_source_get(s, source, &state) != 0)
        return -1;
    int seeded = state.seeded;
    source_state_free(&state);

    if (store_begin(s) != 0)
        return -1;

    for (size_t i = 0; i < jobs->n; i++) {
        job *j = &jobs->items[i];
        job_clean(j);
        if (!job_is_complete(j))
            continue;

        int exists = store_job_exists(s, source, j->ext_id);
        if (exists < 0)
            goto fail;
        if (exists)
            continue;

        char *fingerprint = NULL;
        const char *status = classify(s, m, j, seeded, now, dedupe_window_s, &fingerprint);
        int added = status ? store_job_add(s, source, j, fingerprint, status, now) : -1;
        free(fingerprint);
        if (added < 0)
            goto fail;
        if (added == 0)
            continue;

        stats->fresh++;
        if (strcmp(status, JOB_SEEDED) == 0)
            stats->seeded++;
        else if (strcmp(status, JOB_UNMATCHED) == 0)
            stats->unmatched++;
        else if (strcmp(status, JOB_DUPLICATE) == 0)
            stats->duplicates++;
        else
            stats->queued++;
    }

    if (store_source_success(s, source, now) != 0 || store_commit(s) != 0)
        goto fail;
    return 0;

fail:
    store_rollback(s);
    return -1;
}
