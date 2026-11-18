#include <stdlib.h>
#include <unistd.h>

#include <sqlite3.h>

#include "store.h"
#include "test.h"
#include "util.h"

static job make_job(const char *ext_id, const char *title, const char *company)
{
    job j = {0};
    j.ext_id = xstrdup(ext_id);
    j.url = xstrdup("https://example.com/job");
    j.title = xstrdup(title);
    j.company = company ? xstrdup(company) : NULL;
    return j;
}

static void test_jobs(store *s)
{
    job a = make_job("1", "iOS Developer", "Acme");
    job b = make_job("2", "Swift Engineer", NULL);

    CHECK_INT(store_job_exists(s, "dou", "1"), 0);
    CHECK_INT(store_job_add(s, "dou", &a, "acme|ios developer", JOB_PENDING, 1000), 1);
    CHECK_INT(store_job_exists(s, "dou", "1"), 1);
    /* Same id on the same source is ignored; on another source it is new. */
    CHECK_INT(store_job_add(s, "dou", &a, "acme|ios developer", JOB_PENDING, 1001), 0);
    CHECK_INT(store_job_add(s, "djinni", &a, "acme|ios developer", JOB_DUPLICATE, 1002), 1);
    CHECK_INT(store_job_add(s, "dou", &b, NULL, JOB_SEEDED, 1003), 1);

    CHECK_INT(store_job_count(s, JOB_PENDING), 1);
    CHECK_INT(store_job_count(s, JOB_DUPLICATE), 1);
    CHECK_INT(store_job_count(s, JOB_SEEDED), 1);

    /* Only pending/notified postings inside the window count as recent. */
    CHECK_INT(store_fingerprint_recent(s, "acme|ios developer", 900), 1);
    CHECK_INT(store_fingerprint_recent(s, "acme|ios developer", 1001), 0);
    CHECK_INT(store_fingerprint_recent(s, "other|title", 0), 0);

    queued_job *q = NULL;
    size_t n = 0;
    CHECK_INT(store_queue_load(s, 10, &q, &n), 0);
    CHECK_INT(n, 1);
    if (n == 1) {
        CHECK_STR(q[0].source, "dou");
        CHECK_STR(q[0].j.ext_id, "1");
        CHECK_STR(q[0].j.title, "iOS Developer");
        CHECK_STR(q[0].j.company, "Acme");
        CHECK_STR(q[0].j.location, NULL);
        CHECK_INT(store_job_mark_notified(s, q[0].id, 2000), 0);
    }
    store_queue_free(q, n);

    CHECK_INT(store_job_count(s, JOB_PENDING), 0);
    CHECK_INT(store_job_count(s, JOB_NOTIFIED), 1);
    CHECK_INT(store_fingerprint_recent(s, "acme|ios developer", 900), 1);
    CHECK_INT(store_queue_load(s, 10, &q, &n), 0);
    CHECK_INT(n, 0);
    store_queue_free(q, n);

    job_free(&a);
    job_free(&b);
}

static void test_queue_order_and_limit(store *s)
{
    for (int i = 0; i < 5; i++) {
        char id[16], title[32];
        snprintf(id, sizeof id, "q%d", i);
        snprintf(title, sizeof title, "Job %d", i);
        job j = make_job(id, title, NULL);
        CHECK_INT(store_job_add(s, "queue", &j, NULL, JOB_PENDING, 5000 + i), 1);
        job_free(&j);
    }
    queued_job *q = NULL;
    size_t n = 0;
    CHECK_INT(store_queue_load(s, 3, &q, &n), 0);
    CHECK_INT(n, 3);
    if (n == 3) {
        CHECK_STR(q[0].j.title, "Job 0");
        CHECK_STR(q[2].j.title, "Job 2");
        /* An undeliverable posting leaves the queue for good. */
        CHECK_INT(store_job_mark_undeliverable(s, q[0].id), 0);
    }
    store_queue_free(q, n);
    CHECK_INT(store_job_count(s, JOB_UNDELIVERABLE), 1);
    CHECK_INT(store_queue_load(s, 3, &q, &n), 0);
    CHECK_INT(n, 3);
    if (n == 3)
        CHECK_STR(q[0].j.title, "Job 1");
    store_queue_free(q, n);
}

static int title_starts_with(const job *j, void *ctx)
{
    const char *prefix = ctx;
    return strncmp(j->title, prefix, strlen(prefix)) == 0;
}

static int accept_all(const job *j, void *ctx)
{
    (void)j;
    (void)ctx;
    return 1;
}

static void add_job(store *s, const char *id, const char *title, const char *status)
{
    job j = make_job(id, title, "Acme");
    j.description = xstrdup("long text");
    CHECK_INT(store_job_add(s, "board", &j, NULL, status, 100), 1);
    job_free(&j);
}

