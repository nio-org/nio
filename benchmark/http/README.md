# HTTP benchmarks

Micro-benchmarks for the `http` module (§6.16), comparing Nio against Go's
`net/http`, the JDK's `com.sun.net.httpserver` and Node.js's `node:http`. C,
C++ and Python are not in this suite. The comparison is between HTTP stacks,
and these four are one of each kind:

- Nio: a single-threaded runtime with a library written in the language itself.
- Go: a multi-core runtime with a native library.
- Java: a multi-core JIT-compiled runtime with virtual threads.
- Node: a single-threaded runtime with a C parser behind a JavaScript surface.

Java takes part in `serve` only.

Two benchmarks measure different halves:

| Benchmark | What it stresses |
|---|---|
| `parse` | the request path with no socket at all: 200,000 request messages split, scanned and folded into a header map |
| `serve` | a real server over loopback with keep-alive, driven by a shared load generator at 1, 100 and 1,000 connections |

Every variant of `parse` prints a checksum, and the runner refuses to time
anything if they disagree. Before any server is timed, every server is checked
to serve byte-identical bodies on all three routes.

## Running

```sh
benchmark/http/run.sh                        # 5 runs of parse, 50,000 requests per load pass
benchmark/http/run.sh 10                     # 10 runs of parse
benchmark/http/run.sh --requests 200000      # a longer load pass
benchmark/http/run.sh --conns "1 10 100 500" # other concurrency levels
benchmark/http/run.sh --no-node             # skip a variant
```

Requires `go`, `clang`, `node`, a JDK 21 or newer (`javac` and `java`, for the
virtual threads), and `python3` (which drives the timing). `--no-java` skips the
Java server. Binaries land in `benchmark/bin/` (not checked in).

A load pass is `--requests` requests, or 1,000 per connection when that is
more, so 1,000 connections run 1,000,000 requests. With fewer, each connection
would send only a few hundred requests, and the pass would end before the
server settles.

`--no-go` skips the server half entirely, because the load generator is written
in Go.

## What each file is

| File | What it is |
|---|---|
| `serve.nio` / `serve.go` / `serve.java` / `serve.js` | the same three routes (`/plaintext`, `/json`, `/users/:id`) over each stack |
| `parse.nio` / `parse.go` / `parse.js` | the same message parsed 200,000 times |
| `load.go` | the load generator, shared by all three servers |

**The load generator is one program, not three.** A client written in each
language would measure the three clients as much as the three servers. It is
written in Go because Go is the only runtime here that can saturate a
single-threaded server from 1,000 connections without becoming the bottleneck
itself. It speaks HTTP/1.1 over raw TCP with keep-alive and no pipelining. A
pipelined client would measure the servers' read buffering, not their request
paths.

**Only the requests are timed, not the connects.** The generator opens its
connections 64 at a time and sends one request on each before the clock starts.
macOS caps every listen queue at 128 (`kern.ipc.somaxconn`) and resets a
connection that arrives when the queue is full. A burst of 1,000 connects
therefore fails on all three servers alike, and the failure appears at the
first request on a reset connection. A batch well under the cap never fills the
queue. The first response proves that the server accepted the connection, which
a completed dial does not.

**Java runs on the JDK alone, with two settings.** `serve.java` uses the JDK's
own server with one virtual thread per request, and no library from outside the
JDK. So it has no JSON library either, and its `/json` body is the constant text
a serializer would write. The runner passes
`-Dsun.net.httpserver.maxIdleConnections=10000`. By default that server closes
idle keep-alive connections past 200, and the load generator holds every
connection open before the clock starts, so at 1,000 connections the default
gives a refusal, not a measurement. The `Java` column adds
`-XX:ActiveProcessorCount=1`, from which the JVM sizes its collector, compiler
and virtual-thread pools. This is the counterpart of Go's `GOMAXPROCS=1`,
although the JIT and the collector still run threads of their own.
`Java (multicore)` runs without it.

**Every figure is the median of interleaved passes.** Each pass runs every
server once in turn (`--passes`, default 3). Load from outside the benchmark
therefore falls on all of them alike, and a pass that was descheduled drops out
of the median instead of moving it.

Every server binds port 0 and prints the port it got. So this suite needs no
particular port to be free, and two copies of it can run side by side.
`tests/net_test.nio` does the same.

## Sample results

Apple M4 (10 cores), clang 21, Go 1.26, Java 23 (Temurin), Node.js 24.

**`parse`: 200,000 request messages, no sockets** (median of 7 runs):

| | Nio | Go | Node.js |
|---|---:|---:|---:|
| wall | 160 ms | 162 ms | 156 ms |
| peak RSS | **3.3 MB** | 16.2 MB | 51.8 MB |

**`serve`: requests/second, median of 3 interleaved passes; 200,000 keep-alive
requests per pass (1,000,000 at 1,000 connections):**

| Route | Nio | Go | Go (multicore) | Java | Java (multicore) | Node.js |
|---|---:|---:|---:|---:|---:|---:|
| **1 connection** | | | | | | |
| `/plaintext` | 55,318 | 51,979 | 44,569 | 40,903 | 42,169 | 50,974 |
| `/json` | 56,069 | 51,658 | 43,076 | 42,512 | 41,868 | 51,824 |
| `/users/42` | 55,324 | 51,732 | 29,213 | 43,067 | 41,635 | 51,876 |
| peak RSS | **3.6 MB** | 15.4 MB | 19.4 MB | 216.1 MB | 389.6 MB | 69.5 MB |
| **100 connections** | | | | | | |
| `/plaintext` | 173,412 | 159,845 | 185,691 | 140,590 | 181,236 | 139,354 |
| `/json` | 175,240 | 152,020 | 182,333 | 131,227 | 183,947 | 134,522 |
| `/users/42` | 163,347 | 153,747 | 183,984 | 136,170 | 184,146 | 136,090 |
| peak RSS | **5.3 MB** | 16.5 MB | 23.1 MB | 234.0 MB | 478.5 MB | 98.4 MB |
| **1,000 connections** | | | | | | |
| `/plaintext` | 141,507 | 130,263 | 190,034 | 114,801 | 177,769 | 115,065 |
| `/json` | 138,181 | 125,797 | 190,890 | 118,966 | 179,013 | 115,417 |
| `/users/42` | 136,915 | 128,587 | 189,962 | 115,617 | 184,854 | 116,442 |
| peak RSS | **16.7 MB** | 41.7 MB | 61.0 MB | 396.6 MB | 704.1 MB | 227.5 MB |

