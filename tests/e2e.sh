#!/usr/bin/env bash
# End-to-end test: runs the real jobbot binary against a local mock job
# board and a local mock Telegram API (tests/mock_server.py).
#
#   tests/e2e.sh [path-to-jobbot]     (default: build/san/jobbot)
#
# Needs python3 and the sqlite3 command-line tool.
set -u

BIN=${1:-build/san/jobbot}
DIR=$(mktemp -d)
LOG="$DIR/jobbot.log"
FAILED=0
CHECKS=0

python3 tests/mock_server.py "$DIR" &
SERVER=$!
cleanup() {
    kill "$SERVER" 2>/dev/null
    wait "$SERVER" 2>/dev/null
    if [ "$FAILED" -ne 0 ]; then
        echo "---- jobbot log ----"
        cat "$LOG" 2>/dev/null
    fi
    rm -rf "$DIR"
}
trap cleanup EXIT

for _ in $(seq 1 150); do
    [ -s "$DIR/port" ] && break
    sleep 0.1
done
PORT=$(cat "$DIR/port" 2>/dev/null)
if [ -z "$PORT" ]; then
    echo "mock server did not start"
    exit 1
fi

write_config() { # $1: bot token line
    cat >"$DIR/jobbot.conf" <<EOF
[general]
db_path = $DIR/jobbot.db
poll_interval_minutes = 5
alert_after_failures = 3

[telegram]
$1
chat_id = 4242
api_base = http://127.0.0.1:$PORT/

[match]
include = ios, swift
exclude = qa

[source:board]
type = rss
url = http://127.0.0.1:$PORT/feed.xml
label = Board
country = UA
EOF
}
write_config "bot_token = 123456:TEST_token-abc"

ITEMS=()
publish() { # appends "id|title" items to the board's feed
    ITEMS+=("$@")
    {
        echo '<?xml version="1.0" encoding="utf-8"?><rss version="2.0"><channel><title>t</title>'
        for it in ${ITEMS[@]+"${ITEMS[@]}"}; do
            echo "<item><title>${it#*|}</title><link>https://jobs.example/${it%%|*}</link><guid>${it%%|*}</guid></item>"
        done
        echo '</channel></rss>'
    } >"$DIR/feed.xml"
}

mode() { echo "$1" >"$DIR/telegram_mode"; }
board() { echo "$1" >"$DIR/board_status"; }

run() { # runs jobbot with the test config; prints its exit status
    "$BIN" --config "$DIR/jobbot.conf" "$@" >>"$LOG" 2>&1
    echo $?
}

# Evaluates a Python expression over the recorded Telegram requests:
#   msgs  every request, ok  the accepted ones (each: path, outcome, message)
tg() {
    python3 -c '
import json, os, sys
path = sys.argv[1]
msgs = [json.loads(l) for l in open(path, encoding="utf-8")] if os.path.exists(path) else []
ok = [m for m in msgs if m["outcome"] == "accepted"]
print(eval(sys.argv[2]))' "$DIR/telegram.jsonl" "$1"
}

status_of() { # job status in the database, by posting id
    sqlite3 "$DIR/jobbot.db" "SELECT status FROM jobs WHERE ext_id = '$1'"
}

check() { # description, actual, expected
    CHECKS=$((CHECKS + 1))
    if [ "$2" != "$3" ]; then
        FAILED=$((FAILED + 1))
        printf 'FAIL: %s\n  actual:   %s\n  expected: %s\n' "$1" "$2" "$3"
    fi
}

echo "1. first check only records what is already listed"
publish "a|iOS Developer" "b|Java Developer"
check "exit status" "$(run --once)" 0
check "messages sent" "$(tg 'len(msgs)')" 0
check "a is seeded" "$(status_of a)" seeded

echo "2. a new matching posting is sent once, with a link button"
publish "c|Senior Swift Engineer" "d|Android Developer" "e|iOS QA Engineer"
check "exit status" "$(run --once)" 0
check "messages sent" "$(tg 'len(ok)')" 1
check "request path" "$(tg 'ok[-1]["path"]')" "/bot123456:TEST_token-abc/sendMessage"
check "chat id" "$(tg 'ok[-1]["message"]["chat_id"]')" 4242
check "text" "$(tg 'ok[-1]["message"]["text"]')" "$(printf '<b>Senior Swift Engineer</b>\nBoard · UA')"
check "parse mode" "$(tg 'ok[-1]["message"]["parse_mode"]')" HTML
check "button" "$(tg 'ok[-1]["message"]["reply_markup"]["inline_keyboard"][0][0]["url"]')" "https://jobs.example/c"
check "c is notified" "$(status_of c)" notified
check "d is filtered out" "$(status_of d)" unmatched
check "e is excluded" "$(status_of e)" unmatched