static void test_latest(void)
{
    char err[256] = "";
    store *s = NULL;
    CHECK_INT(store_open(&s, ":memory:", err, sizeof err), 0);
    if (!s)
        return;

    queued_job *q = NULL;
    size_t n = 99;
    CHECK_INT(store_jobs_latest(s, accept_all, NULL, 4, &q, &n), 0);
    CHECK_INT(n, 0);
    store_queue_free(q, n);

    /* Oldest first. Whatever the status, the filter decides: a posting
     * recorded on a source's first check, or one that did not match when it
     * was recorded, counts. A duplicate or a refused one never does. */
    add_job(s, "1", "iOS One", JOB_NOTIFIED);
    add_job(s, "2", "iOS Two", JOB_SEEDED);
    add_job(s, "3", "Java Three", JOB_UNMATCHED);
    add_job(s, "4", "iOS Four", JOB_UNMATCHED);
    add_job(s, "5", "iOS Five", JOB_DUPLICATE);
    add_job(s, "6", "iOS Six", JOB_PENDING);
    add_job(s, "7", "iOS Seven", JOB_UNDELIVERABLE);
    add_job(s, "8", "Java Eight", JOB_NOTIFIED);

    char prefix[] = "iOS";
    CHECK_INT(store_jobs_latest(s, title_starts_with, prefix, 10, &q, &n), 0);
    CHECK_INT(n, 4);
    if (n == 4) {
        CHECK_STR(q[0].j.title, "iOS Six");
        CHECK_STR(q[1].j.title, "iOS Four");
        CHECK_STR(q[2].j.title, "iOS Two");
        CHECK_STR(q[3].j.title, "iOS One");
        CHECK_STR(q[0].source, "board");
        CHECK_STR(q[0].j.ext_id, "6");
        CHECK_STR(q[0].j.url, "https://example.com/job");
        CHECK_STR(q[0].j.company, "Acme");
        CHECK_STR(q[0].j.description, NULL);
    }
    store_queue_free(q, n);

    /* Only the newest `limit` of them. */
    CHECK_INT(store_jobs_latest(s, title_starts_with, prefix, 2, &q, &n), 0);
    CHECK_INT(n, 2);
    if (n == 2) {
        CHECK_STR(q[0].j.title, "iOS Six");
        CHECK_STR(q[1].j.title, "iOS Four");
    }
    store_queue_free(q, n);

    char none[] = "Kotlin";
    CHECK_INT(store_jobs_latest(s, title_starts_with, none, 4, &q, &n), 0);
    CHECK_INT(n, 0);
    store_queue_free(q, n);

    /* Reading leaves every posting as it was. */
    CHECK_INT(store_job_count(s, JOB_PENDING), 1);
    CHECK_INT(store_job_count(s, JOB_NOTIFIED), 2);
    CHECK_INT(store_job_count(s, JOB_SEEDED), 1);
    store_close(s);
}

static void test_sources(store *s)
{
    source_state st;
    CHECK_INT(store_source_get(s, "dou", &st), 0);
    CHECK_INT(st.seeded, 0);
    CHECK_INT(st.failures, 0);
    source_state_free(&st);

    int failures = 0;
    CHECK_INT(store_source_failure(s, "dou", "HTTP 503", 5000, &failures), 0);
    CHECK_INT(failures, 1);
    CHECK_INT(store_source_failure(s, "dou", "timeout", 6000, &failures), 0);
    CHECK_INT(failures, 2);
    CHECK_INT(store_source_set_alerted(s, "dou", 1), 0);

    CHECK_INT(store_source_get(s, "dou", &st), 0);
    CHECK_INT(st.seeded, 0); /* failing never seeds */
    CHECK_INT(st.last_attempt, 6000);
    CHECK_INT(st.last_success, 0);
    CHECK_INT(st.failures, 2);
    CHECK_INT(st.alerted, 1);
    CHECK_STR(st.last_error, "timeout");
    source_state_free(&st);

    CHECK_INT(store_source_success(s, "dou", 7000), 0);
    CHECK_INT(store_source_get(s, "dou", &st), 0);
    CHECK_INT(st.seeded, 1);
    CHECK_INT(st.failures, 0);
    CHECK_INT(st.alerted, 1); /* cleared separately, after the recovery notice */
    CHECK_INT(st.last_success, 7000);
    CHECK_INT(st.last_attempt, 7000);
    CHECK_STR(st.last_error, NULL);
    source_state_free(&st);

    /* A failure after success keeps the source seeded. */
    CHECK_INT(store_source_failure(s, "dou", "boom", 8000, &failures), 0);
    CHECK_INT(failures, 1);
    CHECK_INT(store_source_get(s, "dou", &st), 0);
    CHECK_INT(st.seeded, 1);
    CHECK_INT(st.last_attempt, 8000);
    CHECK_INT(st.last_success, 7000);
    source_state_free(&st);
}

