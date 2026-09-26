# sqlite

An embedded SQL database for Nio programs: the vendored sqlite3 amalgamation
behind a four-call surface. It is an external library like `webview/` (§5.8,
`libraries/README.md`). Nothing in the compiler knows it exists, and it needs
no bake and no bootstrap. A program reaches it with an ordinary path import,
and clang compiles the database into the binary.

```nio
import './libraries/sqlite/sqlite';

sqlite.Db db = sqlite.open("app.db") catch e { ... };
sqlite.exec(db, "CREATE TABLE t (name TEXT, n INTEGER)", []) catch e { ... };
sqlite.exec(db, "INSERT INTO t VALUES (?, ?)", ["it's", "42"]) catch e { ... };
Json rows = sqlite.query(db, "SELECT * FROM t WHERE n > ?", ["1"]) catch e { ... };
print(rows[0]["name"] as String);
sqlite.close(db);
```

`libraries/sqlite/example.nio` is the runnable form of everything on this page
(`nio run libraries/sqlite/example.nio`), and is also the library's smoke test.

## The API

| | |
|---|---|
| `Db open(String path)` | fallible; creates the file if absent; `":memory:"` never touches disk |
| `void exec(Db, String sql, String[] params)` | fallible; DDL/DML: runs every statement in the text, in order |
| `Json query(Db, String sql, String[] params)` | fallible; one SELECT → `[{col: value}, ...]` |
| `void close(Db)` | idempotent; later calls on the handle raise MISUSE |

Rules that follow from this shape:

- **Bind, never splice.** Every `?` binds one element of `params`. The count
  must match exactly, and a mismatch raises MISUSE. The values are texts:
  sqlite's column affinity converts them for INTEGER and REAL columns, so a
  number binds as `string.from(n)`. The parameter exists so that you never
  build SQL by concatenating user text, which is how injection bugs happen.
- **Rows come back as Json.** INTEGER columns arrive exact (the tree's `isint`
  keeps values above 2^53), REAL at round-trip width, and NULL as `null`. A BLOB
  also arrives as `null`: this version has no `byte[]` columns. Store blobs
  encoded (`crypto.hexEncode`) or not at all.
- **Calls block the program.** The language is single-threaded and an extern
  cannot suspend, so a slow query stalls everything, including an event pump.
  Keep queries short and indexed. The busy timeout is small (250 ms), because a
  retry loop in C would freeze the one thread. The caller must handle a BUSY
  raise. Two processes that use one file are serialized through the timeout,
  which is better than the last-save-wins race between two JSON writers.
- **Error codes are the 500 block** (`sqlite.ErrorCode`): CANTOPEN 500,
  SQL 501, BUSY 502, MISUSE 503, FULL 504, CORRUPT 505. This is beside
  webview's 400, as http uses 100, tls 200 and x509 300 (§2.9).

## The files

```
sqlite.nio            the whole surface: externs + fallible wrappers
native/sqlite3.c      vendored amalgamation, verbatim upstream bytes (3.50.4)
native/sqlite3.h      vendored, declared `native source` so it lands beside the IR
native/sqlite_shim.c  the bridge: handle table, binding, rows → JSON text
```

The amalgamation is the upstream version 3.50.4 with no edits. Its tuning comes
from `native flags` instead. The flags reach the compile because the driver
makes one clang invocation (tested: `SQLITE_DQS=0` has an effect). The flags are:

- `SQLITE_THREADSAFE=0`: the language is single-threaded, so no mutexes.
- `SQLITE_OMIT_LOAD_EXTENSION`: a database file must never name code to run.
- `SQLITE_OMIT_DEPRECATED`.
- `SQLITE_DQS=0`: a mistyped column name is an error, not a string.

To upgrade sqlite, copy the two files from the next amalgamation over these.

**The cost is compile time.** The `sqlite3.c` file of about 9 MB is compiled at
`-O2` on every `nio build`/`nio run` of a program that imports the library.
This measured at about 8 s of the example's 9 s total on an M-series laptop.
There is no object cache for it; that would be a compiler feature, not a
library one. Plan for it in edit-run loops, or develop against a stub module
and switch the import at the end.

## The shim, for whoever extends it

The shim follows the webview conventions:

- A database is a slot in a fixed table of 32 (FULL when exceeded).
- Errors are a return code plus a message buffer that the wrapper reads back
  at once. The program is single-threaded, so "most recent failure" is
  well-defined.
- Negative codes are the shim's own (-1 bad handle, -3 parameter mismatch,
  -1000 table full), positive ones are sqlite's, and `codeFor` in `sqlite.nio`
  maps both onto the enum.

The JSON result is built in malloc'd scratch memory, with no GC value held while
it grows, and crosses to Nio in a single `rt_str_alloc` at the end. So the shim
needs no `GCFrame`. A new function that holds a `Str*`/`Arr*` across an
allocation must add one (the `ADDER_C` fixture in `tests/ffi_e2e_test.nio` is
the model). `query` refuses a second statement (MISUSE) and does not drop it
silently. `exec` is the multi-statement form.
