# Async benchmarks

Micro-benchmarks for the async/await machinery, comparing Nio against Go
and Node.js. Each benchmark exists in three files (`.nio`, `.go`, `.js`),
written the same way, so the comparison measures the concurrency runtime, not
algorithm choices. Every variant prints a checksum, and the runner refuses to
time anything if the outputs disagree.

C, C++, and Python are not in this suite. The comparison is between async/await
runtimes, and the three here are one of each kind. The caveats below say what
each model is.

| Benchmark | What it stresses |
|---|---|
| `spawn` | task creation and completion in bulk (100 waves of 10,000 concurrent tasks; a million short-lived futures and frames) |
| `chain` | one spawn + suspend + resume round trip, sequentially (a million awaits of a fresh task inside an async function) |
| `sleepers` | timers (10,000 tasks concurrently sleeping 1 ms: timer heap, wakeups, parked tasks) |

## Running

```sh
benchmark/async/run.sh            # average of 5 runs each
benchmark/async/run.sh 10         # average of 10
benchmark/async/run.sh --no-node  # skip a variant
```

Requires `go`, `clang`, `node`, and `python3` (which drives the timing).
Binaries land in `benchmark/bin/` (not checked in).

## Sample results

Apple M4, clang 17, Go 1.26, Node.js 22. Average of 5 runs, wall clock:

| Benchmark | Nio | Go | Node.js |
|---|---:|---:|---:|
| `spawn` | 36 ms | 140 ms | 69 ms |
| `chain` | 22 ms | 223 ms | 51 ms |
| `sleepers` | 5 ms | 7 ms | 33 ms |

Peak RSS, max over the runs:

| Benchmark | Nio | Go | Node.js |
|---|---:|---:|---:|
| `spawn` | 7.6 MB | 12.5 MB | 79.3 MB |
| `chain` | 2.6 MB | 10.7 MB | 45.9 MB |
| `sleepers` | 3.9 MB | 22.7 MB | 62.6 MB |

Nio's task is the cheapest of the three. A spawn is one heap frame plus one
future block, a suspension stores an integer and returns, and a wakeup is a
queue push. There is no stack, no context switch, and no microtask machinery.
`spawn` and `chain` measure this, and the state-machine compilation strategy
(the same one JavaScript engines and Rust use) is why Nio wins them. On
`sleepers`, Nio and Go are both at the floor of the benchmark (about 1 ms of
real sleep plus bookkeeping). Node's gap is mostly its higher fixed costs,
which also show in every RSS column.

## Caveats: these are not the same concurrency model

- **Nio**: compiled state machines on a single-threaded FIFO scheduler.
  Tasks interleave only at `await`. Nothing preempts, and nothing runs in
  parallel. This is the cheapest model, and these benchmarks favour it: none
  of them would benefit from parallelism.
- **Go**: goroutines are preemptively scheduled, carry growable stacks, and
  run in parallel across OS threads. `chain` is its worst case (spawn,
  cross-goroutine handoff, and join on each iteration), and that cost pays for
  capabilities these benchmarks never use. Go would win any benchmark with real
  CPU work to parallelize.
- **Node.js**: V8 promises on the libuv event loop. It is single-threaded like
  Nio, with generator-based async functions and a microtask queue. It is the
  model closest to Nio's, and the most direct comparison here.

Wall-clock time and peak RSS of the whole process, from per-child rusage via
`os.wait4`. Each process pays its runtime's startup inside the number (about
30 ms of Node's columns is Node starting up, none of it async cost).