echo "3. running again (a restart) sends nothing twice"
check "exit status" "$(run --once)" 0
check "messages sent" "$(tg 'len(msgs)')" 1

echo "4. Telegram unavailable: the posting waits and goes out once later"
mode down
publish "f|iOS Lead"
check "exit status" "$(run --once)" 1
check "one attempt, no blind retry" "$(tg 'len([m for m in msgs if m["outcome"] == "502"])')" 1
check "f is still queued" "$(status_of f)" pending
mode ok
check "exit status" "$(run --once)" 0
check "messages sent" "$(tg 'len(ok)')" 2
check "text" "$(tg 'ok[-1]["message"]["text"]')" "$(printf '<b>iOS Lead</b>\nBoard · UA')"
check "exit status" "$(run --once)" 0
check "not sent again" "$(tg 'len(ok)')" 2

echo "5. rate limit: waits as told, then delivers once"
mode rate_limit:2
publish "g|Swift Dev"
check "exit status" "$(run --once)" 0
check "rate-limited attempts" "$(tg 'len([m for m in msgs if m["outcome"] == "429"])')" 2
check "messages sent" "$(tg 'len(ok)')" 3
check "g is notified" "$(status_of g)" notified

echo "6. formatted message rejected: falls back to plain text with the link"
mode reject_html
publish "h|iOS Dev &amp; More"
check "exit status" "$(run --once)" 0
check "messages sent" "$(tg 'len(ok)')" 4
check "plain text" "$(tg 'ok[-1]["message"]["text"]')" "$(printf 'iOS Dev & More\nBoard · UA\nhttps://jobs.example/h')"
check "no parse mode" "$(tg '"parse_mode" in ok[-1]["message"]')" False
check "no button" "$(tg '"reply_markup" in ok[-1]["message"]')" False

echo "7. one posting Telegram will not accept does not block the others"
mode reject_jobs
publish "i|iOS REJECTME" "j|iOS Fine"
check "exit status" "$(run --once)" 0
check "messages sent" "$(tg 'len(ok)')" 6
check "notice about the bad posting" "$(tg '"could not be shown" in ok[-2]["message"]["text"]')" True
check "next posting delivered" "$(tg 'ok[-1]["message"]["text"]')" "$(printf '<b>iOS Fine</b>\nBoard · UA')"
check "i is set aside" "$(status_of i)" undeliverable
check "j is notified" "$(status_of j)" notified

echo "8. chat unreachable: nothing is dropped"
mode no_chat
publish "k|iOS Later"
check "exit status" "$(run --once)" 1
check "messages sent" "$(tg 'len(ok)')" 6
check "k is still queued" "$(status_of k)" pending
mode ok
check "exit status" "$(run --once)" 0
check "messages sent" "$(tg 'len(ok)')" 7
check "k is notified" "$(status_of k)" notified

echo "9. a failing board is reported once, and again when it recovers"
board 503
check "exit status" "$(run --once)" 1
check "exit status" "$(run --once)" 1
check "no alert before the threshold" "$(tg 'len(ok)')" 7
check "exit status" "$(run --once)" 1
check "alert sent" "$(tg 'len(ok)')" 8
check "alert names the source" "$(tg '"source Board · UA (board) has failed 3 times in a row" in ok[-1]["message"]["text"]')" True
check "alert gives the error" "$(tg '"HTTP 503" in ok[-1]["message"]["text"]')" True
check "exit status" "$(run --once)" 1
check "alert not repeated" "$(tg 'len(ok)')" 8
board 200
check "exit status" "$(run --once)" 0
check "recovery notice" "$(tg 'ok[-1]["message"]["text"]')" "jobbot: source Board · UA (board) is working again."
check "exit status" "$(run --once)" 0
check "recovery not repeated" "$(tg 'len(ok)')" 9

