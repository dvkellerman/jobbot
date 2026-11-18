#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "test.h"
#include "util.h"

static source_def make_source(long interval_s)
{
    source_def src;
    memset(&src, 0, sizeof src);
    src.interval_s = interval_s;
    return src;
}

static source_state make_state(long long last_attempt, int failures)
{
    source_state st;
    memset(&st, 0, sizeof st);
    st.last_attempt = last_attempt;
    st.failures = failures;
    return st;
}

static void test_delay(void)
{
    source_def src = make_source(900); /* 15 minutes */
    CHECK_INT(app_source_delay(&src, 0), 900);
    /* The first failure is retried at the usual interval; then it doubles. */
    CHECK_INT(app_source_delay(&src, 1), 900);
    CHECK_INT(app_source_delay(&src, 2), 1800);
    CHECK_INT(app_source_delay(&src, 3), 3600);
    CHECK_INT(app_source_delay(&src, 5), 14400);
    /* Capped at six hours, however long the outage. */
    CHECK_INT(app_source_delay(&src, 6), 21600);
    CHECK_INT(app_source_delay(&src, 7), 21600);
    CHECK_INT(app_source_delay(&src, 1000), 21600);

    /* A source checked less often than the cap is never retried sooner
     * than its own interval. */
    source_def daily = make_source(86400);
    CHECK_INT(app_source_delay(&daily, 0), 86400);
    CHECK_INT(app_source_delay(&daily, 4), 86400);
}

static void test_due(void)
{
    const long long t0 = 1800000000;
    source_def src = make_source(900);

    /* Never checked: due. */
    source_state st = make_state(0, 0);
    CHECK_INT(app_source_is_due(&src, &st, t0), 1);

    /* Checked at t0: due again once nine tenths of the interval have passed. */
    st = make_state(t0, 0);
    CHECK_INT(app_source_is_due(&src, &st, t0), 0);
    CHECK_INT(app_source_is_due(&src, &st, t0 + 60), 0);
    CHECK_INT(app_source_is_due(&src, &st, t0 + 809), 0);
    CHECK_INT(app_source_is_due(&src, &st, t0 + 810), 1);
    CHECK_INT(app_source_is_due(&src, &st, t0 + 900), 1);
    CHECK_INT(app_source_is_due(&src, &st, t0 + 100000), 1);

    /* The clock was set back: check rather than wait for it to catch up. */
    CHECK_INT(app_source_is_due(&src, &st, t0 - 5), 1);

    /* While failing, the wait grows: 30 minutes after the second failure. */
    st = make_state(t0, 2);
    CHECK_INT(app_source_is_due(&src, &st, t0 + 900), 0);
    CHECK_INT(app_source_is_due(&src, &st, t0 + 1619), 0);
    CHECK_INT(app_source_is_due(&src, &st, t0 + 1620), 1);

    /* An hourly source run from a 15-minute schedule is skipped three times. */
    source_def hourly = make_source(3600);
    st = make_state(t0, 0);
    CHECK_INT(app_source_is_due(&hourly, &st, t0 + 900), 0);
    CHECK_INT(app_source_is_due(&hourly, &st, t0 + 1800), 0);
    CHECK_INT(app_source_is_due(&hourly, &st, t0 + 2700), 0);
    CHECK_INT(app_source_is_due(&hourly, &st, t0 + 3600), 1);
    /* ... and still counts as due when that run comes a few minutes early. */
    CHECK_INT(app_source_is_due(&hourly, &st, t0 + 3300), 1);
}

static void test_path_expand_home(void)
{
    setenv("HOME", "/data/home", 1);
    char *p = path_expand_home("~/.jobbot/jobbot.db");
    CHECK_STR(p, "/data/home/.jobbot/jobbot.db");
    free(p);
    /* Only a leading "~/" is special. */
    p = path_expand_home("/var/tmp/~/jobbot.db");
    CHECK_STR(p, "/var/tmp/~/jobbot.db");
    free(p);
    p = path_expand_home("jobbot.db");
    CHECK_STR(p, "jobbot.db");
    free(p);
    p = path_expand_home("~other/jobbot.db");
    CHECK_STR(p, "~other/jobbot.db");
    free(p);
    p = path_expand_home("");
    CHECK_STR(p, "");
    free(p);

    unsetenv("HOME");
    CHECK(path_expand_home("~/.jobbot/jobbot.db") == NULL);
    p = path_expand_home("/abs/jobbot.db");
    CHECK_STR(p, "/abs/jobbot.db");
    free(p);
    setenv("HOME", "", 1);
    CHECK(path_expand_home("~/x") == NULL);
}

int main(void)
{
    test_delay();
    test_due();
    test_path_expand_home();
    return test_report();
}
