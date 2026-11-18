/* Adzuna job search API (developer.adzuna.com). Needs a free app id and key.
 *
 * Written against Adzuna's published response format; see the note in
 * config/jobbot.conf.example. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "sources/parsers.h"
#include "sources/source.h"
#include "text.h"
#include "util.h"

/* The API reports pay as bare numbers in the market's own currency. */
static const char *country_currency(const char *country)
{
    static const struct {
        const char *country, *currency;
    } table[] = {
        {"in", "INR"}, {"pl", "PLN"}, {"za", "ZAR"}, {"gb", "GBP"}, {"us", "USD"}, {"ca", "CAD"},
        {"au", "AUD"}, {"nz", "NZD"}, {"sg", "SGD"}, {"ch", "CHF"}, {"br", "BRL"}, {"mx", "MXN"},
        {"de", "EUR"}, {"fr", "EUR"}, {"nl", "EUR"}, {"it", "EUR"}, {"es", "EUR"}, {"at", "EUR"},
        {"be", "EUR"},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(table[i].country, country) == 0)
            return table[i].currency;
    return NULL;
}

/* Text fields can carry <strong> markers around the search words. */
static char *plain(const char *s)
{
    if (!s)
        return NULL;
    char *text = html_to_line(s);
    if (*text == '\0') {
        free(text);
        return NULL;
    }
    return text;
}

/* True for 1, "1" and true. */
static int json_flag(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v))
        return v->valuedouble != 0;
    if (cJSON_IsString(v) && v->valuestring)
        return strcmp(v->valuestring, "1") == 0;
    return cJSON_IsTrue(v);
}

int adzuna_parse(const char *json, const char *currency, job_list *out, char *err, size_t errlen)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    if (!cJSON_IsArray(results)) {
        snprintf(err, errlen, "unexpected response: no \"results\" list");
        cJSON_Delete(root);
        return -1;
    }

    const cJSON *r;
    cJSON_ArrayForEach(r, results)
    {
        /* The id is a string in the documentation; accept a number too. */
        char id_text[32] = "";
        const char *id = json_str(r, "id");
        double id_num;
        if (!id && json_num(r, "id", &id_num) && id_num > 0 && id_num < 9e15) {
            snprintf(id_text, sizeof id_text, "%.0f", id_num);
            id = id_text;
        }
        const char *url = json_str(r, "redirect_url");
        char *title = plain(json_str(r, "title"));
        if (!id || !url || !title) {
            free(title);
            continue;
        }

        job *j = job_list_add(out);
        j->ext_id = xstrdup(id);
        j->url = xstrdup(url);
        j->title = title;
        str_set(&j->company,
                json_str(cJSON_GetObjectItemCaseSensitive(r, "company"), "display_name"));
        str_set(&j->location,
                json_str(cJSON_GetObjectItemCaseSensitive(r, "location"), "display_name"));
        str_set(&j->posted_at, json_str(r, "created"));
        j->description = plain(json_str(r, "description"));

        /* Adzuna fills in an estimate when the advert states no pay; only
         * show figures the employer gave. */
        if (!json_flag(r, "salary_is_predicted")) {
            double from = 0, to = 0;
            int has_from = json_num(r, "salary_min", &from);
            int has_to = json_num(r, "salary_max", &to);
            j->salary = source_format_salary(has_from, from, has_to, to, currency, NULL, NULL);
        }
    }
    cJSON_Delete(root);
    return 0;
}

/* ---- source type ------------------------------------------------------ */

static const char *const ADZUNA_KEYS[] = {"api_country", "what", "app_id", "app_key", "results",
                                          "max_days_old", NULL};

static int adzuna_validate(const ini_section *cfg, char *err, size_t errlen)
{
    const char *country = ini_get(cfg, "api_country", "");
    if (strlen(country) != 2 || country[0] < 'a' || country[0] > 'z' || country[1] < 'a' ||
        country[1] > 'z') {
        snprintf(err, errlen, "'api_country' must be a two-letter Adzuna market such as in, pl "
                              "or za");
        return -1;
    }
    if (*ini_get(cfg, "what", "") == '\0') {
        snprintf(err, errlen, "'what' must be set to the search words (e.g. ios developer)");
        return -1;
    }
    if (*ini_get(cfg, "app_id", "") == '\0' || *ini_get(cfg, "app_key", "") == '\0') {
        snprintf(err, errlen, "'app_id' and 'app_key' must be set; they are free from "
                              "https://developer.adzuna.com/");
        return -1;
    }
    long v;
    if (ini_get_long(cfg, "results", 50, 10, 50, &v, err, errlen) != 0)
        return -1;
    return ini_get_long(cfg, "max_days_old", 7, 1, 60, &v, err, errlen);
}

static int adzuna_fetch(const source_ctx *ctx, source_result *out)
{
    long results = 50, max_days = 7;
    char ignored[64];
    ini_get_long(ctx->cfg, "results", 50, 10, 50, &results, ignored, sizeof ignored);
    ini_get_long(ctx->cfg, "max_days_old", 7, 1, 60, &max_days, ignored, sizeof ignored);
    const char *country = ini_get(ctx->cfg, "api_country", "");

    buf url;
    buf_init(&url);
    buf_printf(&url, "https://api.adzuna.com/v1/api/jobs/%s/search/1?app_id=", country);
    http_url_encode(&url, ini_get(ctx->cfg, "app_id", ""));
    buf_puts(&url, "&app_key=");
    http_url_encode(&url, ini_get(ctx->cfg, "app_key", ""));
    buf_printf(&url, "&results_per_page=%ld&max_days_old=%ld&sort_by=date&what=", results,
               max_days);
    http_url_encode(&url, ini_get(ctx->cfg, "what", ""));

    static const char *const headers[] = {"Accept: application/json", NULL};
    http_request req = {0};
    req.url = url.data;
    req.label = "Adzuna search"; /* the address carries the key; keep it out of the log */
    req.headers = headers;
    req.retries = 1;

    http_response resp;
    int rc = http_send(ctx->http, &req, &resp);
    buf_free(&url);
    if (rc != 0) {
        snprintf(out->error, sizeof out->error, "request failed: %.200s", resp.error);
    } else if (resp.status == 401 || resp.status == 403) {
        snprintf(out->error, sizeof out->error,
                 "HTTP %ld: Adzuna did not accept app_id/app_key", resp.status);
        rc = -1;
    } else if (resp.status < 200 || resp.status > 299) {
        snprintf(out->error, sizeof out->error, "HTTP %ld", resp.status);
        rc = -1;
    } else {
        rc = adzuna_parse(resp.body.data ? resp.body.data : "", country_currency(country),
                          &out->jobs, out->error, sizeof out->error);
    }
    http_response_free(&resp);
    return rc;
}

const source_type source_type_adzuna = {
    .type = "adzuna",
    .keys = ADZUNA_KEYS,
    .validate = adzuna_validate,
    .fetch = adzuna_fetch,
};
