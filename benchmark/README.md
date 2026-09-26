# Benchmarks

Micro-benchmarks that compare Nio against C (`clang -O2`, the same backend and
optimization level `nio` uses), C++ (`clang++ -O2`), Go, Java, and Node.js. Each
benchmark exists in seven files (`.nio`, `.c`, `.cpp`, `.go`, `.java`, `.js`,
`.py`),
written statement for statement the same way. So the comparison measures code
generation, not algorithm choices. The exception is `files`, where each program
mostly calls its language's regexp engine and file-system library. Every
variant prints a checksum, and the runner refuses to time anything if the
outputs disagree. Python is opt-in (`--python`). Its total is about 20x the
total of all the others and it would dominate a run, so the `.py` files are
kept as reference implementations but are not timed by default.

| Benchmark | What it stresses |
|---|---|
| `loops` | raw loop and integer-`%` throughput (100,000,000 iterations over a 10,000-element array) |
| `fib` | function call overhead (recursive Fibonacci of 35) |
| `primes` | integer loops, `%`, comparisons (trial division below 2,000,000) |
| `mandelbrot` | float arithmetic (escape-time over a 1000×1000 grid) |
| `records` | record/struct allocation and field access (2,000,000 nested segment records through a ring buffer) |
| `strings` | string allocation, copying, equality (a 32-char string built by concatenation, 100,000 times) |
| `files` | file I/O, regexp matching, string surgery (an 8.6 KB file read, searched, edited, and written 1,000 times) |

The async/await machinery has its own suite against Go and Node.js in
[`async/`](async/README.md), with its own runner (`benchmark/async/run.sh`).
The `http` module has one in [`http/`](http/README.md), with a third runner
(`benchmark/http/run.sh`). The HTTP suite is the only one here that is not a
program that prints a checksum. Half of it is a real server over a real socket,
driven by one shared load generator, so that the comparison is between three
servers and not between three clients.

## Running

```sh
benchmark/run.sh                  # median of 5 runs each
benchmark/run.sh 10               # median of 10
benchmark/run.sh --python         # add Python back
benchmark/run.sh 10 --no-node     # skip a variant entirely
benchmark/run.sh --no-java        # ...Java included
```

A default 5-run pass takes about 25 seconds. `--python` adds more than three
minutes: `loops.py`, `mandelbrot.py` and `primes.py` take about 9, 10 and 8
seconds per run, against about 0.1 seconds for the others. That is why it is off
by default. `--no-c`, `--no-cpp`, `--no-go`, `--no-java` and `--no-node` drop those variants
from the build and the checksum check as well as from the tables. If C is
skipped, the ratio row uses Nio as its base.

Requires `go`, `clang` (whose `clang++` driver builds the C++ variants),
a JDK (`javac` and `java`), `node`, and `python3` (`python3` drives the timing, so it is needed even
without `--python`). Binaries are written to `benchmark/bin/` (not
checked in).

## Sample results

Apple M4, clang 17, Go 1.26, Node.js 24. Median of 9 runs, wall clock:

| Benchmark | Nio | C `-O2` | C++ `-O2` | Go | Node.js |
|---|---:|---:|---:|---:|---:|
| `loops` | 57 ms | 33 ms | 33 ms | 132 ms | 117 ms |
| `fib` | 21 ms | 20 ms | 19 ms | 23 ms | 75 ms |
| `primes` | 64 ms | 65 ms | 65 ms | 56 ms | 128 ms |
| `mandelbrot` | 86 ms | 107 ms | 106 ms | 89 ms | 113 ms |
| `records` | 34 ms | 92 ms | 95 ms | 53 ms | 40 ms |
| `strings` | 16 ms | 42 ms | 21 ms | 27 ms | 34 ms |
| `files` | 77 ms | 70 ms | 566 ms | 59 ms | 84 ms |

Peak RSS, max over the runs:

| Benchmark | Nio | C `-O2` | C++ `-O2` | Go | Node.js |
|---|---:|---:|---:|---:|---:|
| `loops` / `fib` / `primes` / `mandelbrot` | 1.4-1.5 MB | 1.4 MB | 1.4 MB | 4.1-4.3 MB | 51-53 MB |
| `records` | 2.9 MB | 1.4 MB | 1.4 MB | 11.3 MB | 55 MB |
| `strings` | 3.1 MB | 1.5 MB | 1.4 MB | 10.7 MB | 54 MB |
| `files` | 3.1 MB | 1.8 MB | 2.3 MB | 10.7 MB | 53 MB |

