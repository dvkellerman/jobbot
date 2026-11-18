#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <curl/curl.h>

#include "util.h"

/* Largest response body accepted. Job-board pages and feeds are well under
 * this; the cap stops a misbehaving endpoint from filling memory. */
#define HTTP_MAX_BODY (16u * 1024u * 1024u)
#define HTTP_DEFAULT_TIMEOUT_S 45L
#define HTTP_CONNECT_TIMEOUT_S 15L
#define HTTP_MAX_RETRY_WAIT_S 60L

struct http_client {
    CURL *curl;
    char *user_agent;
};

typedef struct {
    http_response *resp;
    int too_large;
} transfer;

int http_global_init(void)
{
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1;
}

void http_global_cleanup(void)
{
    curl_global_cleanup();
}

http_client *http_client_new(const char *user_agent)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return NULL;
    http_client *c = xcalloc(1, sizeof *c);
    c->curl = curl;
    c->user_agent = xstrdup(user_agent);
    return c;
}

void http_client_free(http_client *c)
{
    if (!c)
        return;
    curl_easy_cleanup(c->curl);
    free(c->user_agent);
    free(c);
}

static void response_reset(http_response *resp)
{
    resp->status = 0;
    buf_clear(&resp->body);
    resp->retry_after_s = -1;
    resp->error[0] = '\0';
}

void http_response_free(http_response *resp)
{
    buf_free(&resp->body);
}

static size_t on_body(char *data, size_t size, size_t nmemb, void *userdata)
{
    transfer *t = userdata;
    size_t n = size * nmemb;
    if (t->resp->body.len + n > HTTP_MAX_BODY) {
        t->too_large = 1;
        return 0; /* aborts the transfer */
    }
    buf_append(&t->resp->body, data, n);
    return n;
}

/* Returns the trimmed value when `line` is the header `name`, else NULL.
 * The caller frees the result. */
static char *header_value(const char *line, size_t len, const char *name)
{
    size_t nlen = strlen(name);
    if (len <= nlen || strncasecmp(line, name, nlen) != 0 || line[nlen] != ':')
        return NULL;
    char *v = xstrndup(line + nlen + 1, len - nlen - 1);
    return str_trim(v);
}

static size_t on_header(char *data, size_t size, size_t nmemb, void *userdata)
{
    transfer *t = userdata;
    size_t n = size * nmemb;

    /* A new status line starts a new response (e.g. after a redirect), so
     * drop what was captured from the previous one. */
    if (n >= 5 && memcmp(data, "HTTP/", 5) == 0) {
        t->resp->retry_after_s = -1;
        return n;
    }

    char *v = header_value(data, n, "Retry-After");
    if (v) {
        /* Only the delta-seconds form is honoured; an HTTP-date is ignored. */
        char *end;
        long secs = strtol(v, &end, 10);
        if (end != v && *end == '\0' && secs >= 0)
            t->resp->retry_after_s = secs;
        free(v);
    }
    return n;
}

/* Lets a shutdown request abort a transfer instead of waiting out the timeout. */
static int on_progress(void *userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                       curl_off_t ulnow)
{
    (void)userdata;
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    return stop_requested() ? 1 : 0;
}

static int is_transient_status(long status)
{
    return status == 429 || (status >= 500 && status <= 599);
}

static int perform(http_client *c, const http_request *req, http_response *resp)
{
    CURL *curl = c->curl;
    transfer t = {resp, 0};
    char errbuf[CURL_ERROR_SIZE] = "";
    struct curl_slist *headers = NULL;
    int is_post = req->method && strcasecmp(req->method, "POST") == 0;

    response_reset(resp);
    curl_easy_reset(curl);

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, c->user_agent);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); /* all built-in encodings */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, HTTP_CONNECT_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, req->timeout_s > 0 ? req->timeout_s : HTTP_DEFAULT_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    if (is_post) {
        const char *body = req->body ? req->body : "";
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(body));
    }

    for (const char *const *h = req->headers; h && *h; h++)
        headers = curl_slist_append(headers, *h);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    /* The error buffer and transfer state live on this stack frame. */
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);

    if (rc != CURLE_OK) {
        if (t.too_large)
            snprintf(resp->error, sizeof resp->error, "response body exceeds %u bytes", HTTP_MAX_BODY);
        else
            snprintf(resp->error, sizeof resp->error, "%s", errbuf[0] ? errbuf : curl_easy_strerror(rc));
        buf_clear(&resp->body);
        resp->status = 0;
        return t.too_large ? -2 : -1;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->status);
    return 0;
}

int http_send(http_client *c, const http_request *req, http_response *resp)
{
    memset(resp, 0, sizeof *resp);
    buf_init(&resp->body);
    resp->retry_after_s = -1;

    int attempts = 1 + (req->retries > 0 ? req->retries : 0);
    int rc = -1;

    for (int attempt = 1; attempt <= attempts; attempt++) {
        rc = perform(c, req, resp);
        int transient = rc == -1 || (rc == 0 && is_transient_status(resp->status));
        if (!transient || attempt == attempts)
            break;

        long wait = 2L * attempt;
        if (resp->retry_after_s >= 0)
            wait = resp->retry_after_s;
        if (wait > HTTP_MAX_RETRY_WAIT_S)
            break; /* asked to wait longer than is useful here; let the caller back off */
        const char *name = req->label ? req->label : req->url;
        if (rc == 0)
            log_debug("http: %s returned %ld, retrying in %lds", name, resp->status, wait);
        else
            log_debug("http: %s failed (%s), retrying in %lds", name, resp->error, wait);
        if (sleep_seconds(wait) != 0)
            break; /* shutting down */
    }
    return rc == 0 ? 0 : -1;
}

void http_url_encode(buf *out, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            buf_putc(out, (char)*p);
        } else {
            buf_putc(out, '%');
            buf_putc(out, hex[*p >> 4]);
            buf_putc(out, hex[*p & 15]);
        }
    }
}
