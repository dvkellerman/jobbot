/* PNet (pnet.co.za) search results pages. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sources/htmlcards.h"
#include "sources/parsers.h"
#include "sources/source.h"
#include "util.h"

/* A full results page; a shorter page is the last one. */
#define PNET_PAGE_SIZE 25
#define PNET_PAGE_PAUSE_S 2

static const card_spec PNET_CARDS = {
    .item = "//article[@data-at='job-item']",
    .id = "string(@id)", /* job-item-4308558 */
    .id_after = '-',
    .url = "string(.//a[@data-at='job-item-title'][1]/@href)",
    .title = "string(.//a[@data-at='job-item-title'][1])",
    .company = "string(.//*[@data-at='job-item-company-name'][1])",
    .location = "string(.//*[@data-at='job-item-location'][1])",
    .posted = "string(.//*[@data-at='job-item-timeago'][1])",
    .summary = "string(.//*[@data-at='jobcard-content'][1])",
};

int pnet_parse(const char *html, size_t len, const char *base_url, job_list *out, char *err,
               size_t errlen)
{
    return html_cards_parse(html, len, base_url, &PNET_CARDS, out, err, errlen);
}

/* ---- source type ------------------------------------------------------ */

static const char *const PNET_KEYS[] = {"url", "pages", NULL};

static int pnet_validate(const ini_section *cfg, char *err, size_t errlen)
{
    const char *url = ini_get(cfg, "url", "");
    if (strncmp(url, "https://www.pnet.co.za/", 23) != 0) {
        snprintf(err, errlen, "'url' must be a search page address starting with "
                              "https://www.pnet.co.za/");
        return -1;
    }
    if (strchr(url, '#') || strstr(url, "page=")) {
        snprintf(err, errlen, "'url' must not contain '#' or a page number; use 'pages'");
        return -1;
    }
    long pages;
    return ini_get_long(cfg, "pages", 3, 1, 10, &pages, err, errlen);
}

static int pnet_fetch(const source_ctx *ctx, source_result *out)
{
    long pages = 3;
    char ignored[64];
    ini_get_long(ctx->cfg, "pages", 3, 1, 10, &pages, ignored, sizeof ignored);
    const char *base = ini_get(ctx->cfg, "url", "");

    for (long page = 1; page <= pages; page++) {
        if (page > 1 && sleep_seconds(PNET_PAGE_PAUSE_S) != 0) {
            snprintf(out->error, sizeof out->error, "interrupted");
            return -1;
        }
        buf url;
        buf_init(&url);
        buf_puts(&url, base);
        if (page > 1)
            buf_printf(&url, "%cpage=%ld", strchr(base, '?') ? '&' : '?', page);

        http_request req = {0};
        req.url = url.data;
        req.retries = 1;

        http_response resp;
        int rc = source_http(ctx, &req, &resp, out->error, sizeof out->error);
        int cards = -1;
        if (rc == 0) {
            cards = pnet_parse(resp.body.data ? resp.body.data : "", resp.body.len, url.data,
                               &out->jobs, out->error, sizeof out->error);
            http_response_free(&resp);
        }
        buf_free(&url);
        if (cards < 0)
            return -1;
        if (cards < PNET_PAGE_SIZE)
            break;
    }
    return 0;
}

const source_type source_type_pnet = {
    .type = "pnet",
    .keys = PNET_KEYS,
    .validate = pnet_validate,
    .fetch = pnet_fetch,
};