Java was added later, and was measured on Apple M4, clang 21, Java 23
(Temurin), median of 10 runs:

| Benchmark | Java | Peak RSS |
|---|---:|---:|
| `loops` | 81 ms | 43.2 MB |
| `fib` | 42 ms | 42.8 MB |
| `primes` | 89 ms | 43.0 MB |
| `mandelbrot` | 116 ms | 43.3 MB |
| `records` | 57 ms | 115.8 MB |
| `strings` | 56 ms | 86.8 MB |
| `files` | 178 ms | 77.6 MB |

Every Java variant is a class named `Main`, compiled ahead of time with `javac`
and run with the JVM's default flags. Like the Node.js numbers, these include
the virtual machine itself: about 29 ms of startup and about 42 MB of baseline
RSS, which is most of the difference on `fib`. The JIT also compiles the hot
loops while they run, so a program this short pays for that too. `files` uses
`java.util.regex`, a backtracking engine.

The wall-clock column is a median of interleaved runs. Both parts of that
matter. Every benchmark here finishes in well under a second, so one run that
is descheduled behind another process moves a mean by more than the
differences these tables show. And if one variant ran all nine of its runs
before the next variant started, a machine that got busy halfway through would
put all of the slowdown on whichever language was running then. `files` is the
clearest case: it is 1,000 pairs of system calls, and on an idle machine its
slowest run is often 7x its fastest. Older versions of the harness took a mean,
one variant at a time. A `files` number from those versions is not comparable
with a run of the current script.

On `fib`, `primes`, and `mandelbrot`, Nio is where a straightforward LLVM
frontend should be. It is level with C on `fib` and `primes`, ahead of C on
`mandelbrot` (where the C and C++ variants give up FMA, see below), and close to
Go. C and C++ stay within noise of each other on all three, as expected:
identical loops through the same backend.

`loops` is where Nio is furthest behind C, and the disassembly shows why. clang
keeps `a[i]` in a register for the whole inner loop, unrolls the loop two ways
into independent accumulator chains, and turns `% u` into a multiply-and-shift.
The result is 100,000,000 iterations in 33 ms, about 1.3 cycles each, without a
single SIMD instruction. Nio runs the same loop in 57 ms. That is well ahead of
Go and Node.js, which also check indices, but 1.7x slower than a C loop with no
bounds checks, because every `a[i]` in Nio is a checked access.

The host's target attributes are a large part of this result. LLVM's inliner
does not inline a callee whose target features are not a subset of the
caller's. The C runtime's functions carry `"target-cpu"`/`"target-features"`,
so generated functions must carry the same attributes (`hostTargetAttrs` in
`src/driver.nio`), or no runtime helper is inlined into generated code,
whatever `-flto` does. Without them, `a[i]` compiles to two opaque calls to
`rt_arr_slot` (one for the read and one for the write), which also keeps the
bounds check inside the loop and the element in memory. With the attributes,
`loops` went from 203 ms to 58 ms, `strings` from 326 to 271 (measured at the
200,000-iteration size that benchmark had then, twice its current size), and
`fib` from 20 to 18.

String search in `lib/string.c` has a similar story. `find_from` is behind
every search the language does: `string.find`, `contains`, `split`, `replace`,
`replaceAll`. It picks candidate positions with `memchr` on the first byte of
the separator, which every platform vectorizes. The alternative, a `memcmp` at
every byte offset of the subject, costs about 150 calls per separator to find
the two-byte `\r\n` in a 150-byte header block, and `split` scans twice (once
to count the pieces, once to cut them). With `memchr`, `string.split` on that
block takes 28 ms per 200,000 calls instead of 208 ms, and a whole HTTP request
parse takes 157 ms instead of 320 ms. That is the difference between losing to
Go's `net/http` parser and being level with it (see [`http/`](http/README.md)).
It does not change the benchmarks on this page: `files` spends its search time
in the regexp engine, which has its own scanner. Slow code that gives correct
answers does not fail a test suite, so costs like this one, the target
attributes above, and the `stdio` cost below only appear in benchmarks.

The allocation benchmarks show the memory model. All five programs allocate
hundreds of megabytes and keep almost none of it, and they differ in how the
memory comes back. C and C++ are the slowest on `records`. They pay
`malloc`/`free` (`new`/`delete`) per object, 6 million times each, and a
general-purpose allocator is a poor fit for objects that die in a few
microseconds. The others batch that work. Nio divides its heap into chunks of
same-sized cells and reclaims a chunk whose cells are all unreachable in one
step, without touching the cells. Go has a size-classed bump allocator. V8 has a
generational nursery and rope strings. Rope strings make an expression of `+`
almost free, but do less for the accumulation that `strings` measures, and of
all the languages measured, V8 shows the largest difference between the two
forms. Go and Node.js pay for this in footprint (about 11 MB and 54 MB, against
1.4 MB for C). Nio holds 3.1 MB, which is its 1 MB collection threshold plus
the chunks in use.

