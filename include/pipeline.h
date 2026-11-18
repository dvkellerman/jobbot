#ifndef JOBBOT_PIPELINE_H
#define JOBBOT_PIPELINE_H

#include <stddef.h>

#include "job.h"
#include "match.h"
#include "store.h"

typedef struct {
    size_t fetched;    /* postings the source returned */
    size_t fresh;      /* of those, not stored before */
    size_t seeded;     /* fresh, recorded silently on the source's first poll */
    size_t unmatched;  /* fresh, rejected by the keyword filter */
    size_t duplicates; /* fresh, same company and title as a recent posting */
    size_t queued;     /* fresh, queued for notification */
} ingest_stats;

/* Cleans a posting's text in place so it is safe to store and send: invalid
 * UTF-8 and control characters are replaced, one-line fields are flattened,
 * and over-long fields are shortened. */
void job_clean(job *j);

/* Records the result of one successful poll of `source`, atomically:
 * stores every posting not seen before, decides what happens to each (see
 * the status values in store.h), and marks the poll as a success.
 *
 * The first successful poll of a source only records what is already on
 * the board, so a new source does not flood the chat with its backlog.
 *
 * `dedupe_window_s` is how far back a posting with the same company and
 * title counts as a duplicate; 0 disables that check.
 *
 * Returns 0 on success, -1 on a database error (nothing is stored). */
int pipeline_ingest(store *s, const matcher *m, const char *source, job_list *jobs, long long now,
                    long long dedupe_window_s, ingest_stats *stats);

#endif
