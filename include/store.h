#ifndef JOBBOT_STORE_H
#define JOBBOT_STORE_H

#include <stddef.h>

#include "job.h"

/* SQLite-backed state: every posting ever seen (for de-duplication and the
 * notification queue) and per-source health.
 *
 * Job status values:
 *   seeded     seen on a source's first successful poll; never notified
 *   unmatched  did not pass the keyword filter
 *   duplicate  same company and title as a recently queued/notified posting
 *   pending    matched, waiting to be sent to Telegram
 *   notified   sent to Telegram
 *   undeliverable  Telegram refused the message itself; not retried
 */
#define JOB_SEEDED "seeded"
#define JOB_UNMATCHED "unmatched"
#define JOB_DUPLICATE "duplicate"
#define JOB_PENDING "pending"
#define JOB_NOTIFIED "notified"
#define JOB_UNDELIVERABLE "undeliverable"

typedef struct store store;

typedef struct {
    int seeded;           /* 1 once the source has had a successful poll */
    long long last_success; /* unix time of the last successful poll, 0 if never */
    long long last_attempt; /* unix time of the last poll, successful or not, 0 if never */
    int failures;         /* consecutive failed polls */
    int alerted;          /* 1 while a failure alert is outstanding */
    char *last_error;     /* text of the most recent failure, or NULL */
} source_state;

typedef struct {
    long long id;
    char *source;
    job j;
} queued_job;

/* All functions return 0 on success and -1 on a database error (logged),
 * unless stated otherwise. */

int store_open(store **out, const char *path, char *err, size_t errlen);
/* Opens an existing database for reading only: nothing is created, upgraded
 * or written, so this is safe next to a jobbot that is using the database.
 * Returns 0 with the store in `*out`; 1 with `*out` NULL when there is no
 * database at `path` yet (or it is still empty); -1 with a message in `err`. */
int store_open_readonly(store **out, const char *path, char *err, size_t errlen);
void store_close(store *s);

int store_begin(store *s);
int store_commit(store *s);
void store_rollback(store *s);

/* Loads a source's state; a source never seen before yields a zeroed state. */
int store_source_get(store *s, const char *name, source_state *out);
void source_state_free(source_state *st);
/* Records a successful poll at time `now`: marks the source seeded and
 * clears the failure count and last error. */
int store_source_success(store *s, const char *name, long long now);
/* Records a failed poll at time `now` and returns the new
 * consecutive-failure count in `*failures`. */
int store_source_failure(store *s, const char *name, const char *error, long long now,
                         int *failures);
int store_source_set_alerted(store *s, const char *name, int alerted);

/* Returns 1 when (source, ext_id) is already stored, 0 when not, -1 on error. */
int store_job_exists(store *s, const char *source, const char *ext_id);
/* Inserts a posting. Returns 1 when inserted, 0 when (source, ext_id) was
 * already present, -1 on error. */
int store_job_add(store *s, const char *source, const job *j, const char *fingerprint,
                  const char *status, long long now);
/* Returns 1 when a pending or notified posting with this fingerprint was
 * first seen at or after `since`, 0 when not, -1 on error. */
int store_fingerprint_recent(store *s, const char *fingerprint, long long since);

/* Loads up to `limit` pending postings, oldest first. Free with
 * store_queue_free(). */
int store_queue_load(store *s, size_t limit, queued_job **out, size_t *n);
void store_queue_free(queued_job *q, size_t n);
int store_job_mark_notified(store *s, long long id, long long now);
/* Takes a posting out of the queue without it having been sent. */
int store_job_mark_undeliverable(store *s, long long id);

/* Says whether store_jobs_latest() should return a posting. */
typedef int (*job_filter)(const job *j, void *ctx);

/* Loads the most recently recorded postings for which `accept` returns
 * non-zero, newest first, up to `limit`. Every status is considered except
 * duplicates of another posting and postings Telegram refused. The
 * description is not loaded. Free with store_queue_free(). */
int store_jobs_latest(store *s, job_filter accept, void *ctx, size_t limit, queued_job **out,
                      size_t *n);

/* Counts postings with the given status; returns -1 on error. */
long long store_job_count(store *s, const char *status);

#endif