`strings` splits the C family in two. C++ finishes in half the time of C,
because the small-string optimization of `std::string` keeps every
intermediate of up to 22 bytes inline (only the last five appends of each build
touch the heap), and because it stores its length where the C variant calls
`strlen` again on every concatenation. It uses the same allocator a third as
often, which puts C++ next to Nio and not next to C.

`files` is the one benchmark that leaves the process, and the only one where
the languages' libraries differ more than their code generation. The 1,000
whole-file reads and writes are the same two syscalls everywhere. What
surrounds them is each language's regexp engine and its way to cut a string in
three and join it again. If `regexp.find` is replaced with `string.find` for the
same literal, the benchmark does not change: the search costs nothing
measurable.

It is the one benchmark that Nio does not win. Two pieces of work in the
library, not the language, brought it to its current number.

The first is that `readFile` and `writeFile` do not use `stdio`. `stdio` cost a
`FILE` buffer that the C library sizes itself, a `stat` before the `open` that
can be an `fstat` after it, and a `fwrite` per 4 KB chunk instead of one
`write`, because a `byte[]` had to be narrowed before anything could write it.
The library now uses the descriptor directly (`open`/`read`/`write`/`close`,
one `write` from a buffer narrowed in full, and the size from `fstat` as the
read buffer's capacity, not its length). This took `fs.writeFile` from about
285 ms to about 97 ms per 1,000 calls in isolation, and the whole benchmark
down about 12%. The gain is smaller than it looks because the benchmark reads as
much as it writes.

The second is the representation of `byte[]`. With one 8-byte slot per byte,
`fs.readFile` of an 8.6 KB file allocated 68 KB and filled it one slot at a
time, and `string.fromByteArray` read it back down at once. That was three
passes and two allocations where Go's `os.ReadFile` does one of each, and it
cost about 13 to 24 µs per file. §5.7 packing removed that cost. Every value
now occupies its own width where it is stored, so a `byte[]` is one byte per
element (a `byte[100000000]` peaks at 101 MB of RSS), `fs.readFile` is a single
`memcpy` into it, and `fs.writeFile` is a single `write` out of it.

The phase timings show what remains. Over the same 1,000 rounds, `fs.readFile`
takes about 30 ms, `fs.writeFile` about 53 ms, and everything the language
itself does takes about 3 ms: the regexp, the three-way cut, the integer parse,
and both conversions between `String` and `byte[]`. The two conversions
together take about 1 ms, which is no longer a measurable cost. This is a
system-call benchmark with a program attached. To move it, make the calls
cheaper or fewer. No representation cost remains under them.

The regexp search is cheap because of a start-byte filter. Without it, the
engine enters the simulation at all 8.6 million byte positions it is given, at
about 5 ns each, and the search alone was 48 ms of a 113 ms run. The engine now
works out at compile time which bytes a match can begin with, and scans for the
next one with `memchr` (`rx_first_bytes` in `build/runtime/lib/regexp.c`). On
this benchmark's pattern, `counter = [0-9]{4}`, whose first byte appears
nowhere else in the document, it reaches the single candidate per pass and
costs about 0.02 ns per byte. Measured on the engine alone, over a 46 KB
subject:

- a pattern with a rare first byte: 240x faster
- `\bcat\b`: 220x
- an alternation of three words: 21x
- a case-insensitive literal: 10x
- a literal whose first byte is on every line: 16x, because the bytes after the
  first are compared directly against the program before the machine starts

A pattern that can begin anywhere gets nothing. `.*zebra` admits every byte but
one, so the filter is not built. Nor is it built for a pattern that can match
the empty string, since such a pattern can match at any position, so no
position may be skipped.

C++ is 7.6x C, and the same substitution shows why. Replace
`std::regex_search` with `std::string::find` and its 566 ms becomes about 120.
So `std::regex` spends about 450 ms, about 50 ns per byte: three orders of
magnitude slower than the filtered engine and one order slower than the
unfiltered one. It is the one row where the C++ variant is far from the C one.
Node.js takes 84 ms with a backtracking engine and a `latin1` decode of every
read.

## Caveats

- Wall-clock time and peak RSS of the whole process (per-child rusage via
  `os.wait4`; median time and max RSS over the runs, taken a round at a time
  across the variants and not a variant at a time). So the Node.js numbers
  include the interpreter itself: about 19 ms of startup and about 44 MB of
  baseline V8 heap. The Java numbers include the JVM in the same way. The runner prints the measured baseline. The startup of the
  native binaries is negligible.
- `loops` is the [loops benchmark from
  bddicken/languages](https://github.com/bddicken/languages/blob/main/loops/c/run.c),
  ported statement for statement. Upstream reads `u` from `argv` and seeds `r`
  from the clock, so that nothing in the loop is known at compile time. This
  suite passes no arguments and checks that the variants print the same thing,
  so both are plain constants here. That gives every compiler a constant
  divisor to strength-reduce, but no variant folds the loop away. All of them
  still run the full 100,000,000 iterations. A folded loop would finish in
  microseconds, not in the 33 ms the fastest variant takes.
- `fib`, `primes`, and `mandelbrot` compute on scalars and barely allocate.
  `records` and `strings` are the allocation benchmarks. There the languages
  differ in how memory comes back: the C and C++ variants `free`/`delete` every
  record and intermediate string they drop, and Nio, Go, and V8 all collect.
  The RSS table is the main result of those two benchmarks.
- `files` is the one benchmark that touches the file system, so its numbers
  include about 2,000 syscalls and whatever the page cache is doing. It is the
  noisiest row here, and the one where a repeat run moves the most. Each variant
  makes its own directory and lets the operating system pick the name, so two
  variants that run at once cannot collide: `mkdtemp` under `/tmp` in C and C++,
  and `os.MkdirTemp`, `fs.mkdtempSync`, `tempfile.mkdtemp` and Nio's
  `fs.createTempDir` under `TMPDIR` elsewhere. Each writes one 8.6 KB file in it
  and removes the directory before it prints, so nothing is left behind. The
  regexp is `counter = [0-9]{4}`, which is inside the syntax all five engines
  share. The counter is written back zero-padded to four digits, so the edit
  never moves the text after it. Every pass therefore finds the match at the
  same offset, which makes the summed offsets a checksum. The line being
  searched for is the last line of the file, so every search reads the whole
  document.
- The `records` ring buffer makes the allocations escape the loop. Without it,
  clang, Go's escape analysis, and V8 can all scalar-replace the objects, and
  the benchmark stops measuring allocation. A linked list would be the more
  natural test, but Nio cannot traverse one yet: that needs null-check
  narrowing (specs §7).
- `nio` links with `-flto`, so clang can inline the runtime's allocator, bounds
  check and string helpers into generated code. That works only because
  generated functions carry the host's target attributes. Each of the two alone
  is worth about 5% or nothing; together they are worth 3.5x on `loops`. The C
  and C++ variants are not built with LTO, and the comparison is still fair.
  Each is a single translation unit, so clang already sees the whole program:
  `-flto` measures within noise of `-O2` on the seven C files, and the C++ files
  gain nothing from it either. LTO is how Nio reaches the position that those
  one-file variants start from.
- C and C++ are compiled with `-ffp-contract=off`, and `mandelbrot.go` writes
  its products as `float64(a*b) + c`. All of these say the same thing: do not
  fuse multiply-add. clang and the Go arm64 backend each emit FMA by default,
  which rounds once where the source rounds twice and changes the Mandelbrot
  checksum. Go without those conversions prints 46510466, against 46510475 for
  all the others. nio's emitted IR and JavaScript semantics both forbid the
  fusion.
- The `.go` files carry a `//go:build ignore` tag. It dates from when the
  compiler was written in Go, when `go build ./...` would have collided on seven
  `package main` files in one directory. There is no `go.mod` now, so the tag
  costs nothing and is left in place. `run.sh` names each file explicitly on
  the `go build` command line, which builds it regardless. Go is an optional
  comparison target here, not a dependency of the repository: `--no-go` drops
  it.
- Every variant is plain, because every variant is meant to be the same
  program. The C++ files write `s = s + "ab"`, not `+=` or `reserve`, and use
  `new`/`delete` per record, not arenas or value types. The `.py` files use no
  `__slots__`, `dataclass`, NumPy, or comprehensions in place of the `while`
  loops. If one of them were tuned to its language's idioms, the benchmark
  would measure something else.
- The loops exit through their conditions, not through a `break`, and the
  Mandelbrot grid coordinates advance with float accumulators, because Nio had
  neither `break` nor an int↔float conversion when these programs were
  written. The other variants mirror this and do not use their idiomatic forms.
  The programs are left as they are so that the comparison stays the same.
