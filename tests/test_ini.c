#include "ini.h"
#include "test.h"

static void test_parse_basic(void)
{
    ini_file f;
    char err[256] = "";
    const char *text = "# comment\n"
                       "; also a comment\n"
                       "\n"
                       "[general]\n"
                       "  poll = 20  \n"
                       "name=\"quoted value\"\n"
                       "empty =\n"
                       "[source:dou]\r\n"
                       "url = https://example.com/feed?a=1;b=2#frag\r\n"
                       "enabled = Yes";
    CHECK_INT(ini_parse(&f, text, "t", err, sizeof err), 0);
    CHECK_INT(f.n, 2);

    const ini_section *g = ini_find(&f, "general");
    CHECK(g != NULL);
    CHECK_STR(ini_get(g, "poll", NULL), "20");
    CHECK_STR(ini_get(g, "name", NULL), "quoted value");
    CHECK_STR(ini_get(g, "empty", "x"), "");
    CHECK_STR(ini_get(g, "missing", "fallback"), "fallback");

    /* Values keep '#' and ';' (no inline comments) and tolerate CRLF. */
    const ini_section *s = ini_find(&f, "source:dou");
    CHECK(s != NULL);
    CHECK_STR(ini_get(s, "url", NULL), "https://example.com/feed?a=1;b=2#frag");

    long n = 0;
    CHECK_INT(ini_get_long(g, "poll", 5, 1, 100, &n, err, sizeof err), 0);
    CHECK_INT(n, 20);
    CHECK_INT(ini_get_long(g, "missing", 5, 1, 100, &n, err, sizeof err), 0);
    CHECK_INT(n, 5);
    CHECK_INT(ini_get_long(g, "poll", 5, 30, 100, &n, err, sizeof err), -1);
    CHECK(strstr(err, "between 30 and 100") != NULL);
    CHECK_INT(ini_get_long(g, "name", 5, 1, 100, &n, err, sizeof err), -1);
    CHECK_INT(ini_get_long(g, "empty", 5, 1, 100, &n, err, sizeof err), -1);

    int b = 0;
    CHECK_INT(ini_get_bool(s, "enabled", 0, &b, err, sizeof err), 0);
    CHECK_INT(b, 1);
    CHECK_INT(ini_get_bool(s, "missing", 1, &b, err, sizeof err), 0);
    CHECK_INT(b, 1);
    CHECK_INT(ini_get_bool(g, "poll", 0, &b, err, sizeof err), -1);

    static const char *const allowed[] = {"url", NULL};
    const ini_kv *bad = ini_unknown_key(s, allowed);
    CHECK(bad != NULL);
    if (bad)
        CHECK_STR(bad->key, "enabled");
    static const char *const allowed_all[] = {"url", "enabled", NULL};
    CHECK(ini_unknown_key(s, allowed_all) == NULL);

    ini_free(&f);
}

static void expect_error(const char *text, const char *fragment)
{
    ini_file f;
    char err[256] = "";
    CHECK_INT(ini_parse(&f, text, "t", err, sizeof err), -1);
    CHECK(strstr(err, fragment) != NULL);
    if (!strstr(err, fragment))
        fprintf(stderr, "  error was: %s\n", err);
    CHECK_INT(f.n, 0);
}

static void test_parse_errors(void)
{
    expect_error("key = value\n", "t:1: setting appears before any [section]");
    expect_error("[a]\nnot a pair\n", "t:2: expected 'key = value'");
    expect_error("[a]\n[b\n", "t:2: malformed section header");
    expect_error("[a]\n[]\n", "t:2: empty section name");
    expect_error("[a]\nx = 1\n[a]\n", "t:3: duplicate section [a]");
    expect_error("[a]\nx = 1\n\nx = 2\n", "t:4: duplicate key 'x' in [a] (first set on line 2)");
    expect_error("[a]\n = 1\n", "t:2: missing key before '='");
}

static void test_load_missing_file(void)
{
    ini_file f;
    char err[256] = "";
    CHECK_INT(ini_load(&f, "/nonexistent/jobbot.conf", err, sizeof err), -1);
    CHECK(strstr(err, "cannot open /nonexistent/jobbot.conf") != NULL);
}

int main(void)
{
    test_parse_basic();
    test_parse_errors();
    test_load_missing_file();
    return test_report();
}
