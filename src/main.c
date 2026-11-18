#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include <libxml/parser.h>

#include "app.h"
#include "util.h"

/* Telegram delivery retry delay: doubles from the first value up to the cap. */
#define DELIVERY_RETRY_MIN_S 60L
#define DELIVERY_RETRY_MAX_S 900L
/* The main loop wakes at least this often to notice a shutdown request. */
#define MAX_SLEEP_S 60L

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: jobbot [options]\n"
            "\n"
            "Watches job boards and sends new matching postings to a Telegram chat.\n"
            "\n"
            "Options:\n"
            "  -c, --config PATH    configuration file (default: " JOBBOT_DEFAULT_CONFIG ")\n"
            "      --check-sources  fetch every source once and print what came back and how\n"
            "                       it would be filtered; uses neither the database nor Telegram\n"
            "      --once           check every source once, send what is queued, then exit\n"
            "      --due            like --once, but check only the sources whose interval has\n"
            "                       passed since their last check; for running from a scheduler\n"
            "      --test-telegram  send one message, marked as a test, that shows the newest\n"
            "                       matching postings in the database as real notifications\n"
            "                       look, then exit; checks no board and changes nothing\n"
            "      --dry-run        print messages instead of sending them to Telegram; queued\n"
            "                       postings stay queued\n"
            "  -v, --verbose        log at debug level\n"
            "  -h, --help           show this help\n"
            "      --version        show the version\n"
            "\n"
            "Without --once, --due, --check-sources or --test-telegram, jobbot keeps running\n"
            "and checks each source at its interval.\n"
            "\n"
            "Exit status: 0 on success, 1 on a failure that may pass (a later run can succeed),\n"
            "2 when the configuration needs fixing, including a Telegram token or chat id\n"
            "that Telegram refuses.\n");
}

