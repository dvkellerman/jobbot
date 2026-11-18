#!/data/data/com.termux/files/usr/bin/sh
# Runs jobbot periodically with Android's job scheduler, through Termux:API.
#
#   termux/schedule.sh start [MINUTES]   run jobbot about every MINUTES minutes
#                                        (default 15, which is also Android's minimum)
#   termux/schedule.sh stop              stop running it
#   termux/schedule.sh status            show the schedule and the latest log lines
#
# Needs the Termux:API app, and its commands: pkg install termux-api
#
# Each run is termux/run.sh: it checks the boards that are due and exits.
# How often each board is checked is set in ~/.jobbot/jobbot.conf; MINUTES is
# only how often jobbot gets the chance. The schedule survives reboots, and
# runs wait for a network connection.

set -eu

# Identifies this job to Android; scheduling again replaces the previous one.
JOB_ID=7107

REPO=$(cd "$(dirname "$0")/.." && pwd)
RUN=$REPO/termux/run.sh
BIN=$REPO/build/jobbot
CONF=$HOME/.jobbot/jobbot.conf
LOG=$HOME/.jobbot/jobbot.log

die() {
    echo "schedule.sh: $*" >&2
    exit 1
}

usage() {
    echo "Usage: termux/schedule.sh start [MINUTES] | stop | status" >&2
    exit 2
}

need_scheduler() {
    command -v termux-job-scheduler >/dev/null 2>&1 ||
        die "termux-job-scheduler not found. Install the Termux:API app, then run: pkg install termux-api"
}

# The scheduler's line for our job, or nothing when it is not scheduled.
pending_job() {
    termux-job-scheduler --pending 2>&1 | grep "Job $JOB_ID:" || true
}

start() {
    minutes=${1:-15}
    case $minutes in
    '' | *[!0-9]*) die "MINUTES must be a whole number, got '$minutes'" ;;
    esac
    if [ "$minutes" -lt 15 ] || [ "$minutes" -gt 1440 ]; then
        die "MINUTES must be between 15 (Android's minimum) and 1440, got $minutes"
    fi

    need_scheduler
    [ -x "$BIN" ] || die "$BIN not found. Build it first: make"
    [ -f "$CONF" ] ||
        die "no settings at $CONF. Copy config/jobbot.conf.example there and fill it in"
    # The scheduler refuses a script that is not executable.
    chmod u+x "$RUN"

    # By default a job waits while the battery is low. A mains-powered box may
    # report no battery or an empty one, so that condition is switched off.
    termux-job-scheduler --job-id "$JOB_ID" --script "$RUN" \
        --period-ms "$((minutes * 60000))" --persisted true --network any \
        --battery-not-low false

    [ -n "$(pending_job)" ] ||
        die "Android did not accept the job (see the output above); nothing is scheduled"
    echo
    echo "jobbot will run about every $minutes minutes, also after a reboot."
    echo "Each run is logged to $LOG"
}

stop() {
    need_scheduler
    if [ -z "$(pending_job)" ]; then
        echo "jobbot is not scheduled."
        return
    fi
    termux-job-scheduler --cancel --job-id "$JOB_ID"
    [ -z "$(pending_job)" ] || die "the job is still scheduled (see the output above)"
    echo "jobbot is no longer scheduled."
}

status() {
    need_scheduler
    job=$(pending_job)
    if [ -n "$job" ]; then
        echo "Scheduled: $job"
    else
        echo "jobbot is not scheduled. Start it with: termux/schedule.sh start"
    fi
    if [ -f "$LOG" ]; then
        echo
        echo "Latest lines of $LOG:"
        tail -n 15 "$LOG"
    else
        echo "No log yet at $LOG"
    fi
}

case ${1:-} in
start)
    [ $# -le 2 ] || usage
    start "${2:-15}"
    ;;
stop)
    [ $# -eq 1 ] || usage
    stop
    ;;
status)
    [ $# -eq 1 ] || usage
    status
    ;;
*) usage ;;
esac
