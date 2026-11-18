#ifndef JOBBOT_SOURCE_PARSERS_H
#define JOBBOT_SOURCE_PARSERS_H

#include <stddef.h>

#include "cJSON.h"
#include "job.h"

/* Parsers for each board's response, separate from the fetching so they can
 * be tested against saved responses. Each appends complete postings to
 * `out` and returns -1 with a message in `err` when the response does not
 * have the expected shape. Postings whose id is already in `out` (from an
 * earlier page) are skipped. */

/* NoFluffJobs search API. The API lists one entry per location of a job;
 * entries sharing a `reference` become one posting, and references already
 * in `out` (from an earlier page) are skipped. `*total_pages` receives the
 * page count reported by the API. */
int nofluff_parse(const char *json, job_list *out, int *total_pages, char *err, size_t errlen);

/* JustJoin.it offers API. */
int justjoin_parse(const char *json, job_list *out, char *err, size_t errlen);

/* LinkedIn guest job-search fragment (a list of result cards). Unlike the
 * JSON parsers this returns the number of result cards on the page (0 past
 * the last page), or -1; see html_cards_parse(). */
int linkedin_parse(const char *html, size_t len, job_list *out, char *err, size_t errlen);

/* PNet search results page. `base_url` resolves relative links. Returns the
 * number of result cards on the page, or -1. */
int pnet_parse(const char *html, size_t len, const char *base_url, job_list *out, char *err,
               size_t errlen);

/* Adzuna search API. `currency` labels the pay figures (the API gives none);
 * may be NULL. */
int adzuna_parse(const char *json, const char *currency, job_list *out, char *err, size_t errlen);

/* ---- small JSON helpers shared by the parsers ---- */

/* The string value of `obj[key]`, or NULL when absent, not a string, or blank. */
static inline const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(v) || !v->valuestring)
        return NULL;
    for (const char *p = v->valuestring; *p; p++)
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
            return v->valuestring;
    return NULL;
}

/* Reads `obj[key]` as a number. Returns 1 and sets `*out`, or 0 when absent
 * or not a number. */
static inline int json_num(const cJSON *obj, const char *key, double *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(v))
        return 0;
    *out = v->valuedouble;
    return 1;
}

#endif
