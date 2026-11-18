#ifndef JOBBOT_HTMLCARDS_H
#define JOBBOT_HTMLCARDS_H

#include <stddef.h>

#include "job.h"

/* Reads job postings out of an HTML results page that shows one "card" per
 * posting. A board is described by XPath expressions: `item` selects the
 * card elements, and the others are evaluated relative to a card and
 * converted to strings. Expressions other than `item`, `id`, `url` and
 * `title` may be NULL. */
typedef struct {
    const char *item;
    const char *id;
    char id_after;       /* keep only what follows the last occurrence of this
                            character in the id ("job-item-42" with '-' gives
                            "42"); 0 keeps the whole value */
    const char *url;
    int url_strip_query; /* drop the query string, e.g. tracking parameters */
    const char *title;
    const char *company;
    const char *location;
    const char *posted;
    const char *summary;
} card_spec;

/* Appends the complete postings found in `html` to `out`, skipping ids that
 * are already in `out`. Relative links are resolved against `base_url`
 * (may be NULL). Returns the number of cards on the page (0 for a page
 * without results), or -1 with a message in `err` when cards are present but
 * none could be read, which means the page layout has changed. */
int html_cards_parse(const char *html, size_t len, const char *base_url, const card_spec *spec,
                     job_list *out, char *err, size_t errlen);

/* XPath test for an element carrying the CSS class `cls`. */
#define XP_CLASS(cls) "contains(concat(' ', normalize-space(@class), ' '), ' " cls " ')"

#endif
