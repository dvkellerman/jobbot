#ifndef JOBBOT_HTTP_H
#define JOBBOT_HTTP_H

#include <stddef.h>

#include "util.h"

typedef struct http_client http_client;

typedef struct {
    const char *method;            /* "GET" when NULL; "POST" sends `body` */
    const char *url;
    const char *label;             /* name for log messages instead of the URL, which
                                      may contain a secret; the URL is used when NULL */
    const char *body;              /* NUL-terminated request body, may be NULL */
    const char *const *headers;    /* NULL-terminated "Name: value" list, may be NULL */
    long timeout_s;                /* whole-request timeout, 0 = default */
    int retries;                   /* extra attempts after a transient failure */
} http_request;

typedef struct {
    long status;         /* HTTP status; 0 when no response was received */
    buf body;
    long retry_after_s;  /* Retry-After in seconds, or -1 when absent */
    char error[256];     /* transport error text when status is 0 */
} http_response;

/* Call once before any other http_* function, and once at exit. */
int http_global_init(void);
void http_global_cleanup(void);

http_client *http_client_new(const char *user_agent);
void http_client_free(http_client *c);

/* Performs the request. Returns 0 when an HTTP response was received
 * (whatever its status) and -1 on a transport failure, in which case
 * `resp->error` describes it. Transient failures (transport errors, 429 and
 * 5xx) are retried up to `req->retries` times.
 *
 * `resp` is always initialised; release it with http_response_free(). */
int http_send(http_client *c, const http_request *req, http_response *resp);
void http_response_free(http_response *resp);

/* Appends `s` to `out`, percent-encoded for use in a URL query component. */
void http_url_encode(buf *out, const char *s);

#endif
