#include "telegram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "util.h"

#define TG_TIMEOUT_S 30L
/* Longest rate-limit wait honoured inside one send; anything longer is left
 * to the caller's retry schedule. */
#define TG_MAX_INLINE_WAIT_S 60L
#define TG_RATE_LIMIT_RETRIES 2
/* Telegram's limit on the text of one message. Telegram counts UTF-16 units
 * after removing markup; the bytes counted here are never fewer. */
#define TG_MAX_TEXT_BYTES 4096

#define JOB_BUTTON_LABEL "Open posting"
/* First line of the test message, so that it cannot be taken for a real
 * notification, in the chat or in a notification preview. */
#define TEST_HEADING "TEST MESSAGE from jobbot"

static void append_escaped(buf *out, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&':
            buf_puts(out, "&amp;");
            break;
        case '<':
            buf_puts(out, "&lt;");
            break;
        case '>':
            buf_puts(out, "&gt;");
            break;
        default:
            buf_putc(out, *s);
        }
    }
}

static void append_text(buf *out, const char *s, int html)
{
    if (html)
        append_escaped(out, s);
    else
        buf_puts(out, s);
}

static char *format_job(const job *j, const char *source_label, int html)
{
    buf out;
    buf_init(&out);

    if (html)
        buf_puts(&out, "<b>");
    append_text(&out, j->title ? j->title : "(untitled)", html);
    if (html)
        buf_puts(&out, "</b>");

    int has_company = !str_blank(j->company);
    int has_location = !str_blank(j->location);
    if (has_company || has_location) {
        buf_putc(&out, '\n');
        if (has_company)
            append_text(&out, j->company, html);
        if (has_company && has_location)
            buf_puts(&out, " \xC2\xB7 "); /* middle dot */
        if (has_location)
            append_text(&out, j->location, html);
    }
    if (!str_blank(j->salary)) {
        buf_putc(&out, '\n');
        append_text(&out, j->salary, html);
    }
    if (!str_blank(source_label)) {
        buf_putc(&out, '\n');
        append_text(&out, source_label, html);
    }
    if (!html && !str_blank(j->url)) {
        buf_putc(&out, '\n');
        buf_puts(&out, j->url);
    }
    return buf_steal(&out);
}

char *telegram_format_job(const job *j, const char *source_label)
{
    return format_job(j, source_label, 1);
}

char *telegram_format_job_plain(const job *j, const char *source_label)
{
    return format_job(j, source_label, 0);
}

/* Starts a sendMessage request body with what every message has. Returns
 * NULL on failure. */
static cJSON *payload_new(const char *chat_id, const char *text, int parse_html)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;

    int ok = cJSON_AddStringToObject(root, "chat_id", chat_id) != NULL &&
             cJSON_AddStringToObject(root, "text", text) != NULL;
    if (ok && parse_html)
        ok = cJSON_AddStringToObject(root, "parse_mode", "HTML") != NULL;
    if (ok) {
        /* Keep the message compact: no page preview under it. */
        cJSON *preview = cJSON_AddObjectToObject(root, "link_preview_options");
        ok = preview && cJSON_AddTrueToObject(preview, "is_disabled") != NULL;
    }
    if (!ok) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

/* Adds a link button under the message, on a row of its own. Returns 1, or
 * 0 on failure. */
static int payload_add_button(cJSON *root, const char *label, const char *url)
{
    cJSON *markup = cJSON_GetObjectItemCaseSensitive(root, "reply_markup");
    if (!markup)
        markup = cJSON_AddObjectToObject(root, "reply_markup");
    cJSON *rows = cJSON_GetObjectItemCaseSensitive(markup, "inline_keyboard");
    if (markup && !rows)
        rows = cJSON_AddArrayToObject(markup, "inline_keyboard");
    cJSON *row = rows ? cJSON_CreateArray() : NULL;
    cJSON *button = row ? cJSON_CreateObject() : NULL;
    int ok = button && cJSON_AddStringToObject(button, "text", label) != NULL &&
             cJSON_AddStringToObject(button, "url", url) != NULL;
    /* Attach what was built so one cJSON_Delete(root) frees everything. */
    if (button && !cJSON_AddItemToArray(row, button)) {
        cJSON_Delete(button);
        ok = 0;
    }
    if (row && !cJSON_AddItemToArray(rows, row)) {
        cJSON_Delete(row);
        ok = 0;
    }
    return ok;
}

