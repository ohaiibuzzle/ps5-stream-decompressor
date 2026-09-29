#!/usr/bin/env bash
#
# integration.sh -- build the client and the native server, then exercise the
# protocol end-to-end over loopback. No PS5 required.
#
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${PORT:-19091}"
WORK="${TMPDIR:-/tmp}/ps5sd-it-$$"
SRC="$WORK/src"
OUT="$WORK/out"
CLIENT="$ROOT/client/ps5push"
SERVER="$ROOT/server/server-native"

PASS=0
FAIL=0

info() { printf '%s\n' "$*"; }
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }

cleanup() {
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

# --- build -----------------------------------------------------------------
make -C "$ROOT/server" -f Makefile.host >/dev/null || { echo "server build failed"; exit 1; }
make -C "$ROOT/client" >/dev/null || { echo "client build failed"; exit 1; }

# --- fixtures --------------------------------------------------------------
mkdir -p "$SRC/multi" "$OUT"
head -c 4000000 /dev/urandom > "$SRC/rnd.bin"
head -c 8000000 /dev/zero    > "$SRC/zeros.bin"
cat "$SRC/rnd.bin" "$SRC/zeros.bin" "$SRC/rnd.bin" > "$SRC/plain.bin"
: > "$SRC/empty.bin"
head -c 300000 /dev/urandom > "$SRC/multi/a.bin"
head -c 500000 /dev/zero    > "$SRC/multi/b.bin"

HAVE_7Z=0
if command -v 7z >/dev/null 2>&1; then
    HAVE_7Z=1
    (cd "$SRC" && 7z a -bso0 -bsp0 single.7z plain.bin >/dev/null 2>&1)
    (cd "$SRC" && 7z a -bso0 -bsp0 solid.7z multi/a.bin multi/b.bin >/dev/null 2>&1)
    (cd "$SRC" && 7z a -bso0 -bsp0 -m0=delta:8 delta.7z plain.bin >/dev/null 2>&1)
fi
if command -v python3 >/dev/null 2>&1; then
    python3 - "$SRC" <<'PY'
import sys, zipfile, os
src = sys.argv[1]
with zipfile.ZipFile(os.path.join(src, "single.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    z.write(os.path.join(src, "plain.bin"), "plain.bin")
PY
fi

# --- helpers ---------------------------------------------------------------
start_server() {
    "$SERVER" -q -p "$PORT" -d "$OUT" "$@" &
    SRV_PID=$!
    sleep 0.3
}
stop_server() {
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null
    wait "$SRV_PID" 2>/dev/null
    SRV_PID=
}

# expect_ok NAME EXPECTED_FILE -- CLIENT_ARGS...
expect_ok() {
    local name="$1" expected="$2"; shift 2
    [ "$1" = "--" ] && shift
    rm -f "$OUT/out.bin"
    start_server
    if ! "$CLIENT" -q -H 127.0.0.1 -p "$PORT" -d out.bin "$@" >"$WORK/log" 2>&1; then
        bad "$name (client error)"; cat "$WORK/log"; stop_server; return
    fi
    stop_server
    if [ -f "$expected" ] && cmp -s "$expected" "$OUT/out.bin"; then
        ok "$name"
    else
        bad "$name (content mismatch)"
    fi
}

info "== basic paths =="
expect_ok "plain zstd"        "$SRC/plain.bin" -- "$SRC/plain.bin"
expect_ok "plain raw"         "$SRC/plain.bin" -- --raw "$SRC/plain.bin"
expect_ok "empty zstd"        "$SRC/empty.bin" -- "$SRC/empty.bin"
expect_ok "empty raw"         "$SRC/empty.bin" -- --raw "$SRC/empty.bin"

if [ -f "$SRC/single.zip" ]; then
    expect_ok "zip recompress" "$SRC/plain.bin" -- "$SRC/single.zip"
fi

if [ "$HAVE_7Z" = 1 ]; then
    info "== 7z paths =="
    expect_ok "7z passthrough"      "$SRC/plain.bin" -- "$SRC/single.7z"
    # Make sure the pass-through path (not a silent zstd fallback) is used.
    rm -f "$OUT/out.bin"
    start_server
    "$CLIENT" -H 127.0.0.1 -p "$PORT" -d out.bin "$SRC/single.7z" >"$WORK/log" 2>&1
    stop_server
    if grep -q "pass-through" "$WORK/log" && cmp -s "$SRC/plain.bin" "$OUT/out.bin"; then
        ok "7z passthrough actually used"
    else
        bad "7z passthrough actually used"
    fi
    expect_ok "7z forced recompress" "$SRC/plain.bin" -- --no-passthrough "$SRC/single.7z"
    # solid block shared by two members -> per-member passthrough declined
    expect_ok "7z solid member a"   "$SRC/multi/a.bin" -- -m multi/a.bin "$SRC/solid.7z"
    expect_ok "7z solid member b"   "$SRC/multi/b.bin" -- -m multi/b.bin "$SRC/solid.7z"

    # libarchive cannot decode every 7z filter chain (e.g. delta). Such a
    # transfer must fail cleanly and leave no partial file behind.
    rm -f "$OUT/out.bin"
    start_server
    if "$CLIENT" -q -H 127.0.0.1 -p "$PORT" -d out.bin "$SRC/delta.7z" >/dev/null 2>&1; then
        bad "unsupported filter rejected"
    elif [ -e "$OUT/out.bin" ]; then
        bad "unsupported filter left a partial file"
    else
        ok "unsupported filter rejected"
    fi
    stop_server
fi

info "== token auth =="
rm -f "$OUT/out.bin"
start_server -t 0x1234
if "$CLIENT" -q -H 127.0.0.1 -p "$PORT" -d out.bin -t 0x1234 "$SRC/plain.bin" >/dev/null 2>&1 \
   && cmp -s "$SRC/plain.bin" "$OUT/out.bin"; then ok "correct token"; else bad "correct token"; fi
stop_server
rm -f "$OUT/out.bin"
start_server -t 0x1234
if "$CLIENT" -q -H 127.0.0.1 -p "$PORT" -d out.bin -t 0x9999 "$SRC/plain.bin" >/dev/null 2>&1; then
    bad "wrong token rejected"
else
    ok "wrong token rejected"
fi
stop_server

info "== path safety =="
if command -v python3 >/dev/null 2>&1; then
    raw_hello() {
        python3 - "$1" "$2" "$PORT" <<'PY'
import socket, struct, sys
name = sys.argv[1].encode()
frames = int(sys.argv[2])
port = int(sys.argv[3])
s = socket.create_connection(("127.0.0.1", port))
fixed = struct.pack("<IBBBBHHIIQQQ", 0x53355350, 1, 1, 0, 0, len(name),
                    0, 0, 0, 0, 0, 0)
s.sendall(fixed + name)
if frames:
    s.sendall(struct.pack("<IIB", 0, 0, 1))  # empty final frame
data = s.recv(21)
if len(data) >= 21:
    print(data[0], struct.unpack("<I", data[17:21])[0])
else:
    print(-1, -1)
PY
    }
    start_server
    read -r stype scode < <(raw_hello "../escape" 0)
    # 2 = ERROR, 3 = BAD_NAME
    if [ "$stype" = 2 ] && [ "$scode" = 3 ] && [ ! -e "$WORK/escape" ]; then
        ok "traversal rejected"
    else
        bad "traversal rejected ($stype/$scode)"
    fi
    read -r stype scode < <(raw_hello "ok.bin" 1)
    if [ "$stype" = 1 ]; then ok "safe name accepted"; else bad "safe name accepted"; fi
    stop_server
    rm -f "$OUT/ok.bin"
else
    info "  (python3 not available, skipping)"
fi

info "== resume =="
if command -v truncate >/dev/null 2>&1; then
    rm -f "$OUT/out.bin"
    start_server
    "$CLIENT" -q -H 127.0.0.1 -p "$PORT" -d out.bin -C 1048576 "$SRC/plain.bin" >/dev/null 2>&1
    stop_server
    truncate -s 7340032 "$OUT/out.bin"
    start_server
    if "$CLIENT" -q -H 127.0.0.1 -p "$PORT" -d out.bin -C 1048576 --resume "$SRC/plain.bin" >/dev/null 2>&1 \
       && cmp -s "$SRC/plain.bin" "$OUT/out.bin"; then
        ok "resume zstd"
    else
        bad "resume zstd"
    fi
    stop_server
else
    info "  (truncate not available, skipping)"
fi

info ""
info "passed: $PASS   failed: $FAIL"
[ "$FAIL" -eq 0 ]
