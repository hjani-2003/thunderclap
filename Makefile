CC      ?= gcc
PKGS    := libsodium libzstd libcjson libxxhash
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -pthread $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -pthread
PREFIX  ?= $(HOME)/.local

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/%.o) build/webui.o
WEB := web/index.html web/app.js web/style.css

all: clap

clap: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

build/%.o: src/%.c src/*.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

# The web UI is compiled into the binary as C arrays.
build/bin2c: tools/bin2c.c | build
	$(CC) -O2 -o $@ $<

build/webui.c: build/bin2c $(WEB)
	build/bin2c index_html web/index.html app_js web/app.js style_css web/style.css > $@

build/webui.o: build/webui.c
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

TESTS := $(wildcard tests/unit/*.c)
LIB_OBJ := $(filter-out build/main.o,$(OBJ))

build/test_%: tests/unit/test_%.c $(LIB_OBJ)
	$(CC) $(CFLAGS) -Isrc -o $@ $^ $(LDFLAGS) $(LDLIBS)

test: $(TESTS:tests/unit/test_%.c=build/test_%) clap
	@for t in $(TESTS:tests/unit/test_%.c=build/test_%); do echo "== $$t"; $$t || exit 1; done
	@echo "all unit tests passed"

e2e: clap
	tests/e2e.sh

# User guide PDF (needs Node.js 22+ and Google Chrome or Chromium).
docs: docs/thunderclap-user-guide.pdf

docs/thunderclap-user-guide.pdf: docs/guide/guide.html docs/guide/guide.css $(wildcard docs/img/*.png) docs/tools/build-pdf.mjs docs/tools/cdp.mjs
	node docs/tools/build-pdf.mjs docs/guide/guide.html $@

install: clap
	install -Dm755 clap $(PREFIX)/bin/clap
	install -Dm644 systemd/clap.service $(HOME)/.config/systemd/user/clap.service
	@echo "installed to $(PREFIX)/bin/clap"
	@echo "enable at login with: systemctl --user enable --now clap"

clean:
	rm -rf build clap

.PHONY: all test e2e docs install clean
