#include <stdlib.h>

#include "pipeline.h"
#include "test.h"
#include "util.h"

#define DAY 86400LL

static void add(job_list *l, const char *id, const char *title, const char *company)
{
    job *j = job_list_add(l);
    j->ext_id = xstrdup(id);
    j->url = xstrdup("https://example.com/job");
    j->title = xstrdup(title);
    j->company = company ? xstrdup(company) : NULL;
}

static matcher make_matcher(void)
{
    matcher m;
    keywords_parse(&m.include, "ios, swift");
    keywords_parse(&m.exclude, "qa");
    m.match_tags = 1;
    return m;
}

static store *open_store(void)
{
    char err[256] = "";
    store *s = NULL;
    if (store_open(&s, ":memory:", err, sizeof err) != 0) {
        fprintf(stderr, "store_open: %s\n", err);
        exit(1);
    }
    return s;
}

static void test_first_poll_only_seeds(void)
{
    store *s = open_store();
    matcher m = make_matcher();
    ingest_stats st;
    job_list jobs;

    job_list_init(&jobs);
    add(&jobs, "1", "iOS Developer", "Acme");
    add(&jobs, "2", "Java Developer", "Acme");
    CHECK_INT(pipeline_ingest(s, &m, "dou", &jobs, 1000, 30 * DAY, &st), 0);
    CHECK_INT(st.fetched, 2);
    CHECK_INT(st.fresh, 2);
    CHECK_INT(st.seeded, 2);
    CHECK_INT(st.queued, 0);
    CHECK_INT(store_job_count(s, JOB_PENDING), 0);
    job_list_free(&jobs);

    /* Second poll: the old postings are known, only the new match is queued. */
    job_list_init(&jobs);
    add(&jobs, "1", "iOS Developer", "Acme");
    add(&jobs, "2", "Java Developer", "Acme");
    add(&jobs, "3", "Senior Swift Engineer", "Beta");
    add(&jobs, "4", "Android Developer", "Beta");
    add(&jobs, "5", "iOS QA Engineer", "Beta");
    CHECK_INT(pipeline_ingest(s, &m, "dou", &jobs, 2000, 30 * DAY, &st), 0);
    CHECK_INT(st.fetched, 5);
    CHECK_INT(st.fresh, 3);
    CHECK_INT(st.seeded, 0);
    CHECK_INT(st.queued, 1);
    CHECK_INT(st.unmatched, 2);
    job_list_free(&jobs);

    queued_job *q = NULL;
    size_t n = 0;
    CHECK_INT(store_queue_load(s, 10, &q, &n), 0);
    CHECK_INT(n, 1);
    if (n == 1)
        CHECK_STR(q[0].j.title, "Senior Swift Engineer");
    store_queue_free(q, n);

    /* Third poll with nothing new changes nothing. */
    job_list_init(&jobs);
    add(&jobs, "3", "Senior Swift Engineer", "Beta");
    CHECK_INT(pipeline_ingest(s, &m, "dou", &jobs, 3000, 30 * DAY, &st), 0);
    CHECK_INT(st.fresh, 0);
    CHECK_INT(store_job_count(s, JOB_PENDING), 1);
    job_list_free(&jobs);

    matcher_free(&m);
    store_close(s);
}

static void test_each_source_seeds_separately(void)
{
    store *s = open_store();
    matcher m = make_matcher();
    ingest_stats st;
    job_list jobs;

    job_list_init(&jobs);
    CHECK_INT(pipeline_ingest(s, &m, "a", &jobs, 1000, 30 * DAY, &st), 0); /* seeds "a" */
    add(&jobs, "1", "iOS Developer", NULL);
    CHECK_INT(pipeline_ingest(s, &m, "a", &jobs, 1100, 30 * DAY, &st), 0);
    CHECK_INT(st.queued, 1);
    /* A source added later starts with its own silent first poll. */
    CHECK_INT(pipeline_ingest(s, &m, "b", &jobs, 1200, 30 * DAY, &st), 0);
    CHECK_INT(st.seeded, 1);
    CHECK_INT(st.queued, 0);
    job_list_free(&jobs);

    matcher_free(&m);
    store_close(s);
}

