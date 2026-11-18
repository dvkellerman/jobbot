# jobbot

A small program, written in C, that watches job boards for iOS / macOS /
React Native openings and sends each new matching posting to a Telegram chat,
with a button that opens the posting.

It is set up to run on an Android box under [Termux](https://termux.dev),
started every 15 minutes by Android's job scheduler. It also builds and runs
on Linux and macOS.

## Sources

| Country      | Boards                                  |
| ------------ | --------------------------------------- |
| Ukraine      | DOU, Djinni (RSS)                       |
| Poland       | NoFluffJobs, JustJoin.it                |
| South Africa | PNet                                    |
| Any          | LinkedIn public search, Adzuna API      |

LinkedIn is off by default: its terms of use do not allow automated access.
Adzuna needs a free API key and has not been run against the live API yet.
India is covered only by those two.

## Build

Needs a C11 compiler, `make`, and the development files for libcurl, libxml2
and SQLite. cJSON is vendored.

```sh
# Termux
pkg install clang make pkg-config libcurl libxml2 libsqlite

# Debian / Ubuntu
sudo apt install build-essential pkg-config libcurl4-openssl-dev libxml2-dev libsqlite3-dev

make          # builds build/jobbot
make check    # unit tests and an end-to-end test (needs python3 and sqlite3)
```

On macOS the SDK already provides the three libraries.

## Configure

Settings, the database and the log live together in `~/.jobbot`:

```sh
mkdir -p ~/.jobbot
cp config/jobbot.conf.example ~/.jobbot/jobbot.conf
chmod 600 ~/.jobbot/jobbot.conf
```

Then fill in the `[telegram]` section of `~/.jobbot/jobbot.conf`:

- `bot_token`: from [@BotFather](https://t.me/BotFather). It can be given in
  the `JOBBOT_TELEGRAM_TOKEN` environment variable instead.
- `chat_id`: your numeric id (ask [@userinfobot](https://t.me/userinfobot)).
  Press Start in a chat with your bot once, or it cannot message you.

The same file sets the keywords to look for and the boards to watch; every
setting is explained there.

## Try it

```sh
build/jobbot --check-sources   # show what each board returns; sends nothing
build/jobbot --test-telegram   # send one test message to your chat, then exit
build/jobbot                   # sends "jobbot started" to Telegram, then keeps running
```

The second command proves the token and chat id work. Its message starts
with **TEST MESSAGE from jobbot** and then shows the four newest postings in
the database that pass your keyword filter, each formatted as a real
notification is, with a numbered "Open posting" button for each. Before the first
check of the boards there are none, and the message says so. It only reads
the database: nothing is marked as sent or sent again later, and it can be
run while another jobbot is working. With `--dry-run` it prints the message
instead of sending it.

The third command also sends a message when it starts; stop it with Ctrl+C.

## Run on a schedule (Termux)

Install the Termux:API app and its commands (`pkg install termux-api`), then:

```sh
termux/schedule.sh start     # run jobbot about every 15 minutes, also after a reboot
termux/schedule.sh status    # show the schedule and the latest log lines
termux/schedule.sh stop      # stop running it
```

Each run checks the boards whose interval has passed, sends what is new and
exits; what it did is appended to `~/.jobbot/jobbot.log`. The first check of
each board only records what is already listed. You are notified about
postings that appear after that, once each.

## Other ways to run

```sh
build/jobbot --due                # what a scheduled run does
build/jobbot --once               # check every board now, whatever its interval
build/jobbot --once --dry-run     # the same, printing instead of sending
build/jobbot                      # keep running and check each board at its interval
```

Only one jobbot uses the database at a time; a second one started meanwhile
exits and leaves the work to the first.

## Layout

```
include/   headers
src/       program sources; src/sources/ has one file per job board
tests/     unit tests, saved board responses, end-to-end test
termux/    scheduled run and its set-up for Termux
vendor/    cJSON
```

## License

MIT, see [LICENSE](LICENSE). The vendored cJSON is also MIT licensed, see
`vendor/cjson/LICENSE`.