static void on_signal(int sig)
{
    (void)sig;
    stop_request();
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* Seconds until a source is next checked while jobbot keeps running: its
 * delay plus up to 10% jitter, so the checks do not land on a fixed beat. */
static long next_delay(const source_def *src, int failures)
{
    long delay = app_source_delay(src, failures);
    return delay + rand() % (delay / 10 + 1);
}

/* --once and --due: one pass over the sources, then one delivery round.
 * With `only_due`, sources checked more recently than their interval (or
 * their retry delay, while failing) are left alone. */
static int run_once(app *a, int only_due)
{
    int problems = 0;
    size_t checked = 0;
    long long started = (long long)time(NULL);

    for (size_t i = 0; i < a->cfg.nsources && !stop_requested(); i++) {
        const source_def *src = &a->cfg.sources[i];
        if (only_due) {
            source_state st;
            if (store_source_get(a->db, src->name, &st) != 0) {
                problems++;
                continue;
            }
            int due = app_source_is_due(src, &st, started);
            source_state_free(&st);
            if (!due) {
                log_debug("source %s: not due yet", src->name);
                continue;
            }
        }
        checked++;
        if (app_poll_source(a, src) != 0)
            problems++;
    }
    if (!stop_requested() && app_deliver(a) != 0)
        problems++;
    if (only_due && checked == a->cfg.nsources)
        log_info("checked all %zu sources", checked);
    else if (only_due)
        log_info("checked %zu of %zu sources; the others are not due yet", checked,
                 a->cfg.nsources);
    return problems ? 1 : 0;
}

/* Makes this process the only jobbot using the database, so that two runs
 * that overlap (a scheduler does not wait for the previous run to finish)
 * never send the same posting twice. The lock is a file next to the
 * database and is released when the process exits, however it exits.
 *
 * Returns the lock's descriptor, to be kept open; -1 when another jobbot
 * holds the lock; -2 when the lock file cannot be used, with a message in
 * `err`. */
static int lock_database(const char *db_path, char *err, size_t errlen)
{
    buf path;
    buf_init(&path);
    buf_printf(&path, "%s.lock", db_path);

    int fd = open(path.data, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        snprintf(err, errlen, "cannot create %s: %s", path.data, strerror(errno));
        buf_free(&path);
        return -2;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int busy = errno == EWOULDBLOCK;
        if (!busy)
            snprintf(err, errlen, "cannot lock %s: %s", path.data, strerror(errno));
        close(fd);
        buf_free(&path);
        return busy ? -1 : -2;
    }
    buf_free(&path);
    return fd;
}

static int run_daemon(app *a)
{
    size_t n = a->cfg.nsources;
    long long *due = xcalloc(n, sizeof *due); /* 0: every source is due at once */
    int need_delivery = 1;                    /* postings may be queued from a previous run */
    long long delivery_due = 0;
    int delivery_failures = 0;

    log_info("jobbot %s started: %zu sources, default check interval %ld min", JOBBOT_VERSION, n,
             a->cfg.poll_interval_s / 60);

    while (!stop_requested()) {
        for (size_t i = 0; i < n && !stop_requested(); i++) {
            if (due[i] > (long long)time(NULL))
                continue;
            int failures = app_poll_source(a, &a->cfg.sources[i]);
            /* A result that could not be recorded is retried after one interval. */
            due[i] = (long long)time(NULL) + next_delay(&a->cfg.sources[i], failures > 0 ? failures : 0);
            need_delivery = 1;
        }

        long long now = (long long)time(NULL);
        if (need_delivery && delivery_due <= now && !stop_requested()) {
            if (app_deliver(a) == 0) {
                need_delivery = 0;
                delivery_failures = 0;
            } else {
                long wait = DELIVERY_RETRY_MIN_S;
                for (int i = 0; i < delivery_failures && wait < DELIVERY_RETRY_MAX_S; i++)
                    wait *= 2;
                if (wait > DELIVERY_RETRY_MAX_S)
                    wait = DELIVERY_RETRY_MAX_S;
                delivery_failures++;
                delivery_due = (long long)time(NULL) + wait;
            }
        }

        now = (long long)time(NULL);
        long long next = now + MAX_SLEEP_S;
        for (size_t i = 0; i < n; i++)
            if (due[i] < next)
                next = due[i];
        if (need_delivery && delivery_due < next)
            next = delivery_due;
        long wait = (long)(next - now);
        sleep_seconds(wait < 1 ? 1 : wait);
    }

    free(due);
    log_info("jobbot stopped");
    return 0;
}

int main(int argc, char **argv)
{
    enum { OPT_CHECK = 1000, OPT_ONCE, OPT_DUE, OPT_TEST_TELEGRAM, OPT_DRY_RUN, OPT_VERSION };
    static const struct option options[] = {
        {"config", required_argument, NULL, 'c'},
        {"check-sources", no_argument, NULL, OPT_CHECK},
        {"once", no_argument, NULL, OPT_ONCE},
        {"due", no_argument, NULL, OPT_DUE},
        {"test-telegram", no_argument, NULL, OPT_TEST_TELEGRAM},
        {"dry-run", no_argument, NULL, OPT_DRY_RUN},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, OPT_VERSION},
        {NULL, 0, NULL, 0},
    };

    const char *config_arg = NULL;
    int check = 0, once = 0, only_due = 0, test_telegram = 0, dry_run = 0, verbose = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "c:vh", options, NULL)) != -1) {
        switch (opt) {
        case 'c':
            config_arg = optarg;
            break;
        case OPT_CHECK:
            check = 1;
            break;
        case OPT_ONCE:
            once = 1;
            break;
        case OPT_DUE:
            once = 1;
            only_due = 1;
            break;
        case OPT_TEST_TELEGRAM:
            test_telegram = 1;
            break;
        case OPT_DRY_RUN:
            dry_run = 1;
            break;
        case 'v':
            verbose = 1;
            break;
        case 'h':
            usage(stdout);
            return 0;
        case OPT_VERSION:
            printf("jobbot %s\n", JOBBOT_VERSION);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "jobbot: unexpected argument '%s'\n", argv[optind]);
        usage(stderr);
        return 2;
    }
    if (test_telegram && (check || once)) {
        fprintf(stderr, "jobbot: --test-telegram cannot be combined with --once, --due or "
                        "--check-sources\n");
        return 2;
    }

    char *config_path = config_arg ? xstrdup(config_arg) : path_expand_home(JOBBOT_DEFAULT_CONFIG);
    if (!config_path) {
        fprintf(stderr, "jobbot: HOME is not set, so the default configuration file "
                        JOBBOT_DEFAULT_CONFIG " cannot be found; use --config PATH\n");
        return 2;
    }

    app a;
    memset(&a, 0, sizeof a);
    a.dry_run = dry_run;

    char err[512] = "";
    int need_telegram = !check && !dry_run;
    int loaded = settings_load(&a.cfg, config_path, need_telegram, err, sizeof err);
    free(config_path);
    if (loaded != 0) {
        fprintf(stderr, "jobbot: %s\n", err);
        return 2;
    }
    log_set_level(verbose ? LOG_DEBUG : a.cfg.log_level);
    a.tg.api_base = a.cfg.tg_api_base;
    a.tg.bot_token = a.cfg.tg_bot_token;
    a.tg.chat_id = a.cfg.tg_chat_id;

    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    install_signal_handlers();
    xmlInitParser();
    if (http_global_init() != 0) {
        fprintf(stderr, "jobbot: cannot initialise the HTTP library\n");
        settings_free(&a.cfg);
        return 1;
    }

    int rc = 1;
    int lock_fd = -1;
    a.http = http_client_new(a.cfg.user_agent);
    if (!a.http) {
        fprintf(stderr, "jobbot: cannot create an HTTP client\n");
        goto done;
    }

    if (check) {
        rc = app_check_sources(&a) ? 1 : 0;
        goto done;
    }

    /* The test only reads the database, so it does not need the lock and
     * can run while another jobbot is at work. */
    if (test_telegram) {
        if (store_open_readonly(&a.db, a.cfg.db_path, err, sizeof err) < 0) {
            fprintf(stderr, "jobbot: %s\n", err);
            goto done;
        }
        rc = app_test_telegram(&a);
        goto done;
    }

    lock_fd = lock_database(a.cfg.db_path, err, sizeof err);
    if (lock_fd == -1) {
        fprintf(stderr, "jobbot: another jobbot is already using %s; leaving it to that one\n",
                a.cfg.db_path);
        goto done;
    }
    if (lock_fd < 0) {
        fprintf(stderr, "jobbot: %s\n", err);
        goto done;
    }

    if (store_open(&a.db, a.cfg.db_path, err, sizeof err) != 0) {
        fprintf(stderr, "jobbot: %s\n", err);
        goto done;
    }

    if (once) {
        rc = run_once(&a, only_due);
        goto done;
    }

    /* Announce the start. Besides being a sign of life, this fails fast on
     * a wrong token or chat id instead of quietly never delivering. */
    tg_result hello = app_send_startup(&a);
    if (hello.status == TG_FATAL || hello.status == TG_REJECTED) {
        fprintf(stderr,
                "jobbot: Telegram refused the start-up message: %s\n"
                "Check bot_token and chat_id, and that you have pressed Start in a chat with the bot.\n",
                hello.error);
        rc = 2; /* a set-up problem: restarting would not help */
        goto done;
    }
    if (hello.status == TG_RETRY)
        log_warn("telegram: start-up message not sent, carrying on: %s", hello.error);

    rc = run_daemon(&a);

done:
    store_close(a.db);
    if (lock_fd >= 0)
        close(lock_fd);
    http_client_free(a.http);
    http_global_cleanup();
    xmlCleanupParser();
    settings_free(&a.cfg);
    return rc;
}
