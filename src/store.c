#include "store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#include "util.h"

#define SCHEMA_VERSION 2

struct store {
    sqlite3 *db;
};

static const char SCHEMA_SQL[] =
    "CREATE TABLE jobs ("
    "  id INTEGER PRIMARY KEY,"
    "  source TEXT NOT NULL,"
    "  ext_id TEXT NOT NULL,"
    "  url TEXT NOT NULL,"
    "  title TEXT NOT NULL,"
    "  company TEXT,"
    "  location TEXT,"
    "  salary TEXT,"
    "  tags TEXT,"
    "  description TEXT,"
    "  posted_at TEXT,"
    "  fingerprint TEXT,"
    "  status TEXT NOT NULL,"
    "  first_seen INTEGER NOT NULL,"
    "  notified_at INTEGER,"
    "  UNIQUE (source, ext_id)"
    ");"
    "CREATE INDEX jobs_status ON jobs (status, id);"
    "CREATE INDEX jobs_fingerprint ON jobs (fingerprint);"
    "CREATE TABLE sources ("
    "  name TEXT PRIMARY KEY,"
    "  seeded INTEGER NOT NULL DEFAULT 0,"
    "  last_success INTEGER NOT NULL DEFAULT 0,"
    "  failures INTEGER NOT NULL DEFAULT 0,"
    "  alerted INTEGER NOT NULL DEFAULT 0,"
    "  last_error TEXT,"
    "  last_attempt INTEGER NOT NULL DEFAULT 0"
    ");";

/* Version 1 did not record when a source was last checked. */
static const char MIGRATE_1_TO_2_SQL[] =
    "ALTER TABLE sources ADD COLUMN last_attempt INTEGER NOT NULL DEFAULT 0;";

static int db_error(store *s, const char *what)
{
    log_error("database: %s: %s", what, sqlite3_errmsg(s->db));
    return -1;
}

static int exec(store *s, const char *sql)
{
    char *msg = NULL;
    if (sqlite3_exec(s->db, sql, NULL, NULL, &msg) != SQLITE_OK) {
        log_error("database: %s: %s", sql, msg ? msg : "unknown error");
        sqlite3_free(msg);
        return -1;
    }
    return 0;
}

static int prepare(store *s, const char *sql, sqlite3_stmt **st)
{
    if (sqlite3_prepare_v2(s->db, sql, -1, st, NULL) != SQLITE_OK)
        return db_error(s, sql);
    return 0;
}

/* Binds text, or NULL when `v` is NULL. */
static void bind_text(sqlite3_stmt *st, int idx, const char *v)
{
    if (v)
        sqlite3_bind_text(st, idx, v, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, idx);
}

static char *column_dup(sqlite3_stmt *st, int col)
{
    const unsigned char *v = sqlite3_column_text(st, col);
    return v ? xstrdup((const char *)v) : NULL;
}

/* Runs a statement that returns no rows, then finalizes it. */
static int run(store *s, sqlite3_stmt *st, const char *what)
{
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return db_error(s, what);
    return 0;
}

static int user_version(store *s, int *version)
{
    sqlite3_stmt *st;
    if (prepare(s, "PRAGMA user_version", &st) != 0)
        return -1;
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        *version = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 0 : db_error(s, "PRAGMA user_version");
}

/* Opens the database file with the given SQLite open flags and reads its
 * schema version, refusing one newer than this build knows. */
static int open_db(store **out, const char *path, int flags, int *version, char *err,
                   size_t errlen)
{
    *out = NULL;
    store *s = xcalloc(1, sizeof *s);

    if (sqlite3_open_v2(path, &s->db, flags, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "cannot open database %s: %s", path,
                 s->db ? sqlite3_errmsg(s->db) : "out of memory");
        goto fail;
    }
    sqlite3_busy_timeout(s->db, 5000);

    if (user_version(s, version) != 0) {
        snprintf(err, errlen, "cannot read database %s: %s", path, sqlite3_errmsg(s->db));
        goto fail;
    }
    if (*version > SCHEMA_VERSION) {
        snprintf(err, errlen, "database %s has schema version %d; this build supports up to %d",
                 path, *version, SCHEMA_VERSION);
        goto fail;
    }
    *out = s;
    return 0;

fail:
    sqlite3_close(s->db);
    free(s);
    return -1;
}

int store_open(store **out, const char *path, char *err, size_t errlen)
{
    *out = NULL;
    store *s;
    int version = 0;
    if (open_db(&s, path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, &version, err, errlen) != 0)
        return -1;

    if (version < SCHEMA_VERSION) {
        /* A new database gets the current schema; an older one is upgraded
         * in place, keeping everything it has recorded. */
        const char *sql = version == 0 ? SCHEMA_SQL : MIGRATE_1_TO_2_SQL;
        char pragma[64];
        snprintf(pragma, sizeof pragma, "PRAGMA user_version = %d", SCHEMA_VERSION);
        if (exec(s, "BEGIN IMMEDIATE") != 0 || exec(s, sql) != 0 || exec(s, pragma) != 0 ||
            exec(s, "COMMIT") != 0) {
            snprintf(err, errlen, "cannot %s %s: %s",
                     version == 0 ? "create the schema in" : "upgrade the schema of", path,
                     sqlite3_errmsg(s->db));
            sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
            goto fail;
        }
    }

    *out = s;
    return 0;

fail:
    sqlite3_close(s->db);
    free(s);
    return -1;
}

