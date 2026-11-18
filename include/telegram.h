#ifndef JOBBOT_TELEGRAM_H
#define JOBBOT_TELEGRAM_H

#include <stddef.h>

#include "http.h"
#include "job.h"

typedef struct {
    const char *api_base;  /* e.g. "https://api.telegram.org" */
    const char *bot_token; /* secret: never logged */
    const char *chat_id;
} telegram_cfg;

typedef enum {
    TG_OK = 0,
    TG_RETRY,    /* temporary: network trouble, rate limit, or Telegram 5xx */
    TG_REJECTED, /* Telegram refused this particular message; resending it unchanged will not help */
    TG_FATAL,    /* token or chat is wrong, or the bot is blocked: nothing will get through */
} tg_status;

typedef struct {
    tg_status status;
    long retry_after_s; /* wait asked for by Telegram on a rate limit, else -1 */
    char error[256];
} tg_result;

/* Builds the notification text for a posting, in Telegram's HTML markup.
 * `source_label` names the board. The caller frees the result. */
char *telegram_format_job(const job *j, const char *source_label);
/* Same content with no markup and the link written out, for when Telegram
 * rejects the formatted message. */
char *telegram_format_job_plain(const job *j, const char *source_label);

/* Builds the sendMessage request body. `parse_html` selects HTML markup;
 * `button_url`, when not NULL, adds one "Open posting" link button.
 * The caller frees the result. */
char *telegram_build_payload(const char *chat_id, const char *text, int parse_html,
                             const char *button_url);

/* Interprets a sendMessage response. `http_status` 0 means the request did
 * not reach Telegram and `transport_error` says why. */
tg_result telegram_parse_response(long http_status, const char *body, const char *transport_error);

/* Sends one message. See telegram_build_payload() for the arguments. */
tg_result telegram_send(http_client *http, const telegram_cfg *cfg, const char *text,
                        int parse_html, const char *button_url);

/* Sends a posting: formatted with a link button first and, if Telegram
 * rejects that, once more as plain text with the link in the message. */
tg_result telegram_send_job(http_client *http, const telegram_cfg *cfg, const job *j,
                            const char *source_label);

/* A stored posting shown as an example in the test message. */
typedef struct {
    const job *j;
    const char *source_label;
} tg_sample;

/* Builds the text of the test message: a heading that says it is a test,
 * then each sample exactly as telegram_format_job() (or, without `html`,
 * telegram_format_job_plain()) writes a real notification. `samples` are
 * newest first; when they do not all fit in one Telegram message the oldest
 * are left out. The caller frees the result. */
char *telegram_format_test(const tg_sample *samples, size_t n, int html);
/* Builds the sendMessage request body for the test message. With `html`
 * every sample shown gets a link button, numbered in the order shown. The
 * caller frees the result. */
char *telegram_build_test_payload(const char *chat_id, const tg_sample *samples, size_t n,
                                  int html);
/* Sends the test message the way a posting is sent: formatted with link
 * buttons first and, if Telegram rejects that, once more as plain text. */
tg_result telegram_send_test(http_client *http, const telegram_cfg *cfg, const tg_sample *samples,
                             size_t n);

#endif
