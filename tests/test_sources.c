#include <stdlib.h>
#include <string.h>

#include "fixture.h"
#include "sources/htmlcards.h"
#include "sources/parsers.h"
#include "sources/source.h"
#include "test.h"
#include "util.h"

static const job *find_job(const job_list *l, const char *ext_id)
{
    for (size_t i = 0; i < l->n; i++)
        if (strcmp(l->items[i].ext_id, ext_id) == 0)
            return &l->items[i];
    return NULL;
}

static void test_format_salary(void)
{
    char *s = source_format_salary(1, 18000, 1, 22000, "PLN", "Month", "b2b");
    CHECK_STR(s, "18000\xE2\x80\x93" "22000 PLN/month, b2b");
    free(s);
    s = source_format_salary(1, 5000, 1, 5000, "USD", NULL, NULL);
    CHECK_STR(s, "5000 USD");
    free(s);
    s = source_format_salary(1, 4000, 0, 0, "EUR", "month", NULL);
    CHECK_STR(s, "from 4000 EUR/month");
    free(s);
    s = source_format_salary(0, 0, 1, 9000, NULL, NULL, "permanent");
    CHECK_STR(s, "up to 9000, permanent");
    free(s);
    CHECK(source_format_salary(0, 0, 0, 0, "PLN", "month", "b2b") == NULL);
}

