#include "sources/source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

static const source_type *const SOURCE_TYPES[] = {
    &source_type_rss,     &source_type_nofluff, &source_type_justjoin,
    &source_type_linkedin, &source_type_pnet,    &source_type_adzuna,
};

#define SOURCE_TYPE_COUNT (sizeof SOURCE_TYPES / sizeof SOURCE_TYPES[0])

void source_result_init(source_result *r)
{
    job_list_init(&r->jobs);
    r->error[0] = '\0';
}

void source_result_free(source_result *r)
{
    job_list_free(&r->jobs);
}

const source_type *source_type_find(const char *type)
{
    for (size_t i = 0; i < SOURCE_TYPE_COUNT; i++)
        if (strcmp(SOURCE_TYPES[i]->type, type) == 0)
            return SOURCE_TYPES[i];
    return NULL;
}

void source_type_names(char *out, size_t outlen)
{
    size_t used = 0;
    if (outlen)
        out[0] = '\0';
    for (size_t i = 0; i < SOURCE_TYPE_COUNT; i++) {
        int n = snprintf(out + used, outlen - used, "%s%s", i ? ", " : "", SOURCE_TYPES[i]->type);
        if (n < 0 || (size_t)n >= outlen - used)
            break;
        used += (size_t)n;
    }
}

int source_http(const source_ctx *ctx, const http_request *req, http_response *resp, char *err,
                size_t errlen)
{
    if (http_send(ctx->http, req, resp) != 0) {
        snprintf(err, errlen, "request failed: %s", resp->error);
        http_response_free(resp);
        return -1;
    }
    if (resp->status < 200 || resp->status > 299) {
        if (resp->status == 403 || resp->status == 429 || resp->status == 503)
            snprintf(err, errlen, "HTTP %ld (the board may be blocking or rate-limiting this address)",
                     resp->status);
        else
            snprintf(err, errlen, "HTTP %ld", resp->status);
        http_response_free(resp);
        return -1;
    }
    return 0;
}

char *source_format_salary(int has_from, double from, int has_to, double to, const char *currency,
                           const char *unit, const char *kind)
{
    if (!has_from && !has_to)
        return NULL;

    buf out;
    buf_init(&out);
    if (has_from && has_to && from != to)
        buf_printf(&out, "%.0f\xE2\x80\x93%.0f", from, to); /* en dash */
    else if (has_from && has_to)
        buf_printf(&out, "%.0f", from);
    else if (has_from)
        buf_printf(&out, "from %.0f", from);
    else
        buf_printf(&out, "up to %.0f", to);

    if (currency && *currency)
        buf_printf(&out, " %s", currency);
    if (unit && *unit) {
        buf_putc(&out, '/');
        for (const char *p = unit; *p; p++)
            buf_putc(&out, (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : *p);
    }
    if (kind && *kind)
        buf_printf(&out, ", %s", kind);
    return buf_steal(&out);
}

void place_list_init(place_list *p)
{
    memset(p, 0, sizeof *p);
}

void place_list_add(place_list *p, const char *name)
{
    if (str_blank(name))
        return;
    for (size_t i = 0; i < p->nshown; i++)
        if (strcasecmp(p->shown[i], name) == 0)
            return;
    for (size_t i = 0; i < p->nhidden; i++)
        if (strcasecmp(p->hidden[i], name) == 0)
            return;

    if (p->nshown < PLACE_LIST_SHOWN) {
        p->shown[p->nshown++] = xstrdup(name);
        return;
    }
    p->hidden = xrealloc(p->hidden, (p->nhidden + 1) * sizeof *p->hidden);
    p->hidden[p->nhidden++] = xstrdup(name);
}

char *place_list_finish(place_list *p)
{
    buf out;
    buf_init(&out);
    for (size_t i = 0; i < p->nshown; i++) {
        if (i)
            buf_puts(&out, ", ");
        buf_puts(&out, p->shown[i]);
        free(p->shown[i]);
    }
    if (p->nhidden)
        buf_printf(&out, ", +%zu more", p->nhidden);
    for (size_t i = 0; i < p->nhidden; i++)
        free(p->hidden[i]);
    free(p->hidden);

    char *text = p->nshown ? buf_steal(&out) : NULL;
    buf_free(&out);
    place_list_init(p);
    return text;
}
