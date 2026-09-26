# visual-http: HTTP server benchmark with a report page

This benchmark drives the same HTTP server, written in **Nio**, **Go**,
**Node.js**, **Bun** and **Java**, through a growing number of concurrent
keep-alive connections. It records what each runtime does under that load:
throughput, response-time percentiles, CPU load and memory. Two scenarios run
per language: a plaintext `GET`, and a `POST` of a JSON document of about
50 KB that the server must parse and answer with a JSON summary. The output is
a single self-contained web page, `results/report.html`, with one section per
scenario: interactive charts and a per-stage summary table.

```sh
sh run.sh                          # node bun go java nio, 10→1000 connections
sh run.sh nio go                   # a subset (targets: nio go go1 node bun java)
STAGES=10,100,1000 DURATION=15 SCENARIOS=json sh run.sh
open results/report.html
```

Everything runs one at a time: one server process, one scenario, one load run.
Two servers or two loads never run together.

Requirements: `./nio` at the repo root (or `NIO=...`), `go`, `node`, `bun`,
`java` 21+ (virtual threads), `python3`. The first run creates a local `.venv`
and installs `psutil` into it. Nothing else is installed.

## How it measures

- **Servers** (`servers/`) share four routes: `GET /plaintext`, `GET /json`,
  `GET /users/:id`, and `POST /data`. `POST /data` parses the posted document,
  sums the `price` field over `items`, and answers `{"count": n, "total": t}`
  as JSON. Each server frames responses by `Content-Length`, binds port 0 and
  prints the port it got, so no fixed port is ever named. Bun runs `Bun.serve`
  (its native path), not the `node:http` shim. Java runs the JDK's built-in
  `com.sun.net.httpserver` on a virtual-thread executor, with a minimal
  vendored JSON parser because the JDK has none. Every other language uses its
  standard JSON library.
- **Load** (`bench.py`) is closed-loop: each connection sends its request,
  reads the whole response, and repeats. The schedule is a series of stages:
  hold N connections for `DURATION` seconds, then increase N. So "growing
  connections over time" is literal. Latency is recorded per request into
  per-second buckets. Connections are spread over worker processes that run
  asyncio, so the ceiling of the Python client is well above the range of
  interest. The POST body is deterministic, generated once, and identical for
  every language.
- **CPU and memory** are sampled from the server process twice a second with
  psutil while the load runs. CPU is % of one core, so Go and Java (the
  multicore servers here) can exceed 100. The `go1` target runs the Go binary
  under `GOMAXPROCS=1`, for a like-for-like comparison with the single-threaded
  runtimes.
- The first second of each stage is settling time (connections opening) and is
  dropped from the per-stage aggregates. The per-second charts show it.

## Reading the results

With closed-loop load, throughput and latency are two views of the same thing
(rps = connections ÷ mean latency), so the report shows both. The throughput
charts show how the ceiling scales with connections, and the p50/p99 charts
show what a request experiences on the way there. Errors (timeouts, refused or
dropped connections, non-2xx) are counted per stage in the table. In a
high-connection stage, a nonzero count is the result to look at.

The client raises its file-descriptor soft limit itself (macOS defaults to
256, far below 1000 connections), and the servers inherit it.

`bench.py --target X --scenario Y` runs one combination again into
`results/Y/X.json`. `report.py` rebuilds the page from whatever results exist.