/* Prints the request body, or returns NULL when building it failed, and
 * frees `root` either way. */
static char *payload_finish(cJSON *root, int ok)
{
    char *payload = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return payload;
}

char *telegram_build_payload(const char *chat_id, const char *text, int parse_html,
                             const char *button_url)
{
    cJSON *root = payload_new(chat_id, text, parse_html);
    int ok = root != NULL;
    if (ok && button_url)
        ok = payload_add_button(root, JOB_BUTTON_LABEL, button_url);
    return payload_finish(root, ok);
}

tg_result telegram_parse_response(long http_status, const char *body, const char *transport_error)
{
    tg_result r;
    r.status = TG_RETRY;
    r.retry_after_s = -1;
    r.error[0] = '\0';

    if (http_status == 0) {
        snprintf(r.error, sizeof r.error, "could not reach Telegram: %s",
                 transport_error && *transport_error ? transport_error : "unknown error");
        return r;
    }

    cJSON *root = body ? cJSON_Parse(body) : NULL;
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    const cJSON *description = cJSON_GetObjectItemCaseSensitive(root, "description");
    const cJSON *parameters = cJSON_GetObjectItemCaseSensitive(root, "parameters");
    const cJSON *retry_after = cJSON_GetObjectItemCaseSensitive(parameters, "retry_after");

    if (http_status >= 200 && http_status <= 299 && cJSON_IsTrue(ok)) {
        r.status = TG_OK;
        cJSON_Delete(root);
        return r;
    }

    snprintf(r.error, sizeof r.error, "HTTP %ld: %s", http_status,
             cJSON_IsString(description) && description->valuestring ? description->valuestring
                                                                    : "no description in response");
    if (cJSON_IsNumber(retry_after) && retry_after->valuedouble >= 0 &&
        retry_after->valuedouble < 1e9)
        r.retry_after_s = (long)retry_after->valuedouble;

    if (http_status == 400)
        r.status = TG_REJECTED;
    else if (http_status == 401 || http_status == 403 || http_status == 404)
        r.status = TG_FATAL;
    else
        r.status = TG_RETRY; /* 429, 5xx, and anything unexpected */

    cJSON_Delete(root);
    return r;
}

/* Posts a request body built by one of the payload builders, and frees it.
 * `payload` is NULL when building it failed. */
static tg_result send_payload(http_client *http, const telegram_cfg *cfg, char *payload)
{
    tg_result r;
    if (!payload) {
        r.status = TG_REJECTED;
        r.retry_after_s = -1;
        snprintf(r.error, sizeof r.error, "could not encode the message");
        return r;
    }

    buf url;
    buf_init(&url);
    buf_printf(&url, "%s/bot%s/sendMessage", cfg->api_base, cfg->bot_token);

    static const char *const headers[] = {"Content-Type: application/json", NULL};
    http_request req = {0};
    req.method = "POST";
    req.url = url.data;
    req.label = "telegram sendMessage"; /* the URL holds the token */
    req.body = payload;
    req.headers = headers;
    req.timeout_s = TG_TIMEOUT_S;
    /* No blind retries: if a reply is lost after Telegram accepted the
     * message, repeating the request here would post it twice. */
    req.retries = 0;

    for (int attempt = 0;; attempt++) {
        http_response resp;
        http_send(http, &req, &resp);
        r = telegram_parse_response(resp.status, resp.body.data, resp.error);
        if (r.status == TG_RETRY && r.retry_after_s < 0 && resp.retry_after_s >= 0)
            r.retry_after_s = resp.retry_after_s;
        http_response_free(&resp);

        /* A rate limit means the message was not accepted, so waiting and
         * sending again is safe. */
        int rate_limited = r.status == TG_RETRY && r.retry_after_s >= 0;
        if (!rate_limited || attempt >= TG_RATE_LIMIT_RETRIES ||
            r.retry_after_s > TG_MAX_INLINE_WAIT_S)
            break;
        log_info("telegram: rate limited, waiting %lds", r.retry_after_s);
        if (sleep_seconds(r.retry_after_s + 1) != 0)
            break;
    }

    buf_free(&url);
    free(payload);
    return r;
}

