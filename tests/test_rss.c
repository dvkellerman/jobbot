#include <stdlib.h>

#include "fixture.h"
#include "sources/rss.h"
#include "test.h"
#include "util.h"

static void test_dou_fixture(void)
{
    size_t len = 0;
    char *xml = fixture_read("tests/fixtures/dou_ios.xml", &len);
    CHECK(xml != NULL);
    if (!xml)
        return;

    job_list jobs;
    job_list_init(&jobs);
    char err[256] = "";
    CHECK_INT(rss_parse(xml, len, RSS_DOU, &jobs, err, sizeof err), 0);
    CHECK_INT(jobs.n, 8);
    if (jobs.n == 8) {
        const job *j = &jobs.items[0];
        CHECK_STR(j->title, "Head of Mobile (Angular + Capacitor → Native iOS/Android)");
        CHECK_STR(j->company, "ArboStar");
        CHECK_STR(j->location, "віддалено");
        CHECK_STR(j->salary, NULL);
        /* The tracking query is dropped and the link, not the changing guid, is the id. */
        CHECK_STR(j->url, "https://jobs.dou.ua/companies/arbostar/vacancies/379951/");
        CHECK_STR(j->ext_id, "https://jobs.dou.ua/companies/arbostar/vacancies/379951/");
        CHECK_STR(j->posted_at, "Fri, 13 Nov 2026 18:38:08 +0200");
        CHECK(j->description && strstr(j->description, "We’re looking for a Head of Mobile"));
        CHECK(j->description && strstr(j->description, "\n- Audit the current app"));
        CHECK(j->description && !strchr(j->description, '<'));

        j = &jobs.items[3];
        CHECK_STR(j->title, "AI Native Mobile Product Manager (iOS + Android)");
        CHECK_STR(j->company, "Mr Task");
        CHECK_STR(j->salary, "$2000–5000");
        CHECK_STR(j->location, "Київ, Атланта (США), Сан-Франциско (США), віддалено");

        j = &jobs.items[5]; /* title is escaped twice in the feed */
        CHECK_STR(j->title, "Unreal Engine Developer (macOS Porting & Support)");
        CHECK_STR(j->company, "The Fourth Law");
        CHECK_STR(j->location, "Львів");

        j = &jobs.items[7];
        CHECK_STR(j->title, "Middle iOS Engineer (Lviv only) (#6003)");
        CHECK_STR(j->company, "N-iX");
        CHECK_STR(j->location,
                  "Київ, Львів, Дніпро, Вінниця, Івано-Франківськ, Тернопіль, віддалено");
    }
    job_list_free(&jobs);
    free(xml);
}

static void test_djinni_fixture(void)
{
    size_t len = 0;
    char *xml = fixture_read("tests/fixtures/djinni_ios.xml", &len);
    CHECK(xml != NULL);
    if (!xml)
        return;

    job_list jobs;
    job_list_init(&jobs);
    char err[256] = "";
    CHECK_INT(rss_parse(xml, len, RSS_DJINNI, &jobs, err, sizeof err), 0);
    CHECK_INT(jobs.n, 7);
    if (jobs.n == 7) {
        const job *j = &jobs.items[0];
        CHECK_STR(j->title, "Senior ASO Specialist (iOS, iGaming)");
        CHECK_STR(j->ext_id, "850041");
        CHECK_STR(j->url, "https://djinni.co/jobs/850041-senior-aso-specialist-ios-igaming/");
        CHECK_STR(j->company, NULL);
        CHECK(j->description && strstr(j->description, "Leadsdoit"));

        CHECK_STR(jobs.items[1].title, "iOS Developer (Swift)");
        CHECK_STR(jobs.items[1].ext_id, "841429");
        CHECK_STR(jobs.items[6].ext_id, "855110");
    }
    job_list_free(&jobs);
    free(xml);
}

static int parse(const char *xml, rss_flavor flavor, job_list *jobs, char *err, size_t errlen)
{
    job_list_init(jobs);
    return rss_parse(xml, strlen(xml), flavor, jobs, err, errlen);
}

static void test_generic_and_incomplete_items(void)
{
    const char *xml =
        "<?xml version=\"1.0\"?><rss version=\"2.0\"><channel><title>Feed</title>"
        "<item><title><![CDATA[C++ &amp; Swift <b>Dev</b>]]></title>"
        "<link> https://example.com/a?x=1 </link><guid>guid-a</guid></item>"
        "<item><title>No guid</title><link>https://example.com/b</link></item>"
        "<item><title>No link</title><guid>guid-c</guid></item>"
        "<item><link>https://example.com/d</link><guid>guid-d</guid></item>"
        "<item><title>   </title><link>https://example.com/e</link></item>"
        "</channel></rss>";
    job_list jobs;
    char err[256] = "";
    CHECK_INT(parse(xml, RSS_GENERIC, &jobs, err, sizeof err), 0);
    CHECK_INT(jobs.n, 2);
    if (jobs.n == 2) {
        CHECK_STR(jobs.items[0].title, "C++ & Swift Dev");
        CHECK_STR(jobs.items[0].url, "https://example.com/a?x=1");
        CHECK_STR(jobs.items[0].ext_id, "guid-a");
        CHECK_STR(jobs.items[1].title, "No guid");
        CHECK_STR(jobs.items[1].ext_id, "https://example.com/b");
    }
    job_list_free(&jobs);
}

