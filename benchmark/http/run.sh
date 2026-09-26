#!/usr/bin/env bash
# Builds and times the Nio, Go, Java and Node.js HTTP servers, and the parsing
# benchmark without sockets (Nio, Go, Node.js). C, C++ and Python are not in
# this suite: it compares HTTP stacks, and these four are one of each kind.
# benchmark/http/README.md describes each one.
#
# Usage: benchmark/http/run.sh [runs] [--no-go] [--no-node] [--no-java] [--requests N] [--conns "1 100 1000"] [--passes N]
#
#   runs         the parse benchmark is the median of N runs (default 5)
#   --no-<x>     drop that variant from the build, the checks, and the tables
#   --requests   requests per load pass (default 50000), and at least 1,000
#                per connection, so a high count still reaches a steady state
#   --conns      connection counts to test, space-separated (default "1 100 1000")
#   --passes     load passes per server, interleaved; the median is shown (default 3)
set -euo pipefail

cd "$(dirname "$0")/../.."
BIN=benchmark/bin
SRC=benchmark/http
mkdir -p "$BIN"

NIO=${NIO:-./nio}
if [[ ! -x $NIO ]]; then
    echo "no compiler at $NIO -- run 'sh bootstrap/build.sh', or set NIO=<path>" >&2
    exit 2
fi

usage() {
    sed -n '2,14p' "$0" | sed 's|^# \?||'
}

RUNS=5
REQUESTS=50000
CONNS="1 100 1000"
PASSES=3
SKIP=" " # Space-delimited and space-surrounded, so on()'s glob tests membership.
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-go | --no-node | --no-java) SKIP+="${1#--no-} " ;;
        --requests) REQUESTS=$2 && shift ;;
        --conns) CONNS=$2 && shift ;;
        --passes) PASSES=$2 && shift ;;
        -h | --help) usage && exit 0 ;;
        '' | *[!0-9]*) usage >&2 && exit 2 ;;
        *) RUNS=$1 ;;
    esac
    shift
done
on() { [[ $SKIP != *" $1 "* ]]; }

if ! on go; then
    echo "note: the load generator is written in Go, so --no-go also skips" >&2
    echo "      every server measurement. Only 'parse' will run." >&2
fi

echo "== building =="
"$NIO" build --release "$SRC/serve.nio" -o "$BIN/http_serve_nio"
"$NIO" build --release "$SRC/parse.nio" -o "$BIN/http_parse_nio"
if on go; then
    go build -o "$BIN/http_serve_go" "$SRC/serve.go"
    go build -o "$BIN/http_parse_go" "$SRC/parse.go"
    go build -o "$BIN/http_load" "$SRC/load.go"
fi
if on java; then
    javac -d "$BIN/http_java" "$SRC/serve.java"
fi

# ---------------------------------------------------------------------------
# parse: no sockets and no scheduler, only the language over strings
# ---------------------------------------------------------------------------

echo
echo "== verifying the parse variants agree =="
expected=$("$BIN/http_parse_nio")
echo "nio:  $expected"
if on go; then
    got=$("$BIN/http_parse_go")
    [[ $got == "$expected" ]] || { echo "MISMATCH: nio=$expected go=$got" >&2 && exit 1; }
    echo "go:   $got"
fi
if on node; then
    got=$(node "$SRC/parse.js")
    [[ $got == "$expected" ]] || { echo "MISMATCH: nio=$expected node=$got" >&2 && exit 1; }
    echo "node: $got"
fi

echo
echo "== parse: 200,000 request messages (median of $RUNS runs) =="
python3 - "$RUNS" "$SKIP" <<'PYEOF'
import os, subprocess, sys, time

runs = int(sys.argv[1])
skipped = sys.argv[2].split()

def run_once(cmd):
    """One run: (wall ms, peak RSS in bytes). ru_maxrss is bytes on macOS,
    KiB on Linux."""
    t0 = time.perf_counter()
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL)
    _, status, ru = os.wait4(p.pid, 0)
    dt = (time.perf_counter() - t0) * 1000
    code = os.waitstatus_to_exitcode(status)
    if code != 0:
        sys.exit(f"error: {' '.join(cmd)} exited with {code}")
    rss = ru.ru_maxrss if sys.platform == "darwin" else ru.ru_maxrss * 1024
    return dt, rss

variants = [
    ("nio", "Nio", ["benchmark/bin/http_parse_nio"]),
    ("go", "Go", ["benchmark/bin/http_parse_go"]),
    ("node", "Node.js", ["node", "benchmark/http/parse.js"]),
]
variants = [v for v in variants if v[0] not in skipped]

