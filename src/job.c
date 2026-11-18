#include "job.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

void job_list_init(job_list *l)
{
    l->items = NULL;
    l->n = 0;
    l->cap = 0;
}

job *job_list_add(job_list *l)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = xrealloc(l->items, l->cap * sizeof *l->items);
    }
    job *j = &l->items[l->n++];
    memset(j, 0, sizeof *j);
    return j;
}

void job_list_drop_last(job_list *l)
{
    if (l->n == 0)
        return;
    job_free(&l->items[--l->n]);
}

void job_free(job *j)
{
    free(j->ext_id);
    free(j->url);
    free(j->title);
    free(j->company);
    free(j->location);
    free(j->salary);
    free(j->tags);
    free(j->description);
    free(j->posted_at);
    memset(j, 0, sizeof *j);
}

void job_list_free(job_list *l)
{
    for (size_t i = 0; i < l->n; i++)
        job_free(&l->items[i]);
    free(l->items);
    job_list_init(l);
}

int job_is_complete(const job *j)
{
    return !str_blank(j->ext_id) && !str_blank(j->url) && !str_blank(j->title);
}