echo "10. an empty board counts as a failed check"
cp "$DIR/feed.xml" "$DIR/feed.bak"
echo '<rss version="2.0"><channel><title>t</title></channel></rss>' >"$DIR/feed.xml"
check "exit status" "$(run --once)" 1
check "reason logged" "$(grep -c 'returned no postings' "$LOG")" 1
cp "$DIR/feed.bak" "$DIR/feed.xml"
check "exit status" "$(run --once)" 0

echo "11. --dry-run prints instead of sending and leaves the queue alone"
publish "m|iOS Dry"
check "exit status" "$(run --once --dry-run)" 0
check "printed" "$(grep -c '^iOS Dry$' "$LOG")" 1
check "nothing sent" "$(tg 'len(ok)')" 9
check "m is still queued" "$(status_of m)" pending
check "exit status" "$(run --once)" 0
check "then sent for real" "$(tg 'ok[-1]["message"]["text"]')" "$(printf '<b>iOS Dry</b>\nBoard · UA')"

echo "12. --check-sources reports without sending or storing"
before=$(sqlite3 "$DIR/jobbot.db" 'SELECT COUNT(*) FROM jobs')
publish "n|iOS Unseen"
out=$("$BIN" --config "$DIR/jobbot.conf" --check-sources 2>>"$LOG")
check "exit status" "$?" 0
check "summary line" "$(echo "$out" | grep -c 'board  *OK  *13 postings, 10 match the filter')" 1
check "nothing sent" "$(tg 'len(ok)')" 10
check "nothing stored" "$(sqlite3 "$DIR/jobbot.db" 'SELECT COUNT(*) FROM jobs')" "$before"

echo "13. service mode: announces itself, delivers, stops cleanly on SIGTERM"
"$BIN" --config "$DIR/jobbot.conf" >>"$LOG" 2>&1 &
PID=$!
for _ in $(seq 1 100); do
    [ "$(tg 'len(ok)')" = 12 ] && break
    sleep 0.1
done
check "start-up message" "$(tg 'ok[-2]["message"]["text"]')" "jobbot started. Watching 1 source; new matching postings will appear here."
check "posting delivered" "$(tg 'ok[-1]["message"]["text"]')" "$(printf '<b>iOS Unseen</b>\nBoard · UA')"
kill -TERM "$PID"
start=$(date +%s)
wait "$PID"
check "exit status" "$?" 0
check "stopped within 3s" "$([ $(($(date +%s) - start)) -le 3 ] && echo yes || echo no)" yes
check "logged the stop" "$(grep -c 'jobbot stopped' "$LOG")" 1

echo "14. a wrong token stops the service at start-up with a clear error"
mode unauthorized
check "exit status" "$(run)" 2
check "error shown" "$(grep -c 'Telegram refused the start-up message: HTTP 401: Unauthorized' "$LOG")" 1
mode ok

failures_of() { sqlite3 "$DIR/jobbot.db" "SELECT failures FROM sources WHERE name = 'board'"; }
age() { # makes the board's last check look $1 seconds older
    sqlite3 "$DIR/jobbot.db" "UPDATE sources SET last_attempt = last_attempt - $1 WHERE name = 'board'"
}

echo "15. --due leaves a source alone until its interval (5 min here) has passed"
check "exit status" "$(run --once)" 0
sent=$(tg 'len(ok)')
publish "p|iOS Due"
check "exit status" "$(run --due)" 0
check "board not read" "$(status_of p)" ""
check "nothing sent" "$(tg 'len(ok)')" "$sent"
check "said so" "$(grep -c 'checked 0 of 1 sources' "$LOG")" 1
age 200
check "exit status" "$(run --due)" 0
check "still too early" "$(status_of p)" ""
age 100
check "exit status" "$(run --due)" 0
check "read once the interval has passed" "$(status_of p)" notified
check "sent" "$(tg 'ok[-1]["message"]["text"]')" "$(printf '<b>iOS Due</b>\nBoard · UA')"
check "exit status" "$(run --due)" 0
check "not sent again" "$(tg 'len(ok)')" "$((sent + 1))"

