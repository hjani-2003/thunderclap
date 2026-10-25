#!/usr/bin/env bash
# End-to-end test: two beam daemons on this machine (separate homes and
# ports) exercise pairing, transfers, metadata, resume and one-shot codes.
set -euo pipefail

BEAM="$(cd "$(dirname "$0")/.." && pwd)/beam"
[ -x "$BEAM" ] || { echo "build first: make"; exit 1; }
W="$(mktemp -d "${TMPDIR:-/tmp}/beam-e2e.XXXXXX")"
A="$W/a"; B="$W/b"; C="$W/c"
PASS=0

cleanup() {
    for h in "$A" "$B" "$C"; do "$BEAM" --home "$h" stop >/dev/null 2>&1 || true; done
    rm -rf "$W"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

ok()   { PASS=$((PASS + 1)); echo "  ✓ $*"; }
fail() { echo "  ✗ $*"; echo "--- logs ---"; tail -n 20 "$A/log" "$B/log" 2>/dev/null; exit 1; }

start() {   # start <home> <name> <port offset>
    mkdir -p "$1"
    "$BEAM" --home "$1" daemon --name "$2" --port $((47800 + $3)) --web $((48000 + $3)) --dir "$1/dl" >>"$1/log" 2>&1 &
    for _ in $(seq 50); do [ -f "$1/daemon.pid" ] && "$BEAM" --home "$1" status >/dev/null 2>&1 && return; sleep 0.1; done
    fail "daemon $2 did not start"
}

code_from() { grep -oE '[0-9]{4}-[0-9]{4}-[0-9]{4}' "$1" | head -1; }

tree_digest() {   # paths, types, modes, sizes, mtimes, link targets and content hashes
    (cd "$1" && find . -printf '%P %y %m %s %T@ %l\n' | sort && find . -type f -exec sha256sum {} + | sort -k2)
}

echo "== setup in $W"
start "$A" alpha 1
start "$B" bravo 2
start "$C" charlie 3

echo "== test data"
mkdir -p "$W/src/tree/sub/deeper" "$W/src/tree/empty"
head -c 300M /dev/urandom > "$W/src/big.bin"
for i in $(seq 1 3000); do head -c $((RANDOM % 4000 + 1)) /dev/urandom > "$W/src/tree/sub/f$i"; done
{ yes "2026-10-25 12:00:00 INFO GET /api/items status=200 latency=12ms" || true; } | head -c 50M > "$W/src/tree/app.log"
: > "$W/src/tree/empty.txt"
chmod 640 "$W/src/tree/empty.txt"; chmod 750 "$W/src/tree/sub/deeper"
ln -s sub/f1 "$W/src/tree/link"
touch -d '2021-06-01 10:00' "$W/src/tree/app.log"

echo "== pairing"
"$BEAM" --home "$B" pair > "$W/p1" 2>&1 & BG=$!
sleep 0.5
if "$BEAM" --home "$A" pair 127.0.0.1:47802 0000-0000-0000 >/dev/null 2>&1; then fail "wrong code accepted"; fi
wait $BG || true
ok "wrong pairing code rejected"
"$BEAM" --home "$B" pair > "$W/p2" 2>&1 & BG=$!
sleep 0.5
"$BEAM" --home "$A" pair 127.0.0.1:47802 "$(code_from "$W/p2")" >/dev/null || fail "pairing failed"
wait $BG
"$BEAM" --home "$B" peers | grep -q alpha || fail "bravo does not list alpha"
ok "paired alpha ↔ bravo"

echo "== transfers"
"$BEAM" --home "$A" send "$W/src/big.bin" --to 127.0.0.1:47802 >/dev/null || fail "big file send"
cmp "$W/src/big.bin" "$B/dl/big.bin" || fail "big file differs"
ok "300 MiB file"

"$BEAM" --home "$A" send "$W/src/tree" --to 127.0.0.1:47802 >/dev/null || fail "tree send"
diff <(tree_digest "$W/src/tree") <(tree_digest "$B/dl/tree") >/dev/null || fail "tree differs"
ok "folder: 3000 small files, modes, mtimes, symlink, empty file/dir"

"$BEAM" --home "$A" send "$W/src/big.bin" --to 127.0.0.1:47802 >/dev/null || fail "second send"
[ -f "$B/dl/big (1).bin" ] || fail "existing file was not kept"
ok "name clash saved as 'big (1).bin'"

if "$BEAM" --home "$C" send "$W/src/big.bin" --to 127.0.0.1:47802 >/dev/null 2>&1; then fail "unpaired send accepted"; fi
ok "unpaired device refused"

"$BEAM" --home "$B" recv --code > "$W/r1" 2>&1 & BG=$!
sleep 0.5
"$BEAM" --home "$C" send "$W/src/tree/app.log" --to 127.0.0.1:47802 --code "$(code_from "$W/r1")" >/dev/null || fail "code send"
wait $BG
cmp "$W/src/tree/app.log" "$B/dl/app.log" || fail "code transfer differs"
ok "one-shot code transfer from an unpaired device"

echo "== resume"
mkdir -p "$W/src/res"; ln "$W/src/big.bin" "$W/src/res/big.bin"; cp -r "$W/src/tree/sub" "$W/src/res/"
# zstd level 19 on random data over one stream is slow enough to interrupt.
"$BEAM" --home "$A" send "$W/src/res" --to 127.0.0.1:47802 --compress 19 --streams 1 --detach >/dev/null
sleep 2
kill -9 "$(cat "$A/daemon.pid")"; rm -f "$A/daemon.pid"
sleep 1
ls "$B/dl/.beam/"*.journal >/dev/null 2>&1 || fail "no journal after interruption"
start "$A" alpha 1
"$BEAM" --home "$A" send "$W/src/res" --to 127.0.0.1:47802 > "$W/resume.out" || fail "resumed send"
grep -q resumed "$W/resume.out" || fail "transfer did not resume"
diff <(tree_digest "$W/src/res") <(tree_digest "$B/dl/res") >/dev/null || fail "resumed tree differs"
ls "$B/dl/.beam/"*.journal >/dev/null 2>&1 && fail "journal left behind"
ok "killed sender mid-transfer, resumed without resending finished chunks"

echo "== web"
TOKEN="$(cat "$A/web_token")"
[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST http://127.0.0.1:48001/api/info)" = 403 ] || fail "web API without token"
[ "$(curl -s -o /dev/null -w '%{http_code}' -H 'Host: evil.example' http://127.0.0.1:48001/)" = 403 ] || fail "foreign Host accepted"
curl -sf -X POST -H "Authorization: Bearer $TOKEN" http://127.0.0.1:48001/api/info | grep -q '"name":"alpha"' || fail "web API with token"
ok "web API requires the token and a loopback Host"

echo "== bench"
"$BEAM" --home "$A" bench 127.0.0.1:47802 --secs 2 || fail "bench"
ok "link benchmark"

echo "all $PASS end-to-end checks passed"
