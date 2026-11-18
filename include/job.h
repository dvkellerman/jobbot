#ifndef JOBBOT_JOB_H
#define JOBBOT_JOB_H

#include <stddef.h>

/* One job posting as read from a board. All strings are owned, UTF-8, plain
 * text (no markup). Only `ext_id`, `url` and `title` are required; the rest
 * may be NULL. */
typedef struct {
    char *ext_id;      /* identifier that is stable and unique within the source */
    char *url;         /* page a person opens to read and apply */
    char *title;
    char *company;
    char *location;
    char *salary;
    char *tags;        /* comma-separated skills or categories */
    char *description;
    char *posted_at;   /* as given by the board */
} job;

typedef struct {
    job *items;
    size_t n;
    size_t cap;
} job_list;

void job_list_init(job_list *l);
/* Appends a zeroed job and returns it. The pointer is valid until the next add. */
job *job_list_add(job_list *l);
/* Removes the most recently added job (used to discard an incomplete one). */
void job_list_drop_last(job_list *l);
void job_list_free(job_list *l);
void job_free(job *j);

/* True when the required fields are present and non-blank. */
int job_is_complete(const job *j);

#endif
