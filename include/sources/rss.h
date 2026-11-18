#ifndef JOBBOT_SOURCE_RSS_H
#define JOBBOT_SOURCE_RSS_H

#include <stddef.h>

#include "job.h"

/* How to read a feed's items.
 *
 *   generic  title and link as given; the guid (or link) identifies the item
 *   dou      jobs.dou.ua: the title is "Role в Company, [salary,] places"
 *            and the guid changes when a vacancy is edited, so the link
 *            (without its query string) identifies the item
 *   djinni   djinni.co: the number at the start of the link's last path
 *            segment identifies the item, so a re-titled job stays the same
 */
typedef enum { RSS_GENERIC, RSS_DOU, RSS_DJINNI } rss_flavor;

/* Returns 0 and sets `*flavor`, or -1 when `name` is not a known flavor. */
int rss_flavor_parse(const char *name, rss_flavor *flavor);

/* Parses an RSS 2.0 document and appends its complete items to `out`.
 * Returns 0 on success (even when there are no items), or -1 with a message
 * in `err` when the document is not RSS. */
int rss_parse(const char *xml, size_t len, rss_flavor flavor, job_list *out, char *err,
              size_t errlen);

#endif
