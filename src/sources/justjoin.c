/* JustJoin.it (justjoin.it) offers API. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "sources/parsers.h"
#include "sources/source.h"
#include "util.h"

static char *offer_location(const cJSON *offer)
{
    place_list places;
    place_list_init(&places);
    const cJSON *loc;
    cJSON_ArrayForEach(loc, cJSON_GetObjectItemCaseSensitive(offer, "locations"))
        place_list_add(&places, json_str(loc, "city"));
    place_list_add(&places, json_str(offer, "city"));
    char *cities = place_list_finish(&places);

    /* Say how the job is worked (remote, hybrid, office) next to the place. */
    const char *workplace = json_str(offer, "workplaceType");
    if (!workplace)
        return cities;
    buf out;
    buf_init(&out);
    if (cities)
        buf_printf(&out, "%s (%s)", cities, workplace);
    else
        buf_puts(&out, workplace);
    free(cities);
    return buf_steal(&out);
}

/* Pay as the employer stated it: the first entry in the offer's own
 * currency that has figures. Converted amounts are skipped. */
static char *offer_salary(const cJSON *offer)
{
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(offer, "employmentTypes"))
    {
        const char *source = json_str(e, "currencySource");
        if (source && strcmp(source, "original") != 0)
            continue;
        double from = 0, to = 0;
        int has_from = json_num(e, "from", &from);
        int has_to = json_num(e, "to", &to);
        char *s = source_format_salary(has_from, from, has_to, to, json_str(e, "currency"),
                                       json_str(e, "unit"), json_str(e, "type"));
        if (s)
            return s;
    }
    return NULL;
}

static char *offer_tags(const cJSON *offer)
{
    buf out;
    buf_init(&out);
    const cJSON *skill;
    cJSON_ArrayForEach(skill, cJSON_GetObjectItemCaseSensitive(offer, "requiredSkills"))
    {
        const char *name = json_str(skill, "name");
        if (!name)
            continue;
        if (out.len)
            buf_puts(&out, ", ");
        buf_puts(&out, name);
    }
    if (!out.len) {
        buf_free(&out);
        return NULL;
    }
    return buf_steal(&out);
}

int justjoin_parse(const char *json, job_list *out, char *err, size_t errlen)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) {
        snprintf(err, errlen, "unexpected response: no \"data\" list");
        cJSON_Delete(root);
        return -1;
    }

    const cJSON *o;
    cJSON_ArrayForEach(o, data)
    {
        const char *guid = json_str(o, "guid");
        const char *slug = json_str(o, "slug");
        const char *title = json_str(o, "title");
        if (!guid || !slug || !title)
            continue;

        job *j = job_list_add(out);
        j->ext_id = xstrdup(guid);
        j->title = xstrdup(title);
        buf url;
        buf_init(&url);
        buf_printf(&url, "https://justjoin.it/job-offer/%s", slug);
        j->url = buf_steal(&url);
        str_set(&j->company, json_str(o, "companyName"));
        j->location = offer_location(o);
        j->salary = offer_salary(o);
        j->tags = offer_tags(o);
        str_set(&j->posted_at, json_str(o, "publishedAt"));
    }
    cJSON_Delete(root);
    return 0;
}

/* ---- source type ------------------------------------------------------ */

static const char *const JUSTJOIN_KEYS[] = {"category", "keywords", "items", NULL};

static int justjoin_validate(const ini_section *cfg, char *err, size_t errlen)
{
    if (*ini_get(cfg, "category", "") == '\0' && *ini_get(cfg, "keywords", "") == '\0') {
        snprintf(err, errlen, "set 'category' (e.g. mobile) or 'keywords' (e.g. ios), or both");
        return -1;
    }
    long items;
    return ini_get_long(cfg, "items", 100, 10, 200, &items, err, errlen);
}

static int justjoin_fetch(const source_ctx *ctx, source_result *out)
{
    long items = 100;
    char ignored[64];
    ini_get_long(ctx->cfg, "items", 100, 10, 200, &items, ignored, sizeof ignored);
    const char *category = ini_get(ctx->cfg, "category", "");
    const char *keywords = ini_get(ctx->cfg, "keywords", "");

    /* Newest first, so a new offer is always inside the first page. */
    buf url;
    buf_init(&url);
    buf_printf(&url,
               "https://justjoin.it/api/candidate-api/offers?itemsCount=%ld"
               "&sortBy=publishedAt&orderBy=descending",
               items);
    if (*category) {
        buf_puts(&url, "&categories=");
        http_url_encode(&url, category);
    }
    if (*keywords) {
        buf_puts(&url, "&keywords=");
        http_url_encode(&url, keywords);
    }

    static const char *const headers[] = {"Accept: application/json", "Version: 2", NULL};
    http_request req = {0};
    req.url = url.data;
    req.headers = headers;
    req.retries = 1;

    http_response resp;
    int rc = source_http(ctx, &req, &resp, out->error, sizeof out->error);
    buf_free(&url);
    if (rc != 0)
        return -1;
    rc = justjoin_parse(resp.body.data ? resp.body.data : "", &out->jobs, out->error,
                        sizeof out->error);
    http_response_free(&resp);
    return rc;
}

const source_type source_type_justjoin = {
    .type = "justjoin",
    .keys = JUSTJOIN_KEYS,
    .validate = justjoin_validate,
    .fetch = justjoin_fetch,
};
