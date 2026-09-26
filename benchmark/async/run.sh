#!/usr/bin/env bash
# Builds and times the Nio, Go and Node.js variants of each async benchmark.
# C, C++ and Python have no comparable runtime. Run with -h for the options.
set -euo pipefail

cd "$(dirname "$0")/../.."
BIN=benchmark/bin
mkdir -p "$BIN"

NIO=${NIO:-./nio}
if [[ ! -x $NIO ]]; then
    echo "no compiler at $NIO -- run 'sh bootstrap/build.sh', or set NIO=<path>" >&2
    exit 2
fi

BENCHES=(spawn chain sleepers)

usage() {
    cat <<'USAGE'
usage: benchmark/async/run.sh [runs] [--no-go] [--no-node]

  runs        wall time is the average of N runs (default 5)
  --no-<x>    drop that variant from the build, the checksum check, and
              the tables.
USAGE
}

RUNS=5
SKIP=" " # Space-delimited and space-surrounded, so on()'s glob tests membership.
for arg in "$@"; do
    case "$arg" in
        --no-go | --no-node) SKIP+="${arg#--no-} " ;;
        -h | --help) usage && exit 0 ;;
        '' | *[!0-9]*) usage >&2 && exit 2 ;;
        *) RUNS=$arg ;;
    esac
done
on() { [[ $SKIP != *" $1 "* ]]; }

echo "== building =="
for b in "${BENCHES[@]}"; do
    "$NIO" build --release "benchmark/async/$b.nio" -o "$BIN/async_${b}_nio"
    if on go; then go build -o "$BIN/async_${b}_go" "benchmark/async/$b.go"; fi
done

echo "== verifying the variants agree =="
for b in "${BENCHES[@]}"; do
    expected=$("$BIN/async_${b}_nio")
    if on go; then
        out=$("$BIN/async_${b}_go")
        [[ $out == "$expected" ]] || { echo "MISMATCH in $b: nio=$expected go=$out" >&2 && exit 1; }
    fi
    if on node; then
        out=$(node "benchmark/async/$b.js")
        [[ $out == "$expected" ]] || { echo "MISMATCH in $b: nio=$expected node=$out" >&2 && exit 1; }
    fi
    echo "$b: $expected"
done

echo "== timing (average of $RUNS runs, wall clock) =="
python3 - "$RUNS" "$SKIP" "${BENCHES[@]}" <<'EOF'
import os, subprocess, sys, time

runs = int(sys.argv[1])
skipped = sys.argv[2].split()
benches = sys.argv[3:]

def run_once(cmd):
    """One run: (wall ms, peak RSS in bytes). os.wait4 gives per-child
    rusage; ru_maxrss is bytes on macOS but KiB on Linux."""
    t0 = time.perf_counter()
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL)
    _, status, ru = os.wait4(p.pid, 0)
    dt = (time.perf_counter() - t0) * 1000
    p.returncode = os.waitstatus_to_exitcode(status)
    if p.returncode != 0:
        sys.exit(f"error: {' '.join(cmd)} exited with {p.returncode}")
    rss = ru.ru_maxrss if sys.platform == "darwin" else ru.ru_maxrss * 1024
    return dt, rss

def measure(cmd):
    results = [run_once(cmd) for _ in range(runs)]
    return sum(r[0] for r in results) / runs, max(r[1] for r in results)

variants = [
    ("nio", "Nio", lambda b: [f"benchmark/bin/async_{b}_nio"]),
    ("go", "Go", lambda b: [f"benchmark/bin/async_{b}_go"]),
    ("node", "Node.js", lambda b: ["node", f"benchmark/async/{b}.js"]),
]
variants = [v for v in variants if v[0] not in skipped]

rows = []
for b in benches:
    cells = [measure(run(b)) for _, _, run in variants]
    rows.append((b, cells))

headers = [label for _, label, _ in variants]
name_w = max(len(b) for b in benches)
print()
print("wall clock:")
print("  " + "".ljust(name_w) + "".join(f"{h:>12}" for h in headers))
for b, cells in rows:
    print("  " + b.ljust(name_w) + "".join(f"{ms:>10.0f}ms" for ms, _ in cells))
print()
print("peak RSS (max over the runs):")
print("  " + "".ljust(name_w) + "".join(f"{h:>12}" for h in headers))
for b, cells in rows:
    print("  " + b.ljust(name_w) + "".join(f"{rss / (1 << 20):>10.1f}MB" for _, rss in cells))
EOF
