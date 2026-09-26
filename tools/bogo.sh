#!/bin/sh
# Runs BoGo (BoringSSL's TLS protocol test suite) against this stack, through
# tools/bogo_shim.nio.
#
# This needs a Go toolchain and a BoringSSL checkout. The script fetches both at
# run time and vendors neither. For this reason it is not part of
# `nio run tests/all.nio`, which needs only a C compiler.
#
#   sh tools/bogo.sh              # fetch if needed, build the shim, run
#   sh tools/bogo.sh -test Foo    # ...extra arguments go to the runner
#
# Environment:
#   NIO           the compiler to build the shim with     (default ./nio)
#   BOGO_DIR      where the BoringSSL checkout lives      (default build/bogo)
#   BOGO_COMMIT   which commit to pin to                  (see below)
#
# The commit is pinned. A suite that changes under you reports a new upstream
# test as a regression. To move the pin, change it and read the diff in the
# run's output.
set -eu

BOGO_COMMIT="${BOGO_COMMIT:-188ce3c13adb70fc416e7e81c5b8052142e8eb8c}"
BOGO_DIR="${BOGO_DIR:-build/bogo}"
NIO="${NIO:-./nio}"

root=$(pwd)

if ! command -v go >/dev/null 2>&1; then
    echo "tools/bogo.sh needs a Go toolchain to run BoringSSL's test runner." >&2
    echo "Everything else in this repository builds with clang alone; this" >&2
    echo "one job does not. Install Go, or skip it." >&2
    exit 1
fi

if [ ! -x "$NIO" ] && ! command -v "$NIO" >/dev/null 2>&1; then
    echo "no compiler at $NIO -- run 'sh bootstrap/build.sh' first." >&2
    exit 1
fi

# A sparse, blobless checkout: the runner and the one package it imports are a
# few megabytes. The full repository is much larger.
if [ ! -d "$BOGO_DIR/.git" ]; then
    echo "== fetching BoringSSL's test runner at $BOGO_COMMIT"
    mkdir -p "$BOGO_DIR"
    git -C "$BOGO_DIR" init -q
    git -C "$BOGO_DIR" remote add origin https://boringssl.googlesource.com/boringssl
    git -C "$BOGO_DIR" config extensions.partialClone origin
    git -C "$BOGO_DIR" sparse-checkout init --cone
    git -C "$BOGO_DIR" sparse-checkout set ssl/test/runner util/testresult
fi

if [ "$(git -C "$BOGO_DIR" rev-parse HEAD 2>/dev/null || true)" != "$BOGO_COMMIT" ]; then
    git -C "$BOGO_DIR" fetch -q --filter=blob:none --depth 1 origin "$BOGO_COMMIT"
    git -C "$BOGO_DIR" checkout -q "$BOGO_COMMIT"
fi

echo "== building the shim"
"$NIO" build tools/bogo_shim.nio -o "$root/$BOGO_DIR/bogo_shim"

echo "== running BoGo"
# The shim exits 89 for every flag it does not implement.
# -allow-unimplemented reports those instead of counting them as failures, and
# the count printed below keeps a skip visible.
cd "$BOGO_DIR/ssl/test/runner"
set +e
go test -timeout 60m -args \
    -shim-path "$root/$BOGO_DIR/bogo_shim" \
    -shim-config "$root/tools/bogo_shim_config.json" \
    -allow-unimplemented \
    -pipe \
    "$@" > "$root/$BOGO_DIR/bogo.log" 2>&1
status=$?
set -e
cd "$root"

skipped=$(grep -c '^UNIMPLEMENTED' "$BOGO_DIR/bogo.log" || true)
failed=$(grep -c '^FAILED' "$BOGO_DIR/bogo.log" || true)

echo "== $failed failed, $skipped skipped as unimplemented"
echo "   full output: $BOGO_DIR/bogo.log"

if [ "$status" -ne 0 ]; then
    grep -A12 '^FAILED' "$BOGO_DIR/bogo.log" | head -200
fi
exit "$status"