int store_open_readonly(store **out, const char *path, char *err, size_t errlen)
{
    *out = NULL;
    if (access(path, F_OK) != 0 && errno == ENOENT)
        return 1;

    store *s;
    int version = 0;
    if (open_db(&s, path, SQLITE_OPEN_READONLY, &version, err, errlen) != 0)
        return -1;
    /* Version 0 is a file with no schema in it yet. Older versions need no
     * upgrade to be read: what changed since is not in the jobs table. */
    if (version == 0) {
        store_close(s);
        return 1;
    }
    *out = s;
    return 0;
}

void store_close(store *s)
{
    if (!s)
        return;
    sqlite3_close(s->db);
    free(s);
}

int store_begin(store *s)
{
    return exec(s, "BEGIN IMMEDIATE");
}

int store_commit(store *s)
{
    return exec(s, "COMMIT");
}

void store_rollback(store *s)
{
    if (!sqlite3_get_autocommit(s->db))
        sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
}

int store_source_get(store *s, const char *name, source_state *out)
{
    memset(out, 0, sizeof *out);
    sqlite3_stmt *st;
    if (prepare(s,
                "SELECT seeded, last_success, failures, alerted, last_error, last_attempt "
                "FROM sources WHERE name = ?",
                &st) != 0)
        return -1;
    bind_text(st, 1, name);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        out->seeded = sqlite3_column_int(st, 0);
        out->last_success = sqlite3_column_int64(st, 1);
        out->failures = sqlite3_column_int(st, 2);
        out->alerted = sqlite3_column_int(st, 3);
        out->last_error = column_dup(st, 4);
        out->last_attempt = sqlite3_column_int64(st, 5);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        return db_error(s, "load source state");
    return 0;
}

void source_state_free(source_state *st)
{
    free(st->last_error);
    memset(st, 0, sizeof *st);
}

int store_source_success(store *s, const char *name, long long now)
{
    sqlite3_stmt *st;
    if (prepare(s,
                "INSERT INTO sources (name, seeded, last_success, last_attempt, failures, "
                "last_error) VALUES (?1, 1, ?2, ?2, 0, NULL) "
                "ON CONFLICT (name) DO UPDATE SET seeded = 1, last_success = ?2, "
                "last_attempt = ?2, failures = 0, last_error = NULL",
                &st) != 0)
        return -1;
    bind_text(st, 1, name);
    sqlite3_bind_int64(st, 2, now);
    return run(s, st, "record source success");
}

int store_source_failure(store *s, const char *name, const char *error, long long now,
                         int *failures)
{
    sqlite3_stmt *st;
    if (prepare(s,
                "INSERT INTO sources (name, failures, last_error, last_attempt) "
                "VALUES (?1, 1, ?2, ?3) "
                "ON CONFLICT (name) DO UPDATE SET failures = failures + 1, last_error = ?2, "
                "last_attempt = ?3",
                &st) != 0)
        return -1;
    bind_text(st, 1, name);
    bind_text(st, 2, error);
    sqlite3_bind_int64(st, 3, now);
    if (run(s, st, "record source failure") != 0)
        return -1;

    if (prepare(s, "SELECT failures FROM sources WHERE name = ?", &st) != 0)
        return -1;
    bind_text(st, 1, name);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        *failures = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW)
        return db_error(s, "read source failure count");
    return 0;
}

int store_source_set_alerted(store *s, const char *name, int alerted)
{
    sqlite3_stmt *st;
    if (prepare(s, "UPDATE sources SET alerted = ?2 WHERE name = ?1", &st) != 0)
        return -1;
    bind_text(st, 1, name);
    sqlite3_bind_int(st, 2, alerted ? 1 : 0);
    return run(s, st, "update source alert flag");
}

int store_job_exists(store *s, const char *source, const char *ext_id)
{
    sqlite3_stmt *st;
    if (prepare(s, "SELECT 1 FROM jobs WHERE source = ? AND ext_id = ?", &st) != 0)
        return -1;
    bind_text(st, 1, source);
    bind_text(st, 2, ext_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_ROW)
        return 1;
    if (rc == SQLITE_DONE)
        return 0;
    return db_error(s, "look up job");
}

int store_job_add(store *s, const char *source, const job *j, const char *fingerprint,
                  const char *status, long long now)
{
    sqlite3_stmt *st;
    if (prepare(s,
                "INSERT INTO jobs (source, ext_id, url, title, company, location, salary, tags, "
                "description, posted_at, fingerprint, status, first_seen) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                "ON CONFLICT (source, ext_id) DO NOTHING",
                &st) != 0)
        return -1;
    bind_text(st, 1, source);
    bind_text(st, 2, j->ext_id);
    bind_text(st, 3, j->url);
    bind_text(st, 4, j->title);
    bind_text(st, 5, j->company);
    bind_text(st, 6, j->location);
    bind_text(st, 7, j->salary);
    bind_text(st, 8, j->tags);
    bind_text(st, 9, j->description);
    bind_text(st, 10, j->posted_at);
    bind_text(st, 11, fingerprint);
    bind_text(st, 12, status);
    sqlite3_bind_int64(st, 13, now);
    if (run(s, st, "insert job") != 0)
        return -1;
    return sqlite3_changes(s->db) > 0 ? 1 : 0;
}