static void test_dou_title_shapes(void)
{
    const char *xml =
        "<rss><channel>"
        "<item><title>Розробник в команду iOS в Acme, від $1200, Київ</title>"
        "<link>https://jobs.dou.ua/companies/acme/vacancies/1/?utm_source=jobsrss</link></item>"
        "<item><title>iOS Developer</title>"
        "<link>https://jobs.dou.ua/companies/acme/vacancies/2/</link></item>"
        "<item><title>iOS Developer в Solo</title>"
        "<link>https://jobs.dou.ua/companies/solo/vacancies/3/#apply</link></item>"
        "</channel></rss>";
    job_list jobs;
    char err[256] = "";
    CHECK_INT(parse(xml, RSS_DOU, &jobs, err, sizeof err), 0);
    CHECK_INT(jobs.n, 3);
    if (jobs.n == 3) {
        /* Only the last " в " separates the company. */
        CHECK_STR(jobs.items[0].title, "Розробник в команду iOS");
        CHECK_STR(jobs.items[0].company, "Acme");
        CHECK_STR(jobs.items[0].salary, "від $1200");
        CHECK_STR(jobs.items[0].location, "Київ");
        CHECK_STR(jobs.items[0].url, "https://jobs.dou.ua/companies/acme/vacancies/1/");

        CHECK_STR(jobs.items[1].title, "iOS Developer");
        CHECK_STR(jobs.items[1].company, NULL);

        CHECK_STR(jobs.items[2].title, "iOS Developer");
        CHECK_STR(jobs.items[2].company, "Solo");
        CHECK_STR(jobs.items[2].location, NULL);
        CHECK_STR(jobs.items[2].url, "https://jobs.dou.ua/companies/solo/vacancies/3/");
    }
    job_list_free(&jobs);
}

static void test_djinni_link_shapes(void)
{
    const char *xml = "<rss><channel>"
                      "<item><title>A</title><link>https://djinni.co/jobs/123-a/?from=rss</link></item>"
                      "<item><title>B</title><link>https://djinni.co/jobs/456-b</link></item>"
                      "<item><title>C</title><link>https://djinni.co/jobs/no-number/</link></item>"
                      "</channel></rss>";
    job_list jobs;
    char err[256] = "";
    CHECK_INT(parse(xml, RSS_DJINNI, &jobs, err, sizeof err), 0);
    CHECK_INT(jobs.n, 3);
    if (jobs.n == 3) {
        CHECK_STR(jobs.items[0].ext_id, "123");
        CHECK_STR(jobs.items[1].ext_id, "456");
        CHECK_STR(jobs.items[2].ext_id, "https://djinni.co/jobs/no-number/");
    }
    job_list_free(&jobs);
}

static void test_bad_documents(void)
{
    job_list jobs;
    char err[256] = "";

    CHECK_INT(parse("<html><body>Just a moment...</body></html>", RSS_GENERIC, &jobs, err,
                    sizeof err),
              -1);
    CHECK(strstr(err, "expected an <rss> document, got <html>") != NULL);
    CHECK_INT(jobs.n, 0);
    job_list_free(&jobs);

    CHECK_INT(parse("<rss><channel><item>", RSS_GENERIC, &jobs, err, sizeof err), -1);
    CHECK(strstr(err, "not well-formed") != NULL);
    job_list_free(&jobs);

    job_list_init(&jobs);
    CHECK_INT(rss_parse("", 0, RSS_GENERIC, &jobs, err, sizeof err), -1);
    job_list_free(&jobs);

    /* An empty feed is valid; whether that is suspicious is the caller's call. */
    CHECK_INT(parse("<rss><channel><title>t</title></channel></rss>", RSS_GENERIC, &jobs, err,
                    sizeof err),
              0);
    CHECK_INT(jobs.n, 0);
    job_list_free(&jobs);
}

static void test_external_entity_is_not_read(void)
{
    /* A hostile feed must not be able to pull a local file into a title. */
    const char *xml = "<?xml version=\"1.0\"?>"
                      "<!DOCTYPE rss [<!ENTITY xxe SYSTEM \"file:///etc/passwd\">]>"
                      "<rss><channel><item><title>T &xxe; end</title>"
                      "<link>https://example.com/x</link></item></channel></rss>";
    job_list jobs;
    char err[256] = "";
    int rc = parse(xml, RSS_GENERIC, &jobs, err, sizeof err);
    CHECK_INT(rc, 0);
    for (size_t i = 0; i < jobs.n; i++) {
        CHECK(strstr(jobs.items[i].title, "root:") == NULL);
        CHECK(strstr(jobs.items[i].title, "/bin/") == NULL);
    }
    job_list_free(&jobs);
}

static void test_flavor_names(void)
{
    rss_flavor f = RSS_GENERIC;
    CHECK_INT(rss_flavor_parse("dou", &f), 0);
    CHECK_INT(f, RSS_DOU);
    CHECK_INT(rss_flavor_parse("djinni", &f), 0);
    CHECK_INT(f, RSS_DJINNI);
    CHECK_INT(rss_flavor_parse("generic", &f), 0);
    CHECK_INT(f, RSS_GENERIC);
    CHECK_INT(rss_flavor_parse("DOU", &f), -1);
}

int main(void)
{
    test_dou_fixture();
    test_djinni_fixture();
    test_generic_and_incomplete_items();
    test_dou_title_shapes();
    test_djinni_link_shapes();
    test_bad_documents();
    test_external_entity_is_not_read();
    test_flavor_names();
    return test_report();
}
