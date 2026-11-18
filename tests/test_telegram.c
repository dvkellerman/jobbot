#include <stdlib.h>

#include "cJSON.h"
#include "telegram.h"
#include "test.h"
#include "util.h"

static void test_format(void)
{
    job j = {0};
    j.title = "Senior iOS Developer <Swift & ObjC>";
    j.company = "R&D Lab";
    j.location = "Київ, віддалено";
    j.salary = "$4000–5500";
    j.url = "https://example.com/jobs/1?a=1&b=2";

    char *html = telegram_format_job(&j, "DOU · UA");
    CHECK_STR(html, "<b>Senior iOS Developer &lt;Swift &amp; ObjC&gt;</b>\n"
                    "R&amp;D Lab · Київ, віддалено\n"
                    "$4000–5500\n"
                    "DOU · UA");
    free(html);

    char *plain = telegram_format_job_plain(&j, "DOU · UA");
    CHECK_STR(plain, "Senior iOS Developer <Swift & ObjC>\n"
                     "R&D Lab · Київ, віддалено\n"
                     "$4000–5500\n"
                     "DOU · UA\n"
                     "https://example.com/jobs/1?a=1&b=2");
    free(plain);

    /* Missing fields leave no blank lines or stray separators. */
    job bare = {0};
    bare.title = "iOS Developer";
    bare.url = "https://example.com/2";
    html = telegram_format_job(&bare, "Djinni");
    CHECK_STR(html, "<b>iOS Developer</b>\nDjinni");
    free(html);

    bare.location = "Remote";
    html = telegram_format_job(&bare, "");
    CHECK_STR(html, "<b>iOS Developer</b>\nRemote");
    free(html);

    bare.location = NULL;
    bare.company = "Acme";
    html = telegram_format_job(&bare, NULL);
    CHECK_STR(html, "<b>iOS Developer</b>\nAcme");
    free(html);
}

static void test_payload(void)
{
    char *payload =
        telegram_build_payload("123456", "<b>Title</b>\n\"quoted\" Київ", 1, "https://example.com/j?a=1&b=2");
    CHECK(payload != NULL);
    cJSON *root = cJSON_Parse(payload ? payload : "");
    CHECK(root != NULL);

    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(root, "chat_id")), "123456");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(root, "text")), "<b>Title</b>\n\"quoted\" Київ");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(root, "parse_mode")), "HTML");
    CHECK(cJSON_IsTrue(
        cJSON_GetObjectItem(cJSON_GetObjectItem(root, "link_preview_options"), "is_disabled")));

    const cJSON *rows = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "reply_markup"), "inline_keyboard");
    CHECK_INT(cJSON_GetArraySize(rows), 1);
    const cJSON *row = cJSON_GetArrayItem(rows, 0);
    CHECK_INT(cJSON_GetArraySize(row), 1);
    const cJSON *button = cJSON_GetArrayItem(row, 0);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(button, "text")), "Open posting");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(button, "url")), "https://example.com/j?a=1&b=2");
    cJSON_Delete(root);
    free(payload);

    /* Plain text with no button: no markup fields at all. */
    payload = telegram_build_payload("@channel", "hello", 0, NULL);
    root = cJSON_Parse(payload ? payload : "");
    CHECK(root != NULL);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(root, "chat_id")), "@channel");
    CHECK(cJSON_GetObjectItem(root, "parse_mode") == NULL);
    CHECK(cJSON_GetObjectItem(root, "reply_markup") == NULL);
    cJSON_Delete(root);
    free(payload);
}

