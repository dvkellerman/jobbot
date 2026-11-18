#ifndef JOBBOT_MATCH_H
#define JOBBOT_MATCH_H

#include <stddef.h>

#include "job.h"

/* Keyword filter.
 *
 * A posting matches when its title (or, when enabled, its skill tags)
 * contains at least one include keyword and its title contains no exclude
 * keyword. Keywords are compared case-insensitively as whole words or
 * phrases, so "ios" matches "iOS/Android" but not "kiosk" or "scenarios". */

typedef struct {
    char **words; /* folded for comparison */
    size_t n;
} keyword_list;

typedef struct {
    keyword_list include;
    keyword_list exclude;
    int match_tags; /* also look for include keywords in the skill tags */
} matcher;

typedef struct {
    int matched;
    const char *included_by; /* include keyword that hit, or NULL */
    const char *excluded_by; /* exclude keyword that vetoed, or NULL */
} match_result;

/* Parses a comma-separated keyword list. Blank entries are skipped. */
void keywords_parse(keyword_list *k, const char *csv);
void keywords_free(keyword_list *k);
/* Returns the first keyword found in `folded_text`, or NULL. */
const char *keywords_find(const keyword_list *k, const char *folded_text);

void matcher_free(matcher *m);
match_result match_job(const matcher *m, const job *j);

/* Returns a new key that is equal for postings with the same company and
 * title regardless of case, punctuation and spacing, or NULL when the
 * company is unknown (a title alone is too weak to call two postings the
 * same). */
char *job_fingerprint(const job *j);

#endif