static void test_transaction_rollback(store *s)
{
    job j = make_job("tx", "Rolled back", NULL);
    CHECK_INT(store_begin(s), 0);
    CHECK_INT(store_job_add(s, "tx", &j, NULL, JOB_PENDING, 1), 1);
    store_rollback(s);
    CHECK_INT(store_job_exists(s, "tx", "tx"), 0);
    store_rollback(s); /* harmless outside a transaction */

    CHECK_INT(store_begin(s), 0);
    CHECK_INT(store_job_add(s, "tx", &j, NULL, JOB_UNMATCHED, 1), 1);
    CHECK_INT(store_commit(s), 0);
    CHECK_INT(store_job_exists(s, "tx", "tx"), 1);
    job_free(&j);
}

static void test_persistence(const char *path)
{
    char err[256] = "";
    store *s = NULL;
    CHECK_INT(store_open(&s, path, err, sizeof err), 0);
    if (!s)
        return;
    job j = make_job("keep", "Persisted", NULL);
    CHECK_INT(store_job_add(s, "p", &j, NULL, JOB_PENDING, 1), 1);
    job_free(&j);
    store_close(s);

    /* Reopening keeps the data and does not recreate the schema. */
    CHECK_INT(store_open(&s, path, err, sizeof err), 0);
    if (!s)
        return;
    CHECK_INT(store_job_exists(s, "p", "keep"), 1);
    store_close(s);
}

/* The schema as first released (version 1), before sources recorded when
 * they were last checked. */
static const char SCHEMA_V1[] =
    "CREATE TABLE jobs (id INTEGER PRIMARY KEY, source TEXT NOT NULL, ext_id TEXT NOT NULL,"
    "  url TEXT NOT NULL, title TEXT NOT NULL, company TEXT, location TEXT, salary TEXT,"
    "  tags TEXT, description TEXT, posted_at TEXT, fingerprint TEXT, status TEXT NOT NULL,"
    "  first_seen INTEGER NOT NULL, notified_at INTEGER, UNIQUE (source, ext_id));"
    "CREATE INDEX jobs_status ON jobs (status, id);"
    "CREATE INDEX jobs_fingerprint ON jobs (fingerprint);"
    "CREATE TABLE sources (name TEXT PRIMARY KEY, seeded INTEGER NOT NULL DEFAULT 0,"
    "  last_success INTEGER NOT NULL DEFAULT 0, failures INTEGER NOT NULL DEFAULT 0,"
    "  alerted INTEGER NOT NULL DEFAULT 0, last_error TEXT);"
    "INSERT INTO jobs (source, ext_id, url, title, status, first_seen)"
    "  VALUES ('dou', 'old', 'https://example.com/old', 'iOS Developer', 'pending', 10);"
    "INSERT INTO sources (name, seeded, last_success, failures, alerted, last_error)"
    "  VALUES ('dou', 1, 4000, 2, 1, 'HTTP 503');"
    "PRAGMA user_version = 1;";

static int db_version(const char *path)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int version = -1;
    if (sqlite3_open(path, &db) == SQLITE_OK &&
        sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        version = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return version;
}

/* A database written by the first release is upgraded in place and keeps
 * what it had recorded. */