These were measured on a machine that was busy with other work (load average
9 to 13), which is why the passes are interleaved and the median is used. Go's
single-connection `/users/42` figure is still an outlier caused by that load,
not a property of Go.

Peak RSS is what the kernel charged the server process, taken from `wait4`
after the process is killed, not sampled with `ps`, so it cannot miss a spike
between two samples. Wall clock is a median. Everything on this page runs in
well under a second, and one run descheduled behind a background process moves
a mean by hundreds of milliseconds and a median not at all.

Runs on one machine vary by up to 10% between sessions, so compare columns
measured together, not against a number from another day.

## Reading these

**Nio's parser is level with Go's and Node's.** Most of its time goes to string
search. `find_from` in `lib/string.c`, which is behind `string.find`,
`contains`, `split`, `replace` and `replaceAll`, picks candidate positions with
`memchr`. A `memcmp` at every byte offset of the subject costs about 150 calls
per separator to find a two-byte `\r\n` in a 150-byte header block, and
`split` scans twice. With `memchr`, `string.split` on that block takes 28 ms per
200,000 calls instead of 208 ms, and the whole parse takes 157 ms instead of
320 ms (against about 150 ms for Go and Node). A function that is slow but
correct does not fail a test; only a benchmark like this one shows the cost.

Nio's parser is ordinary Nio over the `string` module. Go's is a native parser
tuned over a decade, and Node's JavaScript is JIT-compiled to something close.
So a level result here shows the language's string primitives doing their job.

**Nio uses one core, so Go and Java are held to one core too, and their
multicore runs are the variants.** Async tasks interleave at await points on
one thread, so `serve.nio` is on a single core whatever the load, and so is
Node. The `Go` column is `GOMAXPROCS=1` and the `Java` column
`-XX:ActiveProcessorCount=1`, which makes the four comparable. The multicore
columns show what Go's goroutine per connection and Java's virtual threads do
with all ten cores, and those numbers are confounded. The load generator is
also a multi-core Go program on the same ten cores, so a multicore server and
the client that measures it compete for the same CPUs, while a single-core
server leaves nine cores to the generator. The multicore columns show what each
gives you in production on an otherwise idle machine. A fair multi-core
comparison needs the generator on another machine. On one core, Nio is ahead of
all three at every connection count here.

**The poller keeps each socket's read side registered.** `lib/net.c` keeps the
read side registered with kqueue or epoll, edge-triggered, from the first read
that parks until the socket closes, as Go's netpoller, tokio and libuv do. If a
socket is registered and removed around every parked read, that is two system
calls per request beside the read and the write that do the work. In that
design, `kevent` was the largest single item in the profile at 26%, at both
1,000 and 64 connections. Keeping the registration was worth about 30% at both
counts: at 1,000 connections, from 113,000 to 146,000 requests a second.

**At 1,000 connections, the remaining cost is the collector and the kernel.**
Nio loses about 19% between 100 and 1,000 connections, where single-core Go
loses about 17%. A profile splits Nio's share of that loss roughly evenly
between two causes. The system calls slow down as the kernel handles more
sockets. And each open connection keeps about 28 small blocks alive, a
collection marks all of them, and the collector's share of the time rises from
under 2% to about 7%. More headroom for the collector was measured and gave no
gain above the noise, so its tuning is unchanged.

**Memory is the widest gap in the tables, and it holds under load.** At 1,000
connections Nio's server peaks at 16.7 MB, against 41.7 for Go, 227.5 for Node
and 396.6 for Java, all on one core. That is about 17 KB per open connection
against 42 KB, 228 KB and 397 KB. At 100 connections it is 5.3 MB against 16.5,
98.4 and 234.0. The multicore runs use more (61.0 MB for Go and 704.1 MB for
Java at 1,000), since per-core allocation caches and collector threads each
hold memory of their own. Java's figures use the JVM's defaults, under which the
heap grows well past the live set before it collects. A JVM tuned for footprint
would be far smaller, and slower. On Nio's side, a precise mark-sweep collector
over per-request garbage that dies immediately keeps the live set small: 1.8 MB
at 1,000 open connections.

## What is not measured

* **TLS.** Nio has no TLS server: the `tls` module is a client only. All four
  servers run plaintext HTTP.
* **Large bodies and streaming.** Every request here is a GET with no body, and
  every response is a few bytes. `http` streams bodies in every direction
  (`http.stream`, `Server.routeStream`, `ClientResponse`), and a `byte[]` is one
  byte per element (§5.7), but this suite exercises neither.
* **Many idle connections.** The 1,000 connections here are all busy. Holding
  many connections open and mostly quiet is a poller benchmark, not a request
  benchmark, and belongs with `net`.
* **Routing at scale.** Three routes are too few to separate Nio's linear scan
  from Go's `ServeMux` trie. The `/users/:id` row measures a parameter binding,
  not a router.