static void test_parse_response(void)
{
    tg_result r;

    r = telegram_parse_response(200, "{\"ok\":true,\"result\":{\"message_id\":7}}", NULL);
    CHECK_INT(r.status, TG_OK);

    r = telegram_parse_response(0, NULL, "Could not resolve host: api.telegram.org");
    CHECK_INT(r.status, TG_RETRY);
    CHECK(strstr(r.error, "Could not resolve host") != NULL);
    CHECK_INT(r.retry_after_s, -1);

    r = telegram_parse_response(
        429,
        "{\"ok\":false,\"error_code\":429,\"description\":\"Too Many Requests: retry after 17\","
        "\"parameters\":{\"retry_after\":17}}",
        NULL);
    CHECK_INT(r.status, TG_RETRY);
    CHECK_INT(r.retry_after_s, 17);
    CHECK_STR(r.error, "HTTP 429: Too Many Requests: retry after 17");

    r = telegram_parse_response(
        400, "{\"ok\":false,\"error_code\":400,\"description\":\"Bad Request: can't parse entities\"}",
        NULL);
    CHECK_INT(r.status, TG_REJECTED);
    CHECK_STR(r.error, "HTTP 400: Bad Request: can't parse entities");

    r = telegram_parse_response(401, "{\"ok\":false,\"error_code\":401,\"description\":\"Unauthorized\"}", NULL);
    CHECK_INT(r.status, TG_FATAL);
    r = telegram_parse_response(
        403, "{\"ok\":false,\"error_code\":403,\"description\":\"Forbidden: bot was blocked by the user\"}",
        NULL);
    CHECK_INT(r.status, TG_FATAL);
    r = telegram_parse_response(404, "{\"ok\":false,\"error_code\":404,\"description\":\"Not Found\"}", NULL);
    CHECK_INT(r.status, TG_FATAL);

    r = telegram_parse_response(502, "<html>Bad Gateway</html>", NULL);
    CHECK_INT(r.status, TG_RETRY);
    CHECK_STR(r.error, "HTTP 502: no description in response");

    /* A 200 that is not Telegram's success shape is not treated as delivered. */
    r = telegram_parse_response(200, "<html>captive portal</html>", NULL);
    CHECK_INT(r.status, TG_RETRY);
    r = telegram_parse_response(200, "{\"ok\":false,\"description\":\"odd\"}", NULL);
    CHECK_INT(r.status, TG_RETRY);
    r = telegram_parse_response(200, NULL, NULL);
    CHECK_INT(r.status, TG_RETRY);
}

#define TEST_INTRO                                                                          \
    "Sent by \"jobbot --test-telegram\" to check the connection to this chat; it does not " \
    "announce new postings. "

static const cJSON *keyboard_of(const cJSON *root)
{
    return cJSON_GetObjectItem(cJSON_GetObjectItem(root, "reply_markup"), "inline_keyboard");
}

static const char *button_field(const cJSON *rows, int row, const char *field)
{
    return cJSON_GetStringValue(
        cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetArrayItem(rows, row), 0), field));
}

static void test_test_message(void)
{
    job newest = {0};
    newest.title = "Senior iOS Developer <Swift & ObjC>";
    newest.company = "R&D Lab";
    newest.location = "Київ";
    newest.salary = "$4000–5500";
    newest.url = "https://example.com/jobs/1?a=1&b=2";
    job older = {0};
    older.title = "iOS Developer";
    older.url = "https://example.com/2";
    const tg_sample samples[] = {{&newest, "DOU · UA"}, {&older, "Djinni"}};

    /* Marked as a test on its first line; each posting is then written
     * exactly as its own notification would be. */
    char *html = telegram_format_test(samples, 2, 1);
    char *first = telegram_format_job(&newest, "DOU · UA");
    char *second = telegram_format_job(&older, "Djinni");
    buf expected;
    buf_init(&expected);
    buf_printf(&expected,
               "<b>TEST MESSAGE from jobbot</b>\n" TEST_INTRO
               "Below are the 2 newest matching postings already in the database, newest first, "
               "formatted like real notifications.\n\n%s\n\n%s",
               first, second);
    CHECK_STR(html, expected.data);
    buf_free(&expected);
    free(first);
    free(second);
    free(html);

    char *plain = telegram_format_test(samples, 2, 0);
    CHECK_STR(plain, "TEST MESSAGE from jobbot\n" TEST_INTRO
                     "Below are the 2 newest matching postings already in the database, newest "
                     "first, formatted like real notifications.\n\n"
                     "Senior iOS Developer <Swift & ObjC>\n"
                     "R&D Lab · Київ\n"
                     "$4000–5500\n"
                     "DOU · UA\n"
                     "https://example.com/jobs/1?a=1&b=2\n\n"
                     "iOS Developer\n"
                     "Djinni\n"
                     "https://example.com/2");
    free(plain);

    html = telegram_format_test(samples, 1, 1);
    CHECK_STR(html, "<b>TEST MESSAGE from jobbot</b>\n" TEST_INTRO
                    "Below is the newest matching posting already in the database, formatted "
                    "like a real notification.\n\n"
                    "<b>Senior iOS Developer &lt;Swift &amp; ObjC&gt;</b>\n"
                    "R&amp;D Lab · Київ\n"
                    "$4000–5500\n"
                    "DOU · UA");
    free(html);

    /* An empty database still gives a message to test the connection with. */
    html = telegram_format_test(NULL, 0, 1);
    CHECK_STR(html, "<b>TEST MESSAGE from jobbot</b>\n" TEST_INTRO
                    "The database has no matching postings yet, so there is no example to show.");
    free(html);

    /* Formatted: one numbered link button per posting, in the order shown. */
    char *payload = telegram_build_test_payload("123456", samples, 2, 1);
    cJSON *root = cJSON_Parse(payload ? payload : "");
    CHECK(root != NULL);
    html = telegram_format_test(samples, 2, 1);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(root, "chat_id")), "123456");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(root, "text")), html);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(root, "parse_mode")), "HTML");
    CHECK(cJSON_IsTrue(
        cJSON_GetObjectItem(cJSON_GetObjectItem(root, "link_preview_options"), "is_disabled")));
    const cJSON *rows = keyboard_of(root);
    CHECK_INT(cJSON_GetArraySize(rows), 2);
    CHECK_INT(cJSON_GetArraySize(cJSON_GetArrayItem(rows, 0)), 1);
    CHECK_STR(button_field(rows, 0, "text"), "Open posting 1");
    CHECK_STR(button_field(rows, 0, "url"), "https://example.com/jobs/1?a=1&b=2");
    CHECK_STR(button_field(rows, 1, "text"), "Open posting 2");
    CHECK_STR(button_field(rows, 1, "url"), "https://example.com/2");
    cJSON_Delete(root);
    free(html);
    free(payload);

    /* A single posting gets the button a real notification has. */
    payload = telegram_build_test_payload("123456", samples, 1, 1);
    root = cJSON_Parse(payload ? payload : "");
    rows = keyboard_of(root);
    CHECK_INT(cJSON_GetArraySize(rows), 1);
    CHECK_STR(button_field(rows, 0, "text"), "Open posting");
    cJSON_Delete(root);
    free(payload);

    /* Plain: the links are in the text, so no markup and no buttons. */
    payload = telegram_build_test_payload("123456", samples, 2, 0);
    root = cJSON_Parse(payload ? payload : "");
    CHECK(root != NULL);
    plain = telegram_format_test(samples, 2, 0);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(root, "text")), plain);
    CHECK(cJSON_GetObjectItem(root, "parse_mode") == NULL);
    CHECK(cJSON_GetObjectItem(root, "reply_markup") == NULL);
    cJSON_Delete(root);
    free(plain);
    free(payload);

    payload = telegram_build_test_payload("123456", NULL, 0, 1);
    root = cJSON_Parse(payload ? payload : "");
    CHECK(root != NULL);
    CHECK(cJSON_GetObjectItem(root, "reply_markup") == NULL);
    cJSON_Delete(root);
    free(payload);
}