# Use the median. Every run is under a second, so one descheduled run moves a
# mean by hundreds of milliseconds.
print("  " + "".ljust(8) + "".join(f"{label:>12}" for _, label, _ in variants))
walls, rsses = [], []
for _, _, cmd in variants:
    results = sorted(run_once(cmd) for _ in range(runs))
    walls.append(results[len(results) // 2][0])
    rsses.append(max(r[1] for r in results))
print("  " + "wall".ljust(8) + "".join(f"{ms:>10.0f}ms" for ms in walls))
print("  " + "rss".ljust(8) + "".join(f"{r / (1 << 20):>10.1f}MB" for r in rsses))
PYEOF

if ! on go; then
    echo
    echo "server benchmarks skipped (no load generator without Go)"
    exit 0
fi

# ---------------------------------------------------------------------------
# serve: a real socket, a real client, keep-alive
#
# This part is in Python: peak RSS comes from os.wait4 on the killed server.
# Sampling ps during the run can miss the peak.
# ---------------------------------------------------------------------------

echo
echo "== serve: $REQUESTS keep-alive requests per pass, at least 1,000 per connection =="
python3 - "$SKIP" "$REQUESTS" "$CONNS" "$PASSES" <<'PYEOF'
import os, re, signal, statistics, subprocess, sys, tempfile, time

skipped = sys.argv[1].split()
requests = int(sys.argv[2])
conn_levels = [int(c) for c in sys.argv[3].split()]
passes = int(sys.argv[4])

BIN = "benchmark/bin"
SRC = "benchmark/http"
PATHS = ["/plaintext", "/json", "/users/42"]

# The JDK server closes idle keep-alive connections past 200 by default. The
# load generator opens every connection before the clock starts, so without
# this flag the server refuses most of 1,000 connections.
JAVA = ["java", "-Dsun.net.httpserver.maxIdleConnections=10000", "-cp", f"{BIN}/http_java", "Serve"]

variants = [
    ("nio", "Nio", [f"{BIN}/http_serve_nio"], {}),
    # Nio and Node.js serve from one thread, so Go and Java are held to one core
    # to match, and their multicore runs are the variants.
    ("go", "Go", [f"{BIN}/http_serve_go"], {"GOMAXPROCS": "1"}),
    ("go", "Go(multi)", [f"{BIN}/http_serve_go"], {}),
    # The JVM sizes its GC, JIT and virtual-thread pools from this count.
    ("java", "Java", JAVA[:1] + ["-XX:ActiveProcessorCount=1"] + JAVA[1:], {}),
    ("java", "Java(multi)", JAVA, {}),
    ("node", "Node.js", ["node", f"{SRC}/serve.js"], {}),
]
variants = [v for v in variants if v[0] not in skipped]

def start(argv, extra_env):
    """Start a server and wait for it to announce its port. Every server here
    binds port 0 and prints the port, so this suite needs no particular port to
    be free -- the discipline tests/net_test.nio uses."""
    out = tempfile.NamedTemporaryFile(mode="w+", delete=False, suffix=".port")
    env = dict(os.environ)
    env.update(extra_env)
    p = subprocess.Popen(argv, stdout=out, stderr=subprocess.DEVNULL, env=env)
    for _ in range(400):
        with open(out.name) as f:
            # A complete line of digits. A half-flushed port reads as a smaller
            # number, and the load goes to the wrong place.
            m = re.search(r"^(\d+)$", f.read(), re.M)
        if m:
            return p, int(m.group(1)), out.name
        if p.poll() is not None:
            sys.exit(f"server exited before binding: {' '.join(argv)}")
        time.sleep(0.05)
    p.kill()
    sys.exit(f"server never announced a port: {' '.join(argv)}")

def stop(p, path):
    """Kill the server and answer the peak RSS the kernel charged it."""
    p.send_signal(signal.SIGTERM)
    try:
        _, _, ru = os.wait4(p.pid, 0)
        rss = ru.ru_maxrss if sys.platform == "darwin" else ru.ru_maxrss * 1024
    except ChildProcessError:
        rss = 0
    os.unlink(path)
    return rss

def load(port, target, conns, total, warmup=2000):
    out = subprocess.run(
        [f"{BIN}/http_load", "-addr", f"127.0.0.1:{port}", "-path", target,
         "-conns", str(conns), "-requests", str(total), "-warmup", str(warmup)],
        capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"load failed on {target}: {out.stderr.strip()}")
    body = rps = None
    for line in out.stdout.splitlines():
        if line.startswith("body "):
            body = line[5:]
        elif line.startswith("rps "):
            rps = float(line[4:])
    return body, rps

# Every server must serve the same bytes, or the tables compare different work.
print()
print("  verifying the servers agree on what they serve")
expected = {}
for _, label, argv, env in variants:
    p, port, tmp = start(argv, env)
    for target in PATHS:
        body, _ = load(port, target, 1, 1, warmup=0)
        if target not in expected:
            expected[target] = body
        elif expected[target] != body:
            stop(p, tmp)
            sys.exit(f"MISMATCH on {target}: expected {expected[target]!r}, "
                     f"{label} served {body!r}")
    stop(p, tmp)
for target in PATHS:
    print(f"    {target} -> {expected[target]}")

# Each pass runs every server once, so load from outside the benchmark lands on
# all of them alike, and the median drops a pass that was descheduled.
labels = [label for _, label, _, _ in variants]
for conns in conn_levels:
    got = {(i, t): [] for i in range(len(variants)) for t in PATHS}
    rss = [0] * len(variants)
    for _ in range(passes):
        for i, (_, _, argv, env) in enumerate(variants):
            p, port, tmp = start(argv, env)
            for target in PATHS:
                _, r = load(port, target, conns, max(requests, conns * 1000))
                got[(i, target)].append(r)
            rss[i] = max(rss[i], stop(p, tmp))
    rows = {t: [statistics.median(got[(i, t)]) for i in range(len(variants))] for t in PATHS}

    print()
    print(f"  {conns} connection(s), requests/second (median of {passes} passes):")
    print("    " + "".ljust(12) + "".join(f"{h:>12}" for h in labels))
    for target in PATHS:
        print("    " + target.ljust(12) + "".join(f"{v:>12,.0f}" for v in rows[target]))
    print("    " + "peak rss".ljust(12) + "".join(f"{v / (1 << 20):>10.1f}MB" for v in rss))
PYEOF
echo