static void test_cross_source_duplicates(void)
{
    store *s = open_store();
    matcher m = make_matcher();
    ingest_stats st;
    job_list empty, jobs;

    job_list_init(&empty);
    CHECK_INT(pipeline_ingest(s, &m, "dou", &empty, 1000, 30 * DAY, &st), 0);
    CHECK_INT(pipeline_ingest(s, &m, "linkedin", &empty, 1000, 30 * DAY, &st), 0);

    job_list_init(&jobs);
    add(&jobs, "d1", "Senior iOS Developer", "Acme, Inc.");
    CHECK_INT(pipeline_ingest(s, &m, "dou", &jobs, 2000, 30 * DAY, &st), 0);
    CHECK_INT(st.queued, 1);
    job_list_free(&jobs);

    /* Same company and title on another board inside the window: not sent again. */
    job_list_init(&jobs);
    add(&jobs, "l1", "senior  IOS developer", "ACME Inc");
    add(&jobs, "l2", "Senior iOS Developer", "Other Co");
    add(&jobs, "l3", "Senior iOS Developer", NULL);
    CHECK_INT(pipeline_ingest(s, &m, "linkedin", &jobs, 2000 + 5 * DAY, 30 * DAY, &st), 0);
    CHECK_INT(st.duplicates, 1);
    CHECK_INT(st.queued, 2); /* different company, and unknown company, both go out */
    job_list_free(&jobs);

    /* Outside the window the same posting counts as a repost. */
    job_list_init(&jobs);
    add(&jobs, "l4", "Senior iOS Developer", "Acme Inc");
    CHECK_INT(pipeline_ingest(s, &m, "linkedin", &jobs, 2000 + 40 * DAY, 30 * DAY, &st), 0);
    CHECK_INT(st.duplicates, 0);
    CHECK_INT(st.queued, 1);
    job_list_free(&jobs);

    /* A window of 0 turns the check off. */
    job_list_init(&jobs);
    add(&jobs, "l5", "Senior iOS Developer", "Acme Inc");
    CHECK_INT(pipeline_ingest(s, &m, "linkedin", &jobs, 2000 + 41 * DAY, 0, &st), 0);
    CHECK_INT(st.duplicates, 0);
    CHECK_INT(st.queued, 1);
    job_list_free(&jobs);

    /* Two identical postings in one poll: the second is the duplicate. */
    job_list_init(&jobs);
    add(&jobs, "d2", "Swift Developer", "Gamma");
    add(&jobs, "d3", "Swift Developer", "Gamma");
    add(&jobs, "d2", "Swift Developer", "Gamma"); /* same id listed twice */
    CHECK_INT(pipeline_ingest(s, &m, "dou", &jobs, 9000 + 60 * DAY, 30 * DAY, &st), 0);
    CHECK_INT(st.fresh, 2);
    CHECK_INT(st.queued, 1);
    CHECK_INT(st.duplicates, 1);
    job_list_free(&jobs);

    job_list_free(&empty);
    matcher_free(&m);
    store_close(s);
}

static void test_cleaning(void)
{
    store *s = open_store();
    matcher m = make_matcher();
    ingest_stats st;
    job_list jobs;

    job_list_init(&jobs);
    CHECK_INT(pipeline_ingest(s, &m, "x", &jobs, 1000, 0, &st), 0);

    add(&jobs, "1", "  iOS\n Developer \xFF ", "  ");
    jobs.items[0].description = xstrdup("line one\nline two\x01");
    add(&jobs, "   ", "iOS Developer", NULL); /* blank id: dropped */
    job *longj = job_list_add(&jobs);
    longj->ext_id = xstrdup("long");
    longj->url = xstrdup("https://example.com/long");
    char *title = xmalloc(1001);
    memset(title, 'a', 1000);
    memcpy(title, "iOS ", 4);
    title[1000] = '\0';
    longj->title = title;

    CHECK_INT(pipeline_ingest(s, &m, "x", &jobs, 2000, 0, &st), 0);
    CHECK_INT(st.fresh, 2);
    CHECK_INT(st.queued, 2);
    job_list_free(&jobs);

    queued_job *q = NULL;
    size_t n = 0;
    CHECK_INT(store_queue_load(s, 10, &q, &n), 0);
    CHECK_INT(n, 2);
    if (n == 2) {
        CHECK_STR(q[0].j.title, "iOS Developer ?");
        CHECK_STR(q[0].j.company, NULL);
        CHECK_STR(q[0].j.description, "line one\nline two?");
        CHECK_INT(strlen(q[1].j.title), 300);
    }
    store_queue_free(q, n);

    matcher_free(&m);
    store_close(s);
}

int main(void)
{
    test_first_poll_only_seeds();
    test_each_source_seeds_separately();
    test_cross_source_duplicates();
    test_cleaning();
    return test_report();
}