static void test_upgrade_from_v1(const char *path)
{
    sqlite3 *db = NULL;
    CHECK_INT(sqlite3_open(path, &db), SQLITE_OK);
    CHECK_INT(sqlite3_exec(db, SCHEMA_V1, NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(db);
    CHECK_INT(db_version(path), 1);

    char err[256] = "";
    store *s = NULL;
    CHECK_INT(store_open(&s, path, err, sizeof err), 0);
    if (!s) {
        fprintf(stderr, "store_open: %s\n", err);
        return;
    }
    source_state st;
    CHECK_INT(store_source_get(s, "dou", &st), 0);
    CHECK_INT(st.seeded, 1);
    CHECK_INT(st.last_success, 4000);
    CHECK_INT(st.failures, 2);
    CHECK_INT(st.alerted, 1);
    CHECK_STR(st.last_error, "HTTP 503");
    CHECK_INT(st.last_attempt, 0); /* unknown, so the source counts as due */
    source_state_free(&st);
    CHECK_INT(store_job_exists(s, "dou", "old"), 1);
    CHECK_INT(store_job_count(s, JOB_PENDING), 1);

    int failures = 0;
    CHECK_INT(store_source_failure(s, "dou", "timeout", 9000, &failures), 0);
    CHECK_INT(failures, 3);
    CHECK_INT(store_source_get(s, "dou", &st), 0);
    CHECK_INT(st.last_attempt, 9000);
    source_state_free(&st);
    store_close(s);
    CHECK_INT(db_version(path), 2);

    /* Opening it again changes nothing. */
    CHECK_INT(store_open(&s, path, err, sizeof err), 0);
    if (!s)
        return;
    CHECK_INT(store_source_get(s, "dou", &st), 0);
    CHECK_INT(st.last_attempt, 9000);
    CHECK_INT(st.failures, 3);
    source_state_free(&st);
    store_close(s);
    CHECK_INT(db_version(path), 2);
}

static void test_readonly(const char *path)
{
    char err[256] = "";
    store *s = NULL;

    /* No database yet: says so, and does not create one. */
    unlink(path);
    CHECK_INT(store_open_readonly(&s, path, err, sizeof err), 1);
    CHECK(s == NULL);
    CHECK_INT(access(path, F_OK), -1);

    /* Neither is an empty file turned into a database. */
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f)
        fclose(f);
    CHECK_INT(store_open_readonly(&s, path, err, sizeof err), 1);
    CHECK(s == NULL);
    CHECK_INT(db_version(path), 0);
    unlink(path);

    CHECK_INT(store_open(&s, path, err, sizeof err), 0);
    if (!s)
        return;
    job j = make_job("1", "iOS Developer", "Acme");
    CHECK_INT(store_job_add(s, "dou", &j, NULL, JOB_PENDING, 1), 1);
    job_free(&j);

    /* Readable while another connection (a running jobbot) has it open. */
    store *ro = NULL;
    CHECK_INT(store_open_readonly(&ro, path, err, sizeof err), 0);
    CHECK(ro != NULL);
    if (ro) {
        queued_job *q = NULL;
        size_t n = 0;
        CHECK_INT(store_jobs_latest(ro, accept_all, NULL, 4, &q, &n), 0);
        CHECK_INT(n, 1);
        if (n == 1) {
            CHECK_STR(q[0].j.title, "iOS Developer");
            /* Writing through it is refused. */
            CHECK_INT(store_job_mark_notified(ro, q[0].id, 2), -1);
        }
        store_queue_free(q, n);
        store_close(ro);
    }
    CHECK_INT(store_job_count(s, JOB_PENDING), 1);
    store_close(s);

    /* A file that is not a database is an error, not "no database yet". */
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        for (int i = 0; i < 64; i++)
            fputs("this is not an SQLite database\n", f);
        fclose(f);
    }
    CHECK_INT(store_open_readonly(&s, path, err, sizeof err), -1);
    CHECK(s == NULL);
    CHECK(strstr(err, "cannot read database") != NULL);
}

/* A database from the first release is read as it is, not upgraded. */
static void test_readonly_v1(const char *path)
{
    sqlite3 *db = NULL;
    CHECK_INT(sqlite3_open(path, &db), SQLITE_OK);
    CHECK_INT(sqlite3_exec(db, SCHEMA_V1, NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(db);

    char err[256] = "";
    store *s = NULL;
    CHECK_INT(store_open_readonly(&s, path, err, sizeof err), 0);
    if (!s)
        return;
    queued_job *q = NULL;
    size_t n = 0;
    CHECK_INT(store_jobs_latest(s, accept_all, NULL, 4, &q, &n), 0);
    CHECK_INT(n, 1);
    if (n == 1)
        CHECK_STR(q[0].j.ext_id, "old");
    store_queue_free(q, n);
    store_close(s);
    CHECK_INT(db_version(path), 1);
}

int main(void)
{
    log_set_level(LOG_ERROR + 1); /* silence expected database errors */

    char err[256] = "";
    store *s = NULL;
    CHECK_INT(store_open(&s, ":memory:", err, sizeof err), 0);
    if (!s) {
        fprintf(stderr, "store_open: %s\n", err);
        return 1;
    }
    test_jobs(s);
    test_queue_order_and_limit(s);
    test_sources(s);
    test_transaction_rollback(s);
    store_close(s);
    test_latest();

    char path[] = "/tmp/jobbot-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        close(fd);
        test_persistence(path);
        CHECK_INT(db_version(path), 2);
        unlink(path);
    }

    char old_path[] = "/tmp/jobbot-test-XXXXXX";
    fd = mkstemp(old_path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        close(fd);
        test_upgrade_from_v1(old_path);
        unlink(old_path);
    }

    char ro_path[] = "/tmp/jobbot-test-XXXXXX";
    fd = mkstemp(ro_path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        close(fd);
        test_readonly(ro_path);
        unlink(ro_path);
        test_readonly_v1(ro_path);
        unlink(ro_path);
    }

    CHECK_INT(store_open(&s, "/nonexistent-dir/jobbot.db", err, sizeof err), -1);
    CHECK(strstr(err, "cannot open database") != NULL);

    return test_report();
}