static void test_place_list(void)
{
    place_list p;
    place_list_init(&p);
    CHECK(place_list_finish(&p) == NULL);

    place_list_init(&p);
    place_list_add(&p, "Warszawa");
    place_list_add(&p, NULL);
    place_list_add(&p, "  ");
    place_list_add(&p, "warszawa"); /* repeated, different case */
    place_list_add(&p, "Lublin");
    char *s = place_list_finish(&p);
    CHECK_STR(s, "Warszawa, Lublin");
    free(s);

    place_list_init(&p);
    const char *names[] = {"A", "B", "C", "D", "E", "F", "E", "A"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        place_list_add(&p, names[i]);
    s = place_list_finish(&p);
    CHECK_STR(s, "A, B, C, D, +2 more");
    free(s);
}

static void test_nofluff_fixture(void)
{
    char *json = fixture_read("tests/fixtures/nofluff_ios.json", NULL);
    CHECK(json != NULL);
    if (!json)
        return;

    job_list jobs;
    job_list_init(&jobs);
    char err[256] = "";
    int pages = 0;
    CHECK_INT(nofluff_parse(json, &jobs, &pages, err, sizeof err), 0);
    CHECK_INT(pages, 3);
    /* Eight entries, one per location, describe four jobs. */
    CHECK_INT(jobs.n, 4);
    if (jobs.n == 4) {
        const job *j = &jobs.items[0];
        CHECK_STR(j->ext_id, "6XYHOXTW");
        CHECK_STR(j->title, "Senior iOS Developer");
        CHECK_STR(j->company, "j-labs software specialist");
        CHECK_STR(j->url, "https://nofluffjobs.com/job/"
                          "senior-ios-developer-j-labs-software-specialist-remote");
        /* Province-only places carry no city and are left out. */
        CHECK_STR(j->location, "Remote, Kraków");
        CHECK_STR(j->salary, "23520\xE2\x80\x93" "30240 PLN/month, b2b");
        /* Only requirement tiles are skills; the "mobile" category tile is not. */
        CHECK_STR(j->tags, "iOS, UIKit, Auto Layout");
        CHECK_STR(j->posted_at, "2026-11-16");

        j = &jobs.items[1];
        CHECK_STR(j->ext_id, "LCFYI0QO");
        CHECK_STR(j->title, "iOS Developer");
        CHECK_STR(j->company, "Connectis_");
        CHECK_STR(j->location, "Warszawa");
        CHECK_STR(j->salary, "18000\xE2\x80\x93" "22000 PLN/month, b2b");
        CHECK_STR(j->tags, "iOS, Swift, SwiftUI");

        j = &jobs.items[2];
        CHECK_STR(j->ext_id, "YBKUWARO");
        CHECK_STR(j->title, "iOS Developer (Swift / SwiftUI)");
        CHECK_STR(j->location, "Warszawa, Lublin, Łódź, Radom, +1 more");

        j = &jobs.items[3];
        CHECK_STR(j->ext_id, "VYJYJIET");
        CHECK_STR(j->company, "GFT Poland");
        CHECK_STR(j->url, "https://nofluffjobs.com/job/expert-ios-consultant-gft-poland-warszawa-1");
        CHECK_STR(j->tags, "iOS, Swift, Objective-C");
        CHECK_STR(j->posted_at, "2026-11-12");
    }
    for (size_t i = 0; i < jobs.n; i++)
        CHECK(job_is_complete(&jobs.items[i]));

    /* A job that reappears on a later page is not added twice. */
    CHECK_INT(nofluff_parse(json, &jobs, &pages, err, sizeof err), 0);
    CHECK_INT(jobs.n, 4);

    job_list_free(&jobs);
    free(json);
}

static void test_nofluff_bad_input(void)
{
    job_list jobs;
    job_list_init(&jobs);
    char err[256] = "";
    int pages = 0;
    CHECK_INT(nofluff_parse("<html>blocked</html>", &jobs, &pages, err, sizeof err), -1);
    CHECK(strstr(err, "postings") != NULL);
    CHECK_INT(nofluff_parse("{\"postings\":{}}", &jobs, &pages, err, sizeof err), -1);
    CHECK_INT(nofluff_parse("", &jobs, &pages, err, sizeof err), -1);

    /* No results is a valid answer; entries missing required fields are skipped. */
    CHECK_INT(nofluff_parse("{\"postings\":[],\"totalPages\":0}", &jobs, &pages, err, sizeof err), 0);
    CHECK_INT(pages, 1);
    const char *partial = "{\"postings\":["
                          "{\"reference\":\"A1\",\"title\":\"iOS Dev\"},"
                          "{\"reference\":\"A2\",\"url\":\"x\",\"title\":\"  \"},"
                          "{\"reference\":7,\"url\":\"x\",\"title\":\"iOS Dev\"},"
                          "{\"reference\":\"A4\",\"url\":\"ios-dev-a4\",\"title\":\"iOS Dev\","
                          "\"fullyRemote\":true,\"salary\":{\"from\":\"lots\"},\"posted\":\"x\"}"
                          "],\"totalPages\":2}";
    CHECK_INT(nofluff_parse(partial, &jobs, &pages, err, sizeof err), 0);
    CHECK_INT(pages, 2);
    CHECK_INT(jobs.n, 1);
    if (jobs.n == 1) {
        CHECK_STR(jobs.items[0].ext_id, "A4");
        CHECK_STR(jobs.items[0].url, "https://nofluffjobs.com/job/ios-dev-a4");
        CHECK_STR(jobs.items[0].location, "Remote");
        CHECK_STR(jobs.items[0].company, NULL);
        CHECK_STR(jobs.items[0].salary, NULL);
        CHECK_STR(jobs.items[0].tags, NULL);
        CHECK_STR(jobs.items[0].posted_at, NULL);
    }
    job_list_free(&jobs);
}

static void test_justjoin_fixture(void)
{
    char *json = fixture_read("tests/fixtures/justjoin_mobile.json", NULL);
    CHECK(json != NULL);
    if (!json)
        return;

    job_list jobs;
    job_list_init(&jobs);
    char err[256] = "";
    CHECK_INT(justjoin_parse(json, &jobs, err, sizeof err), 0);
    CHECK_INT(jobs.n, 8);
    if (jobs.n == 8) {
        const job *j = &jobs.items[0];
        CHECK_STR(j->ext_id, "a23fe492-f857-4e58-87ed-8845bdec85ad");
        CHECK_STR(j->title, "Senior Frontend Engineer");
        CHECK_STR(j->company, "Jit Team");
        CHECK_STR(j->url, "https://justjoin.it/job-offer/"
                          "jit-team-senior-frontend-engineer-gdansk-mobile-0145dfba");
        CHECK_STR(j->location, "Gdańsk (hybrid)");
        /* The employer's own figures, not the converted currencies. */
        CHECK_STR(j->salary, "18900\xE2\x80\x93" "26250 PLN/month, b2b");
        CHECK_STR(j->tags, "React Native, TypeScript, AI");
        CHECK_STR(j->posted_at, "2026-11-18T09:55:04.9255363Z");

        j = &jobs.items[1];
        CHECK_STR(j->ext_id, "b101f180-9a5e-4928-ab36-2ace5201b29b");
        CHECK_STR(j->title, "iOS developer");
        CHECK_STR(j->company, "SCALO");
        CHECK_STR(j->url, "https://justjoin.it/job-offer/scalo-ios-developer-warszawa-mobile-71c563de");
        CHECK_STR(j->location, "Warszawa (hybrid)");
        CHECK_STR(j->salary, "16000\xE2\x80\x93" "19200 PLN/month, b2b");
        CHECK_STR(j->tags, "RxSwift, Swift, SwiftUI, REST API, UIKit, Jira, MVVM, AI");

        /* Undisclosed pay. */
        j = &jobs.items[3];
        CHECK_STR(j->company, "H2B Group");
        CHECK_STR(j->location, "Gdańsk, Warszawa (remote)");
        CHECK_STR(j->salary, NULL);

        j = &jobs.items[4];
        CHECK_STR(j->location, "Wrocław, Kraków, Poznań, Łódź, +1 more (remote)");
    }
    for (size_t i = 0; i < jobs.n; i++)
        CHECK(job_is_complete(&jobs.items[i]));
    job_list_free(&jobs);
    free(json);
}

static void test_justjoin_bad_input(void)
{
    job_list jobs;
    job_list_init(&jobs);
    char err[256] = "";
    CHECK_INT(justjoin_parse("Just a moment...", &jobs, err, sizeof err), -1);
    CHECK(strstr(err, "data") != NULL);
    CHECK_INT(justjoin_parse("{\"data\":null}", &jobs, err, sizeof err), -1);
    CHECK_INT(justjoin_parse("{\"data\":[]}", &jobs, err, sizeof err), 0);
    CHECK_INT(jobs.n, 0);

    const char *partial = "{\"data\":["
                          "{\"guid\":\"g1\",\"title\":\"iOS Dev\"},"
                          "{\"slug\":\"s2\",\"title\":\"iOS Dev\"},"
                          "{\"guid\":\"g3\",\"slug\":\"s3\",\"title\":\"iOS Dev\","
                          "\"employmentTypes\":[{\"from\":100,\"to\":200,\"currency\":\"EUR\","
                          "\"currencySource\":\"conversion\"},{\"from\":null,\"to\":900,"
                          "\"currency\":\"PLN\",\"unit\":\"day\"}],\"requiredSkills\":\"swift\"}]}";
    CHECK_INT(justjoin_parse(partial, &jobs, err, sizeof err), 0);
    CHECK_INT(jobs.n, 1);
    if (jobs.n == 1) {
        CHECK_STR(jobs.items[0].ext_id, "g3");
        CHECK_STR(jobs.items[0].url, "https://justjoin.it/job-offer/s3");
        CHECK_STR(jobs.items[0].salary, "up to 900 PLN/day");
        CHECK_STR(jobs.items[0].location, NULL);
        CHECK_STR(jobs.items[0].tags, NULL);
    }
    job_list_free(&jobs);
}

static void test_linkedin_fixture(void)
{
    size_t len = 0;
    char *html = fixture_read("tests/fixtures/linkedin_search.html", &len);
    CHECK(html != NULL);
    if (!html)
        return;

    job_list jobs;
    job_list_init(&jobs);
    char err[256] = "";
    CHECK_INT(linkedin_parse(html, len, &jobs, err, sizeof err), 10);
    CHECK_INT(jobs.n, 10);
    if (jobs.n == 10) {
        const job *j = &jobs.items[0];
        CHECK_STR(j->ext_id, "4480470699");
        CHECK_STR(j->title, "Senior AI Applied Software Engineer (iOS)");
        CHECK_STR(j->company, "Backbase");
        CHECK_STR(j->location, "Cracow, Małopolskie, Poland");
        CHECK_STR(j->posted_at, "2026-11-11");
        /* Tracking parameters are dropped. */
        CHECK_STR(j->url, "https://pl.linkedin.com/jobs/view/"
                          "senior-ai-applied-software-engineer-ios-at-backbase-4480470699");

        j = &jobs.items[1];
        CHECK_STR(j->ext_id, "4488409141");
        CHECK_STR(j->title, "Senior Software Engineer (iOS)");
        CHECK_STR(j->company, "Kingfisher plc");

        j = &jobs.items[2];
        CHECK_STR(j->ext_id, "4488528849");
        CHECK_STR(j->title, "Programista iOS - Warszawa (Mazowieckie), Polska K/M");
        CHECK_STR(j->company, "Astek");
        CHECK_STR(j->location, "Warsaw, Mazowieckie, Poland");
        CHECK_STR(j->posted_at, "2026-11-12");

        j = &jobs.items[9];
        CHECK_STR(j->ext_id, "4490727911");
        CHECK_STR(j->title, "IOS Developer");
        CHECK_STR(j->company, "Primaris");
        CHECK_STR(j->posted_at, "2026-11-13");
    }
    for (size_t i = 0; i < jobs.n; i++)
        CHECK(job_is_complete(&jobs.items[i]));

    /* The same cards on a second page are counted but not added again. */
    CHECK_INT(linkedin_parse(html, len, &jobs, err, sizeof err), 10);
    CHECK_INT(jobs.n, 10);

    job_list_free(&jobs);
    free(html);
}

static void test_linkedin_edge_cases(void)
{
    job_list jobs;
    job_list_init(&jobs);
    char err[256] = "";

    /* What LinkedIn sends past the last page of results. */
    const char *past_end = "<!DOCTYPE html>\n\n      \n";
    CHECK_INT(linkedin_parse(past_end, strlen(past_end), &jobs, err, sizeof err), 0);
    CHECK_INT(linkedin_parse("", 0, &jobs, err, sizeof err), 0);
    /* A sign-in wall has no cards either. */
    const char *wall = "<html><body><form class=\"login\"><input name=\"session_key\"></form>"
                       "</body></html>";
    CHECK_INT(linkedin_parse(wall, strlen(wall), &jobs, err, sizeof err), 0);
    CHECK_INT(jobs.n, 0);

    /* The card itself can be the link. */
    const char *link_card =
        "<li><a class=\"base-card base-search-card\" data-entity-urn=\"urn:li:jobPosting:77\" "
        "href=\"https://za.linkedin.com/jobs/view/ios-dev-at-acme-77?refId=abc&amp;pos=1\">"
        "<h3 class=\"base-search-card__title\">\n  iOS\n  Dev </h3>"
        "<h4 class=\"base-search-card__subtitle\"><a href=\"/company/acme\">Acme &amp; Co</a></h4>"
        "</a></li>";
    CHECK_INT(linkedin_parse(link_card, strlen(link_card), &jobs, err, sizeof err), 1);
    CHECK_INT(jobs.n, 1);
    if (jobs.n == 1) {
        CHECK_STR(jobs.items[0].ext_id, "77");
        CHECK_STR(jobs.items[0].title, "iOS Dev");
        CHECK_STR(jobs.items[0].company, "Acme & Co");
        CHECK_STR(jobs.items[0].url, "https://za.linkedin.com/jobs/view/ios-dev-at-acme-77");
        CHECK_STR(jobs.items[0].location, NULL);
        CHECK_STR(jobs.items[0].posted_at, NULL);
    }

    /* Cards are there but their contents moved: report it instead of
     * silently finding nothing. */
    const char *changed =
        "<li><div class=\"base-card\" data-entity-urn=\"urn:li:jobPosting:78\">"
        "<a class=\"new-link\" href=\"https://www.linkedin.com/jobs/view/78\">"
        "<h3 class=\"new-title\">iOS Dev</h3></a></div></li>";
    err[0] = '\0';
    CHECK_INT(linkedin_parse(changed, strlen(changed), &jobs, err, sizeof err), -1);
    CHECK(strstr(err, "layout") != NULL);
    CHECK_INT(jobs.n, 1);

    /* A link that is not http(s) is never used. */
    const char *script_link =
        "<div class=\"base-card\" data-entity-urn=\"urn:li:jobPosting:79\">"
        "<a class=\"base-card__full-link\" href=\"javascript:alert(1)\">x</a>"
        "<h3 class=\"base-search-card__title\">iOS Dev</h3></div>";
    CHECK_INT(linkedin_parse(script_link, strlen(script_link), &jobs, err, sizeof err), -1);
    CHECK_INT(jobs.n, 1);

    job_list_free(&jobs);
}

static void test_pnet_fixture(void)
{
    size_t len = 0;
    char *html = fixture_read("tests/fixtures/pnet_search.html", &len);
    CHECK(html != NULL);
    if (!html)
        return;

    job_list jobs;
    job_list_init(&jobs);
    char err[256] = "";
    const char *page = "https://www.pnet.co.za/jobs/ios-developer?ag=age_1&page=2";
    CHECK_INT(pnet_parse(html, len, page, &jobs, err, sizeof err), 25);
    CHECK_INT(jobs.n, 25);
    if (jobs.n == 25) {
        const job *j = &jobs.items[0];
        CHECK_STR(j->ext_id, "4308558");
        CHECK_STR(j->title, "iOS Developer");
        CHECK_STR(j->company, "Watershed Consulting");
        CHECK_STR(j->location, "JHB or CT or DBN");
        CHECK_STR(j->posted_at, "22 hours ago");
        CHECK_STR(j->url, "https://www.pnet.co.za/jobs--iOS-Developer-JHB-or-CT-or-DBN-"
                          "Watershed-Consulting--4308558-inline.html");
        CHECK(j->description &&
              strncmp(j->description, "Job Title iOS Developer Description We are looking for a "
                                      "skilled iOS Developer with 5-8 ye", 90) == 0);

        j = &jobs.items[1];
        CHECK_STR(j->ext_id, "4308212");
        CHECK_STR(j->title, "DMZ270 - iOS Developer");
        CHECK_STR(j->company, "Data Monkey (PTY) LTD");
        CHECK_STR(j->location, "Cape Town");
        CHECK_STR(j->posted_at, "1 day ago");

        j = &jobs.items[2];
        CHECK_STR(j->ext_id, "4292555");
        CHECK_STR(j->title, "IOS Software Engineer");
        CHECK_STR(j->company, "MIT Recruitment");

        j = &jobs.items[24];
        CHECK_STR(j->ext_id, "4306479");
        CHECK_STR(j->title, "Full Stack Developer");
        CHECK_STR(j->company, "Network IT");
        CHECK_STR(j->location, "Johannesburg North");
    }
    for (size_t i = 0; i < jobs.n; i++) {
        CHECK(job_is_complete(&jobs.items[i]));
        CHECK(strncmp(jobs.items[i].url, "https://www.pnet.co.za/", 23) == 0);
    }
    CHECK(find_job(&jobs, "4300238") != NULL);
    job_list_free(&jobs);
    free(html);
}

static void test_pnet_edge_cases(void)
{
    job_list jobs;
    job_list_init(&jobs);
    char err[256] = "";
    const char *page = "https://www.pnet.co.za/jobs/ios-developer";

    const char *none = "<html><body><h1>No jobs found</h1></body></html>";
    CHECK_INT(pnet_parse(none, strlen(none), page, &jobs, err, sizeof err), 0);

    /* One readable card and one without a title link: the page still counts
     * as readable, and the broken card is skipped. */
    const char *mixed =
        "<article data-at=\"job-item\" id=\"job-item-11\">"
        "<a data-at=\"job-item-title\" href=\"/jobs--iOS-Dev--11-inline.html\">iOS Dev</a>"
        "<span data-at=\"job-item-company-name\">Acme</span></article>"
        "<article data-at=\"job-item\" id=\"job-item-12\"><span>Sponsored</span></article>";
    CHECK_INT(pnet_parse(mixed, strlen(mixed), page, &jobs, err, sizeof err), 2);
    CHECK_INT(jobs.n, 1);
    if (jobs.n == 1) {
        CHECK_STR(jobs.items[0].ext_id, "11");
        CHECK_STR(jobs.items[0].url, "https://www.pnet.co.za/jobs--iOS-Dev--11-inline.html");
        CHECK_STR(jobs.items[0].company, "Acme");
        CHECK_STR(jobs.items[0].description, NULL);
    }

    /* The site puts <style> elements (and icons) inside the title link;
     * none of that is part of the title. */
    const char *styled =
        "<article data-at=\"job-item\" id=\"job-item-14\">"
        "<a data-at=\"job-item-title\" href=\"https://www.pnet.co.za/jobs--Swift-Dev--14-inline.html\">"
        "<style data-emotion=\"res x1\">.res-x1{display:flex;margin:0}</style>"
        "<div class=\"res-x1\"><script>var a = \"<b>\";</script>"
        "<svg><title>icon</title><path d=\"M0 0\"/></svg>"
        "<noscript><img alt=\"logo\"></noscript><div>Swift Dev</div></div></a>"
        "<span data-at=\"job-item-location\"><style>.res-x2{color:red}</style>Durban</span>"
        "</article>";
    CHECK_INT(pnet_parse(styled, strlen(styled), page, &jobs, err, sizeof err), 1);
    CHECK_INT(jobs.n, 2);
    if (jobs.n == 2) {
        CHECK_STR(jobs.items[1].ext_id, "14");
        CHECK_STR(jobs.items[1].title, "Swift Dev");
        CHECK_STR(jobs.items[1].location, "Durban");
    }

    const char *changed = "<article data-at=\"job-item\" id=\"job-item-13\">"
                          "<a data-at=\"title-v2\" href=\"/x\">iOS Dev</a></article>";
    CHECK_INT(pnet_parse(changed, strlen(changed), page, &jobs, err, sizeof err), -1);
    CHECK(strstr(err, "layout") != NULL);
    job_list_free(&jobs);
}

/* Adzuna has no saved response (it needs an account); this follows the
 * response format in its documentation. */
static void test_adzuna_documented_shape(void)
{
    const char *json =
        "{\"__CLASS__\":\"Adzuna::API::Response::JobSearchResults\",\"count\":3,\"results\":["
        "{\"salary_min\":50000,\"longitude\":-0.776902,"
        "\"location\":{\"__CLASS__\":\"Adzuna::API::Response::Location\","
        "\"area\":[\"South Africa\",\"Western Cape\",\"Cape Town\"],"
        "\"display_name\":\"Cape Town, Western Cape\"},"
        "\"salary_is_predicted\":0,"
        "\"description\":\"We need an <strong>iOS</strong> developer &amp; team player ...\","
        "\"created\":\"2026-11-08T18:07:39Z\","
        "\"redirect_url\":\"https://www.adzuna.co.za/land/ad/129698749?se=abc&v=712F\","
        "\"title\":\"Senior <strong>iOS</strong> Developer\","
        "\"category\":{\"label\":\"IT Jobs\",\"tag\":\"it-jobs\"},"
        "\"id\":\"129698749\",\"salary_max\":55000,"
        "\"company\":{\"__CLASS__\":\"Adzuna::API::Response::Company\","
        "\"display_name\":\"Corporate Project Solutions\"},"
        "\"contract_type\":\"permanent\"},"
        /* estimated pay, numeric id, no company */
        "{\"id\":4480470699,\"title\":\"Swift Engineer\","
        "\"redirect_url\":\"https://www.adzuna.co.za/land/ad/4480470699\","
        "\"salary_is_predicted\":\"1\",\"salary_min\":41000.5,\"salary_max\":41000.5,"
        "\"location\":{\"display_name\":\"Johannesburg\"}},"
        /* no link */
        "{\"id\":\"3\",\"title\":\"iOS Dev\"}"
        "]}";

    job_list jobs;
    job_list_init(&jobs);
    char err[256] = "";
    CHECK_INT(adzuna_parse(json, "ZAR", &jobs, err, sizeof err), 0);
    CHECK_INT(jobs.n, 2);
    if (jobs.n == 2) {
        const job *j = &jobs.items[0];
        CHECK_STR(j->ext_id, "129698749");
        CHECK_STR(j->title, "Senior iOS Developer");
        CHECK_STR(j->company, "Corporate Project Solutions");
        CHECK_STR(j->location, "Cape Town, Western Cape");
        CHECK_STR(j->salary, "50000\xE2\x80\x93" "55000 ZAR");
        CHECK_STR(j->url, "https://www.adzuna.co.za/land/ad/129698749?se=abc&v=712F");
        CHECK_STR(j->posted_at, "2026-11-08T18:07:39Z");
        CHECK_STR(j->description, "We need an iOS developer & team player ...");

        j = &jobs.items[1];
        CHECK_STR(j->ext_id, "4480470699");
        CHECK_STR(j->title, "Swift Engineer");
        CHECK_STR(j->company, NULL);
        CHECK_STR(j->salary, NULL); /* an estimate, not the employer's figure */
        CHECK_STR(j->location, "Johannesburg");
    }
    job_list_free(&jobs);

    job_list_init(&jobs);
    CHECK_INT(adzuna_parse("{\"exception\":\"AUTH_FAIL\",\"display\":\"Authorisation failed\"}",
                           NULL, &jobs, err, sizeof err), -1);
    CHECK(strstr(err, "results") != NULL);
    CHECK_INT(adzuna_parse("{\"results\":[]}", NULL, &jobs, err, sizeof err), 0);
    CHECK_INT(jobs.n, 0);
    job_list_free(&jobs);
}

static void test_registry(void)
{
    const char *types[] = {"rss", "nofluffjobs", "justjoin", "linkedin", "pnet", "adzuna"};
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        const source_type *t = source_type_find(types[i]);
        CHECK(t != NULL);
        if (t) {
            CHECK_STR(t->type, types[i]);
            CHECK(t->keys && t->validate && t->fetch);
        }
    }
    CHECK(source_type_find("monster") == NULL);
}

int main(void)
{
    test_format_salary();
    test_place_list();
    test_nofluff_fixture();
    test_nofluff_bad_input();
    test_justjoin_fixture();
    test_justjoin_bad_input();
    test_linkedin_fixture();
    test_linkedin_edge_cases();
    test_pnet_fixture();
    test_pnet_edge_cases();
    test_adzuna_documented_shape();
    test_registry();
    return test_report();
}
