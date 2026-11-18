/* LinkedIn public ("guest") job search.
 *
 * This reads the same result fragments LinkedIn serves to visitors who are
 * not signed in. LinkedIn's terms of use forbid automated access, so the
 * source is off unless it is enabled in the config, and it keeps requests
 * few and slow. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sources/htmlcards.h"
#include "sources/parsers.h"
#include "sources/source.h"
#include "util.h"

#define LINKEDIN_PAGE_SIZE 10
#define LINKEDIN_PAGE_PAUSE_S 3

static const card_spec LINKEDIN_CARDS = {
    .item = "//*[@data-entity-urn][" XP_CLASS("base-card") "]",
    .id = "string(@data-entity-urn)", /* urn:li:jobPosting:4480470699 */
    .id_after = ':',
    .url = "string((.//a[" XP_CLASS("base-card__full-link") "]/@href | self::a/@href)[1])",
    .url_strip_query = 1,
    .title = "string(.//*[" XP_CLASS("base-search-card__title") "][1])",
    .company = "string(.//*[" XP_CLASS("base-search-card__subtitle") "][1])",
    .location = "string(.//*[" XP_CLASS("job-search-card__location") "][1])",
    .posted = "string(.//time[1]/@datetime)",
};

int linkedin_parse(const char *html, size_t len, job_list *out, char *err, size_t errlen)
{
    return html_cards_parse(html, len, "https://www.linkedin.com/", &LINKEDIN_CARDS, out, err,
                            errlen);
}

/* ---- source type ------------------------------------------------------ */

static const char *const LINKEDIN_KEYS[] = {"keywords", "location", "posted_within_days", "pages",
                                            NULL};

static int linkedin_validate(const ini_section *cfg, char *err, size_t errlen)
{
    if (*ini_get(cfg, "keywords", "") == '\0') {
        snprintf(err, errlen, "'keywords' must be set (e.g. ios developer)");
        return -1;
    }
    if (*ini_get(cfg, "location", "") == '\0') {
        snprintf(err, errlen, "'location' must be set (e.g. Poland)");
        return -1;
    }
    long v;
    if (ini_get_long(cfg, "posted_within_days", 7, 1, 30, &v, err, errlen) != 0)
        return -1;
    return ini_get_long(cfg, "pages", 2, 1, 10, &v, err, errlen);
}

static int linkedin_fetch(const source_ctx *ctx, source_result *out)
{
    long days = 7, pages = 2;
    char ignored[64];
    ini_get_long(ctx->cfg, "posted_within_days", 7, 1, 30, &days, ignored, sizeof ignored);
    ini_get_long(ctx->cfg, "pages", 2, 1, 10, &pages, ignored, sizeof ignored);

    for (long page = 0; page < pages; page++) {
        if (page > 0 && sleep_seconds(LINKEDIN_PAGE_PAUSE_S) != 0) {
            snprintf(out->error, sizeof out->error, "interrupted");
            return -1;
        }
        /* sortBy=DD lists the most recent postings first. */
        buf url;
        buf_init(&url);
        buf_puts(&url, "https://www.linkedin.com/jobs-guest/jobs/api/seeMoreJobPostings/search"
                       "?keywords=");
        http_url_encode(&url, ini_get(ctx->cfg, "keywords", ""));
        buf_puts(&url, "&location=");
        http_url_encode(&url, ini_get(ctx->cfg, "location", ""));
        buf_printf(&url, "&f_TPR=r%ld&sortBy=DD&start=%ld", days * 86400L,
                   page * LINKEDIN_PAGE_SIZE);

        http_request req = {0};
        req.url = url.data;
        req.retries = 1;

        http_response resp;
        int rc = source_http(ctx, &req, &resp, out->error, sizeof out->error);
        buf_free(&url);
        if (rc != 0)
            return -1;
        int cards = linkedin_parse(resp.body.data ? resp.body.data : "", resp.body.len, &out->jobs,
                                   out->error, sizeof out->error);
        http_response_free(&resp);
        if (cards < 0)
            return -1;
        if (cards == 0) /* past the last page of results */
            break;
    }
    return 0;
}

const source_type source_type_linkedin = {
    .type = "linkedin",
    .keys = LINKEDIN_KEYS,
    .validate = linkedin_validate,
    .fetch = linkedin_fetch,
};