echo "16. --due retries a failing source less and less often"
board 503
age 300
check "first failure" "$(run --due)" 1
check "failures" "$(failures_of)" 1
check "not retried at once" "$(run --due)" 0
check "failures" "$(failures_of)" 1
age 300
check "retried after one interval" "$(run --due)" 1
check "failures" "$(failures_of)" 2
age 300
check "not after one more interval" "$(run --due)" 0
check "failures" "$(failures_of)" 2
age 300
board 200
check "but after two" "$(run --due)" 0
check "failures" "$(failures_of)" 0
check "no alert below the threshold" "$(tg 'len(ok)')" "$((sent + 1))"

echo "17. a second jobbot on the same database stands aside"
"$BIN" --config "$DIR/jobbot.conf" >>"$LOG" 2>&1 &
PID=$!
for _ in $(seq 1 100); do
    [ "$(tg 'len(ok)')" = "$((sent + 2))" ] && break
    sleep 0.1
done
check "first one is running" "$(tg 'ok[-1]["message"]["text"][:14]')" "jobbot started"
publish "q|iOS Twice"
check "second exits" "$(run --once)" 1
check "and says why" "$(grep -c 'another jobbot is already using' "$LOG")" 1
check "without touching the board" "$(status_of q)" ""
check "--due too" "$(run --due)" 1
kill -TERM "$PID"
wait "$PID"
check "first stopped cleanly" "$?" 0
check "then it can run" "$(run --once)" 0
check "posting sent once" "$(tg 'len([m for m in ok if "iOS Twice" in m["message"]["text"]])')" 1

echo "18. without --config, settings and database are found under ~/.jobbot"
mkdir -p "$DIR/home/.jobbot"
sed 's#^db_path = .*#db_path = ~/.jobbot/jobbot.db#' "$DIR/jobbot.conf" >"$DIR/home/.jobbot/jobbot.conf"
check "exit status" "$(HOME="$DIR/home" "$BIN" --once >>"$LOG" 2>&1; echo $?)" 0
check "database created there" "$([ -s "$DIR/home/.jobbot/jobbot.db" ] && echo yes || echo no)" yes
check "no HOME, no default" "$(env -u HOME "$BIN" --once >>"$LOG" 2>&1; echo $?)" 2
check "message" "$(grep -c 'HOME is not set' "$LOG")" 1
check "no HOME, ~ in db_path" "$(env -u HOME "$BIN" --config "$DIR/home/.jobbot/jobbot.conf" --once >>"$LOG" 2>&1; echo $?)" 2
check "message" "$(grep -c "'db_path' in \[general\] starts with ~/ but HOME is not set" "$LOG")" 1

echo "19. --test-telegram sends the newest matching postings as one message marked as a test"
base=$(tg 'len(ok)')
db_state() { cksum <"$DIR/jobbot.db"; }
statuses() { sqlite3 "$DIR/jobbot.db" "SELECT group_concat(ext_id || '=' || status) FROM (SELECT ext_id, status FROM jobs ORDER BY id)"; }
state_before=$(db_state)
statuses_before=$(statuses)
check "exit status" "$(run --test-telegram)" 0
check "one message" "$(tg 'len(ok)')" "$((base + 1))"
check "marked as a test" "$(tg 'ok[-1]["message"]["text"].split("\n")[0]')" "<b>TEST MESSAGE from jobbot</b>"
check "says what it is" "$(tg '"jobbot --test-telegram" in ok[-1]["message"]["text"].split("\n")[1]')" True
check "four postings, newest first" "$(tg '"\n\n".join(ok[-1]["message"]["text"].split("\n\n")[1:])')" \
    "$(printf '<b>iOS Twice</b>\nBoard · UA\n\n<b>iOS Due</b>\nBoard · UA\n\n<b>iOS Unseen</b>\nBoard · UA\n\n<b>iOS Dry</b>\nBoard · UA')"
check "each as its real notification was" \
    "$(tg 'ok[-1]["message"]["text"].split("\n\n")[1] == [m for m in ok if "iOS Twice" in m["message"]["text"]][0]["message"]["text"]')" True
check "parse mode" "$(tg 'ok[-1]["message"]["parse_mode"]')" HTML
check "a link button for each" "$(tg '" ".join(r[0]["text"] + "=" + r[0]["url"] for r in ok[-1]["message"]["reply_markup"]["inline_keyboard"])')" \
    "Open posting 1=https://jobs.example/q Open posting 2=https://jobs.example/p Open posting 3=https://jobs.example/n Open posting 4=https://jobs.example/m"
