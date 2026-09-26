#!/usr/bin/env bash
# Builds and times the Nio, C, C++, Go, Java, Node.js and Python variants of each
# benchmark. Run with -h for the options.
set -euo pipefail

cd "$(dirname "$0")/.."
BIN=benchmark/bin
mkdir -p "$BIN"

NIO=${NIO:-./nio}
if [[ ! -x $NIO ]]; then
    echo "no compiler at $NIO -- run 'sh bootstrap/build.sh', or set NIO=<path>" >&2
    exit 2
fi

BENCHES=(loops fib primes mandelbrot records strings files)

usage() {
    cat <<'USAGE'
usage: benchmark/run.sh [runs] [--python] [--no-node] [--no-go] [--no-java] [--no-c] [--no-cpp]

  runs        wall time is the median of N runs (default 5)
  --no-<x>    drop that variant from the build, the checksum check, and
              the tables.
  --python    add Python back. It is off by default because it is ~20x
              everyone else's total and dominates a run: loops.py alone
              takes ~9 seconds against everyone else's ~0.1.
USAGE
}

RUNS=5
# SKIP is space-delimited and space-surrounded, so the glob in on() can test
# membership.
SKIP=" python "
for arg in "$@"; do
    case "$arg" in
        --python) SKIP=${SKIP/ python / } ;;
        --no-c | --no-cpp | --no-go | --no-java | --no-node | --no-python) SKIP+="${arg#--no-} " ;;
        -h | --help) usage && exit 0 ;;
        '' | *[!0-9]*) usage >&2 && exit 2 ;;
        *) RUNS=$arg ;;
    esac
done
on() { [[ $SKIP != *" $1 "* ]]; }

echo "== building =="
for b in "${BENCHES[@]}"; do
    "$NIO" build --release "benchmark/$b.nio" -o "$BIN/${b}_nio"
    # Nio's LLVM IR and JS forbid FMA fusion, so C and C++ need
    # -ffp-contract=off or the mandelbrot checksums differ in the last digits.
    if on c; then clang -O2 -ffp-contract=off "benchmark/$b.c" -o "$BIN/${b}_c"; fi
    if on cpp; then clang++ -O2 -ffp-contract=off "benchmark/$b.cpp" -o "$BIN/${b}_cpp"; fi
    if on go; then go build -o "$BIN/${b}_go" "benchmark/$b.go"; fi
    # Every Java variant is a class named Main, each in its own directory.
    if on java; then javac -d "$BIN/${b}_java" "benchmark/$b.java"; fi
done
if on java; then
    printf 'class Main { public static void main(String[] a) {} }\n' >"$BIN/empty.java"
    javac -d "$BIN/empty_java" "$BIN/empty.java"
fi

echo "== verifying the variants agree =="
for b in "${BENCHES[@]}"; do
    expected=$("$BIN/${b}_nio")
    for variant in \
        "c:C:$BIN/${b}_c" \
        "cpp:C++:$BIN/${b}_cpp" \
        "go:Go:$BIN/${b}_go" \
        "java:Java:java -cp $BIN/${b}_java Main" \
        "node:Node.js:node benchmark/$b.js" \
        "python:Python:python3 benchmark/$b.py"; do
        key=${variant%%:*}
        rest=${variant#*:}
        on "$key" || continue
        out=$(${rest#*:})
        if [[ $out != "$expected" ]]; then
            echo "MISMATCH in $b: nio=$expected ${rest%%:*}=$out" >&2
            exit 1
        fi
    done
    echo "$b: $expected"
done

echo "== timing (median of $RUNS runs, wall clock) =="
python3 - "$RUNS" "$SKIP" "${BENCHES[@]}" <<'EOF'
import os, statistics, subprocess, sys, time

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
    return summarize([run_once(cmd) for _ in range(runs)])

# Use the median. Every benchmark runs in under a second, so one descheduled
# run moves a mean more than the differences the tables show.
def summarize(results):
    return statistics.median(r[0] for r in results), max(r[1] for r in results)

variants = [
    ("c", "C -O2", lambda b: [f"benchmark/bin/{b}_c"]),
    ("cpp", "C++ -O2", lambda b: [f"benchmark/bin/{b}_cpp"]),
    ("nio", "Nio", lambda b: [f"benchmark/bin/{b}_nio"]),
    ("go", "Go", lambda b: [f"benchmark/bin/{b}_go"]),
    ("java", "Java", lambda b: ["java", "-cp", f"benchmark/bin/{b}_java", "Main"]),
    ("node", "Node.js", lambda b: ["node", f"benchmark/{b}.js"]),
    ("python", "Python", lambda b: ["python3", f"benchmark/{b}.py"]),
]
variants = [v for v in variants if v[0] not in skipped]
keys = [key for key, _, _ in variants]

base_col = keys.index("c") if "c" in keys else 0
base_label = f"  vs {variants[base_col][1].split()[0]}"

startups = []
if "java" in keys:
    startups.append(("Java", measure(["java", "-cp", "benchmark/bin/empty_java", "Main"])))
if "node" in keys:
    startups.append(("Node.js", measure(["node", "-e", ""])))
if "python" in keys:
    startups.append(("Python", measure([sys.executable, "-c", ""])))

# Run one round of all variants at a time. A machine that gets busy halfway
# through then does not charge the whole slowdown to one variant.
rows = []
for b in benches:
    samples = [[] for _ in variants]
    for _ in range(runs):
        for i, (_, _, cmd) in enumerate(variants):
            samples[i].append(run_once(cmd(b)))
    rows.append((b, [summarize(s) for s in samples]))

header = f"{'benchmark':<12}" + "".join(f"{name:>10}" for _, name, _ in variants)
print(header + "   (ms)")
for b, cells in rows:
    base = cells[base_col][0]
    print(f"{b:<12}" + "".join(f"{ms:>10.0f}" for ms, _ in cells))
    print(f"{base_label:<12}" + "".join(f"{ms / base:>9.2f}x" for ms, _ in cells))
print(f"\nMemory usage (MB)")
print(f"{header}   (peak RSS, MB)")
for b, cells in rows:
    print(f"{b:<12}" + "".join(f"{rss / 1e6:>10.1f}" for _, rss in cells))

if startups:
    print("\nnote: these numbers include the virtual machine or interpreter itself:")
    for name, (ms, rss) in startups:
        print(f"  {name:<8} ~{ms:>4.0f} ms of startup, ~{rss / 1e6:>4.1f} MB baseline RSS")
    print("(measured by running each on an empty program). Nio, C, C++, and Go")
    print("are precompiled native binaries with negligible startup.")
EOF
