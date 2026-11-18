/* NoFluffJobs (nofluffjobs.com) search API. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "sources/parsers.h"
#include "sources/source.h"
#include "util.h"

#define NOFLUFF_PAGE_SIZE 20
/* Pause between result pages, to go easy on the board. */
#define NOFLUFF_PAGE_PAUSE_S 1

static int has_ext_id(const job_list *l, const char *ext_id)
{
    for (size_t i = 0; i < l->n; i++)
        if (l->items[i].ext_id && strcmp(l->items[i].ext_id, ext_id) == 0)
            return 1;
    return 0;
}

static char *format_date_ms(double epoch_ms)
{
    if (epoch_ms <= 0 || epoch_ms > 4e12)
        return NULL;
    time_t t = (time_t)(epoch_ms / 1000.0);
    struct tm tm;
    gmtime_r(&t, &tm);
    char out[32];
    strftime(out, sizeof out, "%Y-%m-%d", &tm);
    return xstrdup(out);
}

static char *posting_location(const cJSON *posting)
{
    place_list places;
    place_list_init(&places);
    const cJSON *list =
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(posting, "location"), "places");
    const cJSON *place;
    cJSON_ArrayForEach(place, list)
        place_list_add(&places, json_str(place, "city")); /* province-only entries have no city */
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(posting, "fullyRemote")))
        place_list_add(&places, "Remote");
    return place_list_finish(&places);
}

static char *posting_salary(const cJSON *posting)
{
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(posting, "salary");
    double from = 0, to = 0;
    int has_from = json_num(s, "from", &from);
    int has_to = json_num(s, "to", &to);
    return source_format_salary(has_from, from, has_to, to, json_str(s, "currency"),
                                json_str(s, "period"), json_str(s, "type"));
}

/* The required skills shown on the listing, comma-separated. */
static char *posting_tags(const cJSON *posting)
{
    buf out;
    buf_init(&out);
    const cJSON *tiles =
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(posting, "tiles"), "values");
    const cJSON *tile;
    cJSON_ArrayForEach(tile, tiles)
    {
        const char *type = json_str(tile, "type");
        const char *value = json_str(tile, "value");
        if (!value || !type || strcmp(type, "requirement") != 0)
            continue;
        if (out.len)
            buf_puts(&out, ", ");
        buf_puts(&out, value);
    }
    if (!out.len) {
        buf_free(&out);
        return NULL;
    }
    return buf_steal(&out);
}

int nofluff_parse(const char *json, job_list *out, int *total_pages, char *err, size_t errlen)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *postings = cJSON_GetObjectItemCaseSensitive(root, "postings");
    if (!cJSON_IsArray(postings)) {
        snprintf(err, errlen, "unexpected response: no \"postings\" list");
        cJSON_Delete(root);
        return -1;
    }
    double pages = 1;
    json_num(root, "totalPages", &pages);
    *total_pages = pages >= 1 && pages < 100000 ? (int)pages : 1;

    const cJSON *p;
    cJSON_ArrayForEach(p, postings)
    {
        const char *reference = json_str(p, "reference");
        const char *title = json_str(p, "title");
        const char *slug = json_str(p, "url");
        if (!reference || !title || !slug || has_ext_id(out, reference))
            continue;

        job *j = job_list_add(out);
        j->ext_id = xstrdup(reference);
        j->title = xstrdup(title);
        buf url;
        buf_init(&url);
        buf_printf(&url, "https://nofluffjobs.com/job/%s", slug);
        j->url = buf_steal(&url);
        str_set(&j->company, json_str(p, "name"));
        j->location = posting_location(p);
        j->salary = posting_salary(p);
        j->tags = posting_tags(p);
        double posted;
        if (json_num(p, "posted", &posted))
            j->posted_at = format_date_ms(posted);
    }
    cJSON_Delete(root);
    return 0;
}

/* ---- source type ------------------------------------------------------ */

static const char *const NOFLUFF_KEYS[] = {"query", "region", "max_pages", NULL};

static int nofluff_validate(const ini_section *cfg, char *err, size_t errlen)
{
    if (*ini_get(cfg, "query", "") == '\0') {
        snprintf(err, errlen, "'query' must be set (e.g. ios, or requirement='React native')");
        return -1;
    }
    const char *region = ini_get(cfg, "region", "pl");
    if (strlen(region) != 2 || region[0] < 'a' || region[0] > 'z' || region[1] < 'a' ||
        region[1] > 'z') {
        snprintf(err, errlen, "'region' must be a two-letter code such as pl, got '%s'", region);
        return -1;
    }
    long pages;
    return ini_get_long(cfg, "max_pages", 5, 1, 20, &pages, err, errlen);
}

static int nofluff_fetch(const source_ctx *ctx, source_result *out)
{
    long max_pages = 5;
    char ignored[64];
    ini_get_long(ctx->cfg, "max_pages", 5, 1, 20, &max_pages, ignored, sizeof ignored);

    cJSON *body_json = cJSON_CreateObject();
    char *body = NULL;
    if (body_json && cJSON_AddStringToObject(body_json, "rawSearch", ini_get(ctx->cfg, "query", "")) &&
        cJSON_AddNumberToObject(body_json, "pageSize", NOFLUFF_PAGE_SIZE))
        body = cJSON_PrintUnformatted(body_json);
    cJSON_Delete(body_json);
    if (!body) {
        snprintf(out->error, sizeof out->error, "could not build the search request");
        return -1;
    }

    static const char *const headers[] = {"Content-Type: application/infiniteSearch+json",
                                          "Accept: application/json", NULL};
    int rc = 0;
    int total_pages = 1;
    for (int page = 1; page <= max_pages && page <= total_pages; page++) {
        if (page > 1 && sleep_seconds(NOFLUFF_PAGE_PAUSE_S) != 0) {
            snprintf(out->error, sizeof out->error, "interrupted");
            rc = -1;
            break;
        }
        buf url;
        buf_init(&url);
        buf_printf(&url,
                   "https://nofluffjobs.com/api/search/posting?pageTo=%d&pageSize=%d"
                   "&salaryCurrency=PLN&salaryPeriod=month&region=%s&language=en-GB",
                   page, NOFLUFF_PAGE_SIZE, ini_get(ctx->cfg, "region", "pl"));

        http_request req = {0};
        req.method = "POST";
        req.url = url.data;
        req.body = body;
        req.headers = headers;
        req.retries = 1;

        http_response resp;
        rc = source_http(ctx, &req, &resp, out->error, sizeof out->error);
        buf_free(&url);
        if (rc != 0)
            break;
        rc = nofluff_parse(resp.body.data ? resp.body.data : "", &out->jobs, &total_pages, out->error,
                           sizeof out->error);
        http_response_free(&resp);
        if (rc != 0)
            break;
    }
    free(body);
    return rc;
}

const source_type source_type_nofluff = {
    .type = "nofluffjobs",
    .keys = NOFLUFF_KEYS,
    .validate = nofluff_validate,
    .fetch = nofluff_fetch,
};