check "outcome printed" "$(grep -c 'Test message sent to chat 4242' "$LOG")" 1
check "database not written to" "$(db_state)" "$state_before"
check "exit status" "$(run --once)" 0
check "nothing sent again because of it" "$(tg 'len(ok)')" "$((base + 1))"
check "--dry-run prints it" "$(run --test-telegram --dry-run)" 0
check "printed" "$(grep -c '^TEST MESSAGE from jobbot$' "$LOG")" 1
check "nothing sent" "$(tg 'len(ok)')" "$((base + 1))"
mode reject_html
check "formatted message rejected" "$(run --test-telegram)" 0
check "resent as plain text with the links" "$(tg 'ok[-1]["message"]["text"].split("\n\n")[1]')" "$(printf 'iOS Twice\nBoard · UA\nhttps://jobs.example/q')"
check "still marked as a test" "$(tg 'ok[-1]["message"]["text"].split("\n")[0]')" "TEST MESSAGE from jobbot"
check "no buttons" "$(tg '"reply_markup" in ok[-1]["message"]')" False
mode down
check "Telegram unavailable" "$(run --test-telegram)" 1
check "reason shown" "$(grep -c 'test message not sent: HTTP 502' "$LOG")" 1
mode unauthorized
check "wrong token" "$(run --test-telegram)" 2
check "reason shown" "$(grep -c 'Telegram refused the test message: HTTP 401: Unauthorized' "$LOG")" 1
mode no_chat
check "wrong chat" "$(run --test-telegram)" 2
check "reason shown" "$(grep -c 'Telegram refused the test message: HTTP 400: Bad Request: chat not found' "$LOG")" 1
mode ok
check "not together with a check of the boards" "$(run --test-telegram --once)" 2
check "nor with --check-sources" "$(run --test-telegram --check-sources)" 2
check "nothing more sent" "$(tg 'len(ok)')" "$((base + 2))"
"$BIN" --config "$DIR/jobbot.conf" >>"$LOG" 2>&1 &
PID=$!
for _ in $(seq 1 100); do
    [ "$(tg 'len(ok)')" = "$((base + 3))" ] && break
    sleep 0.1
done
check "a jobbot is running" "$(tg 'ok[-1]["message"]["text"][:14]')" "jobbot started"
check "works next to it" "$(run --test-telegram)" 0
check "sent" "$(tg 'ok[-1]["message"]["text"].split("\n")[0]')" "<b>TEST MESSAGE from jobbot</b>"
kill -TERM "$PID"
wait "$PID"
check "the other one stopped cleanly" "$?" 0
check "no posting changed status" "$(statuses)" "$statuses_before"
sed "s#^db_path = .*#db_path = $DIR/fresh.db#" "$DIR/jobbot.conf" >"$DIR/fresh.conf"
check "no database yet" "$("$BIN" --config "$DIR/fresh.conf" --test-telegram >>"$LOG" 2>&1; echo $?)" 0
check "still a test message" "$(tg '"no matching postings yet" in ok[-1]["message"]["text"]')" True
check "without postings or buttons" "$(tg '"reply_markup" in ok[-1]["message"]')" False
check "no database created" "$(ls "$DIR" | grep -c '^fresh\.db')" 0

echo "20. configuration mistakes are reported, not ignored"
write_config "bot_token ="
check "missing token" "$(run --once)" 2
check "message" "$(grep -c 'no Telegram bot token' "$LOG")" 1
write_config "bot_token = not-a-token"
check "malformed token" "$(run --once)" 2
write_config "$(printf 'bot_token = 1:a\nbot_tokne = 1:a')"
check "unknown setting" "$(run --once)" 2
check "message" "$(grep -c "unknown setting 'bot_tokne' in \[telegram\]" "$LOG")" 1
check "missing file" "$("$BIN" --config "$DIR/nope.conf" --once >>"$LOG" 2>&1; echo $?)" 2

echo "21. the token never appears in the log"
check "token occurrences" "$(grep -c 'TEST_token-abc' "$LOG")" 0

echo "22. no sanitizer reports"
check "sanitizer errors" "$(grep -c -E 'AddressSanitizer|runtime error|LeakSanitizer' "$LOG")" 0

echo "$CHECKS checks, $FAILED failed"
[ "$FAILED" -eq 0 ]
