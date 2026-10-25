CC      ?= gcc
PKGS    := libsodium libzstd libcjson libxxhash
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu23 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -pthread $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -pthread
PREFIX  ?= $(HOME)/.local

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/%.o)
WEB := $(wildcard web/*)

all: beam

beam: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

build/%.o: src/%.c src/*.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

# The web UI is compiled into the binary with #embed.
build/http.o: $(WEB)

build:
	mkdir -p build

TESTS := $(wildcard tests/unit/*.c)
LIB_OBJ := $(filter-out build/main.o,$(OBJ))

build/test_%: tests/unit/test_%.c $(LIB_OBJ)
	$(CC) $(CFLAGS) -Isrc -o $@ $^ $(LDFLAGS) $(LDLIBS)

test: $(TESTS:tests/unit/test_%.c=build/test_%) beam
	@for t in $(TESTS:tests/unit/test_%.c=build/test_%); do echo "== $$t"; $$t || exit 1; done
	@echo "all unit tests passed"

e2e: beam
	tests/e2e.sh

install: beam
	install -Dm755 beam $(PREFIX)/bin/beam
	install -Dm644 systemd/beam.service $(HOME)/.config/systemd/user/beam.service
	@echo "installed to $(PREFIX)/bin/beam"
	@echo "enable at login with: systemctl --user enable --now beam"

clean:
	rm -rf build beam

.PHONY: all test e2e install clean