int store_fingerprint_recent(store *s, const char *fingerprint, long long since)
{
    sqlite3_stmt *st;
    if (prepare(s,
                "SELECT 1 FROM jobs WHERE fingerprint = ? AND first_seen >= ? "
                "AND status IN ('" JOB_PENDING "', '" JOB_NOTIFIED "') LIMIT 1",
                &st) != 0)
        return -1;
    bind_text(st, 1, fingerprint);
    sqlite3_bind_int64(st, 2, since);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_ROW)
        return 1;
    if (rc == SQLITE_DONE)
        return 0;
    return db_error(s, "look up fingerprint");
}

/* The columns read_job() expects, in its order. */
#define JOB_COLUMNS "id, source, ext_id, url, title, company, location, salary, tags, posted_at"
#define JOB_NCOLUMNS 10

/* Fills `e` from the current row of a statement that selects JOB_COLUMNS
 * first. The description is left NULL. */
static void read_job(sqlite3_stmt *st, queued_job *e)
{
    memset(e, 0, sizeof *e);
    e->id = sqlite3_column_int64(st, 0);
    e->source = column_dup(st, 1);
    e->j.ext_id = column_dup(st, 2);
    e->j.url = column_dup(st, 3);
    e->j.title = column_dup(st, 4);
    e->j.company = column_dup(st, 5);
    e->j.location = column_dup(st, 6);
    e->j.salary = column_dup(st, 7);
    e->j.tags = column_dup(st, 8);
    e->j.posted_at = column_dup(st, 9);
}

int store_queue_load(store *s, size_t limit, queued_job **out, size_t *n)
{
    *out = NULL;
    *n = 0;
    sqlite3_stmt *st;
    if (prepare(s,
                "SELECT " JOB_COLUMNS ", description FROM jobs WHERE status = '" JOB_PENDING "' "
                "ORDER BY id LIMIT ?",
                &st) != 0)
        return -1;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)limit);

    queued_job *q = NULL;
    size_t count = 0, cap = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (count == cap) {
            cap = cap ? cap * 2 : 16;
            q = xrealloc(q, cap * sizeof *q);
        }
        queued_job *e = &q[count++];
        read_job(st, e);
        e->j.description = column_dup(st, JOB_NCOLUMNS);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        store_queue_free(q, count);
        return db_error(s, "load notification queue");
    }
    *out = q;
    *n = count;
    return 0;
}

void store_queue_free(queued_job *q, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(q[i].source);
        job_free(&q[i].j);
    }
    free(q);
}

int store_jobs_latest(store *s, job_filter accept, void *ctx, size_t limit, queued_job **out,
                      size_t *n)
{
    *out = NULL;
    *n = 0;
    sqlite3_stmt *st;
    /* No LIMIT: rows are read newest first only until enough are accepted. */
    if (prepare(s,
                "SELECT " JOB_COLUMNS " FROM jobs WHERE status NOT IN ('" JOB_DUPLICATE "', '"
                JOB_UNDELIVERABLE "') ORDER BY id DESC",
                &st) != 0)
        return -1;

    queued_job *q = xcalloc(limit, sizeof *q);
    size_t count = 0;
    int rc = SQLITE_DONE;
    while (count < limit && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        queued_job *e = &q[count];
        read_job(st, e);
        if (accept(&e->j, ctx)) {
            count++;
        } else {
            free(e->source);
            job_free(&e->j);
        }
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        store_queue_free(q, count);
        return db_error(s, "load latest postings");
    }
    *out = q;
    *n = count;
    return 0;
}

int store_job_mark_notified(store *s, long long id, long long now)
{
    sqlite3_stmt *st;
    if (prepare(s, "UPDATE jobs SET status = '" JOB_NOTIFIED "', notified_at = ?2 WHERE id = ?1",
                &st) != 0)
        return -1;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, now);
    return run(s, st, "mark job notified");
}

int store_job_mark_undeliverable(store *s, long long id)
{
    sqlite3_stmt *st;
    if (prepare(s, "UPDATE jobs SET status = '" JOB_UNDELIVERABLE "' WHERE id = ?1", &st) != 0)
        return -1;
    sqlite3_bind_int64(st, 1, id);
    return run(s, st, "mark job undeliverable");
}

long long store_job_count(store *s, const char *status)
{
    sqlite3_stmt *st;
    if (prepare(s, "SELECT COUNT(*) FROM jobs WHERE status = ?", &st) != 0)
        return -1;
    bind_text(st, 1, status);
    long long count = -1;
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        count = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW)
        return db_error(s, "count jobs");
    return count;
}