tg_result telegram_send(http_client *http, const telegram_cfg *cfg, const char *text,
                        int parse_html, const char *button_url)
{
    return send_payload(http, cfg,
                        telegram_build_payload(cfg->chat_id, text, parse_html, button_url));
}

tg_result telegram_send_job(http_client *http, const telegram_cfg *cfg, const job *j,
                            const char *source_label)
{
    char *text = telegram_format_job(j, source_label);
    tg_result r = telegram_send(http, cfg, text, 1, j->url);
    free(text);
    if (r.status != TG_REJECTED)
        return r;

    log_warn("telegram: formatted message rejected (%s); resending as plain text", r.error);
    text = telegram_format_job_plain(j, source_label);
    r = telegram_send(http, cfg, text, 0, NULL);
    free(text);
    return r;
}

/* Formats the test message with the first `n` samples, or as many of them
 * as fit in one message, and stores how many that was in `*shown`. */
static char *format_test(const tg_sample *samples, size_t n, int html, size_t *shown)
{
    for (;; n--) {
        buf out;
        buf_init(&out);
        buf_puts(&out, html ? "<b>" TEST_HEADING "</b>\n" : TEST_HEADING "\n");
        buf_puts(&out, "Sent by \"jobbot --test-telegram\" to check the connection to this chat; "
                       "it does not announce new postings. ");
        if (n == 0)
            buf_puts(&out, "The database has no matching postings yet, so there is no example "
                           "to show.");
        else if (n == 1)
            buf_puts(&out, "Below is the newest matching posting already in the database, "
                           "formatted like a real notification.");
        else
            buf_printf(&out,
                       "Below are the %zu newest matching postings already in the database, "
                       "newest first, formatted like real notifications.",
                       n);
        for (size_t i = 0; i < n; i++) {
            char *posting = format_job(samples[i].j, samples[i].source_label, html);
            buf_puts(&out, "\n\n");
            buf_puts(&out, posting);
            free(posting);
        }
        if (out.len <= TG_MAX_TEXT_BYTES || n == 0) {
            *shown = n;
            return buf_steal(&out);
        }
        buf_free(&out);
    }
}

char *telegram_format_test(const tg_sample *samples, size_t n, int html)
{
    size_t shown;
    return format_test(samples, n, html, &shown);
}

char *telegram_build_test_payload(const char *chat_id, const tg_sample *samples, size_t n,
                                  int html)
{
    size_t shown = 0;
    char *text = format_test(samples, n, html, &shown);
    cJSON *root = payload_new(chat_id, text, html);
    free(text);

    int ok = root != NULL;
    /* As for a posting, the plain form has the links in its text instead. */
    for (size_t i = 0; ok && html && i < shown; i++) {
        if (!samples[i].j->url)
            continue;
        char label[48];
        if (shown == 1)
            snprintf(label, sizeof label, "%s", JOB_BUTTON_LABEL);
        else
            snprintf(label, sizeof label, "%s %zu", JOB_BUTTON_LABEL, i + 1);
        ok = payload_add_button(root, label, samples[i].j->url);
    }
    return payload_finish(root, ok);
}

tg_result telegram_send_test(http_client *http, const telegram_cfg *cfg, const tg_sample *samples,
                             size_t n)
{
    tg_result r =
        send_payload(http, cfg, telegram_build_test_payload(cfg->chat_id, samples, n, 1));
    if (r.status != TG_REJECTED)
        return r;

    log_warn("telegram: formatted test message rejected (%s); resending as plain text", r.error);
    return send_payload(http, cfg, telegram_build_test_payload(cfg->chat_id, samples, n, 0));
}
