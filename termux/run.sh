#!/data/data/com.termux/files/usr/bin/sh
# One scheduled jobbot run. Android's job scheduler starts this script (see
# schedule.sh) with no arguments and no terminal.
#
# It checks the job boards that are due, sends what is new, and exits.
# It uses the program built in this repository, build/jobbot, and
#
#   ~/.jobbot/jobbot.conf   settings
#   ~/.jobbot/jobbot.log    what each run did
#
# Running it by hand does the same as a scheduled run.

HOME=${HOME:-/data/data/com.termux/files/home}
export HOME
PATH=/data/data/com.termux/files/usr/bin:${PATH:-}
export PATH

REPO=$(cd "$(dirname "$0")/.." && pwd)
BIN=$REPO/build/jobbot
DIR=$HOME/.jobbot
LOG=$DIR/jobbot.log

mkdir -p "$DIR" || exit 1

# Keep the log from growing without limit: past 1 MB, keep the newest quarter.
if [ -f "$LOG" ] && [ "$(wc -c <"$LOG")" -gt 1048576 ]; then
    tail -c 262144 "$LOG" | sed 1d >"$LOG.tmp" && mv "$LOG.tmp" "$LOG"
fi

if [ ! -x "$BIN" ]; then
    echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) error $BIN is missing; run make in $REPO" >>"$LOG"
    exit 1
fi

exec "$BIN" --config "$DIR/jobbot.conf" --due >>"$LOG" 2>&1