static char *filled(char c, size_t n)
{
    char *s = malloc(n + 1);
    memset(s, c, n);
    s[n] = '\0';
    return s;
}

/* The test message never exceeds what one Telegram message holds (4096). */
static void test_test_message_fits(void)
{
    /* Every one-line field as long as the pipeline lets it be, and a long link. */
    char *title = filled('T', 300), *field = filled('f', 200), *url = filled('u', 1500);
    job big = {0};
    big.title = title;
    big.company = field;
    big.location = field;
    big.salary = field;
    big.url = url;
    job newest = big;
    newest.url = "https://example.com/newest";
    tg_sample samples[] = {
        {&newest, "DOU · UA"}, {&big, "DOU · UA"}, {&big, "DOU · UA"}, {&big, "DOU · UA"}};

    /* Formatted, four such postings still fit. */
    char *html = telegram_format_test(samples, 4, 1);
    CHECK(strlen(html) <= 4096);
    CHECK(strstr(html, "Below are the 4 newest matching postings") != NULL);
    free(html);

    /* In plain text the links are part of the message and they do not: the
     * oldest postings are left out. */
    char *plain = telegram_format_test(samples, 4, 0);
    CHECK(strlen(plain) <= 4096);
    CHECK(strstr(plain, "Below are the 2 newest matching postings") != NULL);
    CHECK(strstr(plain, "https://example.com/newest") != NULL);
    free(plain);

    /* Formatted, it takes an unusually long source label. The text and the
     * buttons agree on which postings are shown. */
    char *label = filled('L', 150);
    for (size_t i = 0; i < 4; i++)
        samples[i].source_label = label;
    char *payload = telegram_build_test_payload("1", samples, 4, 1);
    cJSON *root = cJSON_Parse(payload ? payload : "");
    CHECK(root != NULL);
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
    CHECK(text != NULL);
    if (text) {
        CHECK(strlen(text) <= 4096);
        CHECK(strstr(text, "Below are the 3 newest matching postings") != NULL);
    }
    const cJSON *rows = keyboard_of(root);
    CHECK_INT(cJSON_GetArraySize(rows), 3);
    CHECK_STR(button_field(rows, 0, "url"), "https://example.com/newest");
    CHECK_STR(button_field(rows, 2, "text"), "Open posting 3");
    cJSON_Delete(root);
    free(payload);

    free(label);
    free(title);
    free(field);
    free(url);
}

int main(void)
{
    test_format();
    test_payload();
    test_parse_response();
    test_test_message();
    test_test_message_fits();
    return test_report();
}
