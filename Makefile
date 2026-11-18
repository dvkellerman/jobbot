# jobbot: job-board monitor that notifies over Telegram.
#
#   make            build build/jobbot
#   make test       build with ASan/UBSan and run the unit tests
#   make e2e        run the sanitized binary against a local mock job board
#                   and mock Telegram API (needs python3 and sqlite3)
#   make check      test + e2e
#   make clean
#
# Needs libcurl, libxml2 and sqlite3 development files.
#   Termux:        pkg install clang make pkg-config libcurl libxml2 libsqlite
#   Debian/Ubuntu: apt install build-essential pkg-config \
#                  libcurl4-openssl-dev libxml2-dev libsqlite3-dev
#   macOS:         the SDK already provides all three.

CC ?= cc
CSTD := -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
WARN := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
OPT ?= -O2 -g

ifeq ($(shell uname -s),Darwin)
  DEP_CFLAGS := -I$(shell xcrun --show-sdk-path)/usr/include/libxml2
  DEP_LIBS := -lcurl -lxml2 -lsqlite3
else
  PKGS := libcurl libxml-2.0 sqlite3
  DEP_CFLAGS := $(shell pkg-config --cflags $(PKGS))
  DEP_LIBS := $(shell pkg-config --libs $(PKGS))
endif

SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined

INCLUDES := -Iinclude -Ivendor/cjson
LIB_SRCS := $(filter-out src/main.c,$(wildcard src/*.c src/sources/*.c))
VENDOR_SRCS := vendor/cjson/cJSON.c
TEST_SRCS := $(wildcard tests/test_*.c)

BUILD := build
OBJS := $(patsubst %.c,$(BUILD)/rel/%.o,$(LIB_SRCS) $(VENDOR_SRCS) src/main.c)
SAN_LIB_OBJS := $(patsubst %.c,$(BUILD)/san/%.o,$(LIB_SRCS) $(VENDOR_SRCS))
TEST_BINS := $(patsubst tests/%.c,$(BUILD)/san/tests/%,$(TEST_SRCS))

.PHONY: all test e2e check clean
all: $(BUILD)/jobbot

$(BUILD)/jobbot: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(DEP_LIBS)

# Vendored code is compiled without our warning set.
$(BUILD)/rel/vendor/%.o: vendor/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(OPT) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/rel/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(OPT) $(CFLAGS) $(INCLUDES) $(DEP_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/san/vendor/%.o: vendor/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) -O1 -g $(SAN) -MMD -MP -c -o $@ $<

$(BUILD)/san/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) -O1 -g $(SAN) $(INCLUDES) -Itests $(DEP_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/san/tests/%: $(BUILD)/san/tests/%.o $(SAN_LIB_OBJS)
	$(CC) $(SAN) -o $@ $^ $(DEP_LIBS)

$(BUILD)/san/jobbot: $(BUILD)/san/src/main.o $(SAN_LIB_OBJS)
	$(CC) $(SAN) -o $@ $^ $(DEP_LIBS)

e2e: $(BUILD)/san/jobbot
	tests/e2e.sh $(BUILD)/san/jobbot

check: test e2e

# Tests read fixtures relative to the repository root.
test: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do \
		echo "== $$t"; $$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; echo "all tests passed"

clean:
	rm -rf $(BUILD)

.SECONDARY:
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
