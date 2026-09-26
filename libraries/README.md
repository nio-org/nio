# libraries/

External libraries: Nio modules that are not part of the language and not part
of the standard library. Nothing in the compiler knows they exist. Each is a
file, reached the way any other module is (§5.4):

```
import './libraries/postgres.nio';    // from the repository root
import 'postgres';                    // from beside the file itself
```

Both give the alias `postgres`, because the default alias of an import is the
file's stem.

This is the third of the three places a module can live:

| | where | what makes it that |
|---|---|---|
| built-in, in C | `build/runtime/lib/*.c` + checker/codegen tables | it needs something only C can reach: a syscall, a descriptor, the clock |
| built-in, in Nio | `build/stdlib/*.nio` | it needs nothing from C, but everyone wants it (`test`, `http`) |
| **external** | **here** | it is a choice, not a given: pure Nio, or Nio over C of its own (§5.8) |

An external library costs nothing to add and nothing to carry: no bake, no
re-bootstrap, no grammar re-emit, no entry in any table. It may import built-in
modules and other files. It may also bind C of its own (§5.8): `native source`
names a C or Objective-C file shipped with the library, `native flags` names
its linker arguments, and `extern` declares the functions the Nio side calls. A
library that does this keeps its native sources beside its `.nio` file and
needs nothing from the compiler. `webview/` is the model.

Some sections below mention nion, a browser built on these libraries. It lives
in a separate repository.

## sqlite/

An embedded SQL database. It is the vendored sqlite3 amalgamation (3.50.4,
the upstream bytes unchanged, tuned by `-D` flags and not by edits) behind a
four-call surface: `open`, `exec`, `query`, `close`. Parameters bind with `?`
from a `String[]` and are never spliced into the SQL, because the first
consumer stores text an attacker can influence. Rows come back as one Json
array, since an extern cannot carry records. Error codes use the 500 block.
The cost is about 8 s of clang per build of any program that imports it (the
amalgamation at `-O2`; there is no object cache). nion's history
(`nion/history.nio`, and HISTORY.md there) is the model consumer.
`sqlite/example.nio` is the runnable example and smoke test, and
`sqlite/README.md` is the map.

## translation/

On-device text translation through the platform's own engine: Apple's
Translation framework on macOS 15+, the models Safari translates with. It is
free, offline once the models are downloaded, and private. The library has
three pieces, because the framework speaks only Swift and `native source`
compiles only C and Objective-C:

- a Nio API (`translation.nio`)
- a dlopen glue file that clang compiles
- a Swift dylib that `translation/build.sh` builds once

The dylib build is the one step that needs swiftc, and only on the machine that
builds the dylib. Programs that import the library still build with clang
alone, and answer "unavailable" until the dylib exists. Work is done in jobs
that the program polls from its own event loop, the shape a webview program
already has. nion's "Translate Page" (`nion/translate.nio`) is the model
consumer. It reaches page text through `webview.evalJsResult`, so the page
bridge stays off. `translation/README.md` has the map.

## postgres.nio

A PostgreSQL client. It speaks the version 3.0 wire protocol directly over
`net` (§6.15), which is its only contact with the outside world.

```
import 'postgres';
import 'json';

void async main() {
    postgres.ClientOptions o = postgres.clientOptions();
    o.user = "postgres";
    o.password = "secret";
    o.database = "postgres";

    postgres.Client pg = postgres.createClient(o);
    await pg.connect() catch e { print("Unable to connect: " + e.message); return; };

    postgres.Result res = await pg.query("SELECT NOW()") catch e { return; };
    print(json.toText(res));

    await pg.disconnect();
}
await main();
```

`libraries/postgres_example.nio` is the longer version of that, and runs against
a default local server.

**What it does.** It connects over TCP or a Unix socket. It authenticates by
`trust`, `password`, `md5` or `scram-sha-256` (the last is what a default
PostgreSQL 14+ install asks for). It runs statements through the simple query
protocol, and answers a `Result` whose rows are `Json` objects keyed by column
name, with each value converted by its type OID. A server-side error, such as a
bad table name or a constraint violation, arrives as an `Error` with code
`ErrorCode.SERVER` and leaves the connection usable.

**What it does not do.** No TLS, so a server configured `hostssl` only cannot
be reached. No connection pool. No bound parameters: the simple query protocol
has none, so a value that goes into a statement goes through
`postgres.quoteLiteral`. The extended protocol (Parse/Bind/Describe/Execute/Sync)
is the natural next thing to add. No `COPY`, no binary result format, no query
cancellation.

**The password hashing is written in Nio, from arithmetic.** SCRAM-SHA-256
needs SHA-256, HMAC and PBKDF2, and `md5` auth needs MD5. The library builds
them without bitwise operators: `xor` is a 64 KiB lookup table built on first
use, and `and`/`or` come from the identity `a + b == (a ^ b) + 2·(a & b)`. They
are checked against the published test vectors for all five primitives. A SCRAM
connection takes about 110 ms against 4 ms for `md5`, nearly all of it PBKDF2 at
PostgreSQL's default 4096 iterations. Nothing after the handshake touches that
code. `sha256`, `md5`, `hmacSha256`, `pbkdf2Sha256`, `base64Encode` and
`base64Decode` are exported. The language now has bitwise operators (§3.1) and a
`crypto` module with SHA-256, HMAC and base64, which this library does not use
yet.

**The SCRAM client nonce.** It is built from the clock, a counter, and (once per
process) the name of a directory that `mkdtemp` made unique. That name is the
only unpredictable input the library uses. Without it the nonce is still
unique, which stops a proof from being replayed; only unguessability is lost.
The fix is to use `crypto.randomBytes`.
