#ifndef JOBBOT_SOURCE_H
#define JOBBOT_SOURCE_H

#include <stddef.h>

#include "http.h"
#include "ini.h"
#include "job.h"

/* What a source is given for one poll. */
typedef struct {
    const char *name;          /* configured source name, for log messages */
    const ini_section *cfg;    /* the source's [source:NAME] section */
    http_client *http;
} source_ctx;

/* What a poll produced. */
typedef struct {
    job_list jobs;
    char error[256]; /* reason, when fetch returns -1 */
} source_result;

/* One kind of job board. Each [source:NAME] section selects a kind with its
 * `type` key. */
typedef struct {
    const char *type;
    /* NULL-terminated list of config keys this type accepts, besides the
     * common ones every source accepts. */
    const char *const *keys;
    /* Checks the section at startup. Returns 0, or -1 with a message. */
    int (*validate)(const ini_section *cfg, char *err, size_t errlen);
    /* Fetches and parses the board. Returns 0 on success, or -1 with
     * `out->error` set. Incomplete postings must not be added to `out->jobs`. */
    int (*fetch)(const source_ctx *ctx, source_result *out);
} source_type;

void source_result_init(source_result *r);
void source_result_free(source_result *r);

/* Returns the source type registered under `type`, or NULL. */
const source_type *source_type_find(const char *type);
/* Writes a comma-separated list of the registered type names to `out`. */
void source_type_names(char *out, size_t outlen);

/* Shared helper: performs a request and turns a transport failure or any
 * non-2xx status into an error message in `err`. Returns 0 on success, and
 * then the caller owns `resp` and must free it. */
int source_http(const source_ctx *ctx, const http_request *req, http_response *resp, char *err,
                size_t errlen);

/* Formats a pay range for display, e.g. "18000–22000 PLN/month, b2b".
 * `has_from`/`has_to` say which bounds are known; `currency`, `unit` (the
 * pay period) and `kind` (the contract type) may be NULL. Returns a new
 * string, or NULL when neither bound is known. */
char *source_format_salary(int has_from, double from, int has_to, double to, const char *currency,
                           const char *unit, const char *kind);

/* Builds a short, de-duplicated list of place names for display:
 * "Warszawa, Lublin, Łódź, Radom, +3 more". */
#define PLACE_LIST_SHOWN 4
typedef struct {
    char *shown[PLACE_LIST_SHOWN];
    size_t nshown;
    char **hidden; /* names beyond the shown ones, kept to de-duplicate */
    size_t nhidden;
} place_list;

void place_list_init(place_list *p);
/* Adds a name; NULL, blank and repeated names are ignored. */
void place_list_add(place_list *p, const char *name);
/* Returns the display string (NULL when no names were added) and frees the list. */
char *place_list_finish(place_list *p);

extern const source_type source_type_rss;
extern const source_type source_type_nofluff;
extern const source_type source_type_justjoin;
extern const source_type source_type_linkedin;
extern const source_type source_type_pnet;
extern const source_type source_type_adzuna;

#endif
