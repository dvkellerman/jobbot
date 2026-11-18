#include <stdlib.h>

#include "match.h"
#include "test.h"
#include "util.h"

static matcher make_matcher(int match_tags)
{
    matcher m;
    keywords_parse(&m.include, "iOS, macOS, Swift, SwiftUI, React  Native, , Objective-C");
    keywords_parse(&m.exclude, "ASO, manager, QA, Unreal, farmer");
    m.match_tags = match_tags;
    return m;
}

static match_result run(const matcher *m, const char *title, const char *tags)
{
    job j = {0};
    j.title = (char *)title;
    j.tags = (char *)tags;
    return match_job(m, &j);
}

static void test_keywords_parse(void)
{
    keyword_list k;
    keywords_parse(&k, " iOS ,React   Native,, ,РОЗРОБНИК");
    CHECK_INT(k.n, 3);
    if (k.n == 3) {
        CHECK_STR(k.words[0], "ios");
        CHECK_STR(k.words[1], "react native");
        CHECK_STR(k.words[2], "розробник");
    }
    keywords_free(&k);

    keywords_parse(&k, NULL);
    CHECK_INT(k.n, 0);
    keywords_free(&k);
    keywords_parse(&k, "");
    CHECK_INT(k.n, 0);
    keywords_free(&k);
}

static void test_titles_from_real_feeds(void)
{
    matcher m = make_matcher(0);
    match_result r;

    r = run(&m, "Head of Mobile (Angular + Capacitor → Native iOS/Android)", NULL);
    CHECK(r.matched);
    CHECK_STR(r.included_by, "ios");

    r = run(&m, "Senior iOS Developer (iGaming)", NULL);
    CHECK(r.matched);
    r = run(&m, "Middle iOS Engineer (Lviv only) (#6003)", NULL);
    CHECK(r.matched);
    r = run(&m, "iOS Swift Engineer", NULL);
    CHECK(r.matched);
    r = run(&m, "Trainee React Native Developer (Internship)", NULL);
    CHECK(r.matched);
    CHECK_STR(r.included_by, "react native");
    r = run(&m, "Senior SwiftUI   Engineer", NULL);
    CHECK(r.matched);
    CHECK_STR(r.included_by, "swiftui");
    r = run(&m, "IOS-розробник", NULL);
    CHECK(r.matched);
    r = run(&m, "Objective-C / Swift developer", NULL);
    CHECK(r.matched);

    /* In the iOS feeds but not iOS development roles. */
    r = run(&m, "Senior ASO Specialist (iOS / Android, iGaming)", NULL);
    CHECK(!r.matched);
    CHECK_STR(r.excluded_by, "aso");
    r = run(&m, "AI Native Mobile Product Manager (iOS + Android)", NULL);
    CHECK(!r.matched);
    CHECK_STR(r.excluded_by, "manager");
    r = run(&m, "Unreal Engine Developer (macOS Porting & Support)", NULL);
    CHECK(!r.matched);
    CHECK_STR(r.excluded_by, "unreal");
    r = run(&m, "IOS Farmer", NULL);
    CHECK(!r.matched);
    r = run(&m, "Solution Architect — Mobile (Contract, Part-time)", NULL);
    CHECK(!r.matched);
    CHECK_STR(r.included_by, NULL);
    r = run(&m, "Senior Flutter Developer", NULL);
    CHECK(!r.matched);
    r = run(&m, "Senior Mainframe Assembler Developer (HLASM / z/OS)", NULL);
    CHECK(!r.matched);
    r = run(&m, "Kiosk Software Developer", NULL);
    CHECK(!r.matched);

    matcher_free(&m);
}

static void test_tags(void)
{
    matcher with = make_matcher(1);
    matcher without = make_matcher(0);

    CHECK(run(&with, "Mobile Developer", "Kotlin, React Native, TypeScript").matched);
    CHECK(!run(&without, "Mobile Developer", "Kotlin, React Native, TypeScript").matched);
    CHECK(!run(&with, "Mobile Developer", "Kotlin, Flutter").matched);
    CHECK(!run(&with, "Mobile Developer", NULL).matched);
    /* Exclusion looks at the title only, and still applies to a tag match. */
    CHECK(!run(&with, "QA Engineer", "iOS, XCUITest").matched);
    CHECK(run(&with, "Mobile Developer", "iOS, QA").matched);

    matcher_free(&with);
    matcher_free(&without);
}

static void check_fingerprint(const char *company, const char *title, const char *expected)
{
    job j = {0};
    j.company = (char *)company;
    j.title = (char *)title;
    char *fp = job_fingerprint(&j);
    CHECK_STR(fp, expected);
    free(fp);
}

static void test_fingerprint(void)
{
    /* Case, punctuation and spacing do not matter. */
    check_fingerprint("Acme, Inc.", "Senior iOS Developer", "acmeinc|senioriosdeveloper");
    check_fingerprint("ACME Inc", "  senior   IOS developer ", "acmeinc|senioriosdeveloper");
    check_fingerprint("N-iX", "Middle iOS Engineer (Lviv only) (#6003)",
                      "nix|middleiosengineerlvivonly6003");
    check_fingerprint("ТОВ Ромашка", "iOS розробник", "товромашка|iosрозробник");
    /* Without a company (or with nothing comparable) there is no fingerprint. */
    check_fingerprint(NULL, "iOS Developer", NULL);
    check_fingerprint("  ", "iOS Developer", NULL);
    check_fingerprint("Acme", NULL, NULL);
    check_fingerprint("...", "iOS Developer", NULL);
    check_fingerprint("Acme", "!!!", NULL);
}

int main(void)
{
    test_keywords_parse();
    test_titles_from_real_feeds();
    test_tags();
    test_fingerprint();
    return test_report();
}
