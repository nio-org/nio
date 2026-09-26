<p align="center">
  <img src=".github/assets/logo.svg" alt="Nio" width="120" height="120">
</p>

<h1 align="center">Nio</h1>

<p align="center">
  <strong>Fast, safe programs for the back end, CLIs and webview apps.</strong>
</p>

<p align="center">
  <a href="https://nio-lang.org/docs/versions"><img alt="Version" src="https://img.shields.io/badge/version-0.1.0-22c55e"></a>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-Apache--2.0%20with%20LLVM%20exceptions-16a34a"></a>
  <img alt="Platforms" src="https://img.shields.io/badge/platforms-macOS%20%7C%20Linux%20%7C%20Windows-4ade80">
</p>

<p align="center">
  <a href="https://nio-lang.org">Website</a> ·
  <a href="https://nio-lang.org/docs/installation">Install</a> ·
  <a href="https://nio-lang.org/docs/quickstart">Take the tour</a> ·
  <a href="https://nio-lang.org/docs">Documentation</a> ·
  <a href="specs.md">Language reference</a> ·
  <a href="https://nio-lang.org/playground">Playground</a>
</p>

---

Nio is a small, statically typed language. It compiles your code to **one
native program**, finds mistakes **before it runs**, and comes with an **HTTP
server, TLS and cryptography** in the box.

> [!NOTE]
> The current version is **0.1.0**. Nio is ready to try, but it can still
> change in ways that break existing code before 1.0.

## A complete web service

```
import 'http';
import 'json';

type User {
    String? id;
    String name;
}

http.Response showUser(http.Request req) {
    User u = { id: req.params["id"], name: "Ada" };
    return http.json(200, json.toText(u));
}

http.Server app = http.server();
app.route(http.Method.GET, "/", http.Response (http.Request r) -> http.text(200, "hello"));
app.route(http.Method.GET, "/users/:id", http.Response (http.Request r) -> showUser(r));

await app.listen("0.0.0.0", 8080, null) catch e {
    print(`cannot listen: ${e.message}`);
};
```

`nio build --release server.nio` turns it into a single file you can copy to
any server.

## Why Nio

<table>
  <tr>
    <td width="50%" valign="top">
      <h3>⚡ Native speed</h3>
      Nio compiles through LLVM to one native executable. There is no virtual
      machine and nothing to install where it runs.
    </td>
    <td width="50%" valign="top">
      <h3>🛡️ Types that catch mistakes</h3>
      Every value is checked before the program runs. A <code>String</code> is
      never null: a value that can be missing is a <code>String?</code>, and
      you must check it first.
    </td>
  </tr>
  <tr>
    <td valign="top">
      <h3>🎯 Errors you cannot forget</h3>
      The compiler works out which functions can fail. Errors go up to the
      caller by themselves, and <code>catch</code> handles them where it makes
      sense. No exceptions and no hidden jumps.
    </td>
    <td valign="top">
      <h3>🔀 Async on one thread</h3>
      <code>async</code> and <code>await</code> let one program serve many
      connections at once. There are no threads, so there are no locks and no
      data races.
    </td>
  </tr>
  <tr>
    <td valign="top">
      <h3>🌐 Built for the back end</h3>
      An HTTP server and client, TLS 1.3, cryptography, JSON, sockets, files,
      processes and regular expressions are all in the standard library.
    </td>
    <td valign="top">
      <h3>🪶 Light on memory</h3>
      Each value uses only its own width, and the garbage collector gives
      memory back to the system when the program goes quiet.
    </td>
  </tr>
  <tr>
    <td valign="top">
      <h3>🔒 Safe by default</h3>
      TLS checks certificates unless you say not to. Regular expressions run
      in linear time. Packages are checked against a hash on every build and
      never run code when you install them.
    </td>
    <td valign="top">
      <h3>🧰 Tools included</h3>
      A formatter, a test runner, a documentation viewer, debugger support and
      a language server for your editor, all in the one <code>nio</code>
      command.
    </td>
  </tr>
</table>

## Mistakes show up early

Records turn into JSON and back with one call. A field that can be missing
says so in its type, and a function that can fail needs no special signature:
its errors go up to the caller, and `catch` handles them where you choose.

```
import 'fs';
import 'json';
import 'string';

type Config {
    String host;
    int port;
    String? name;    // may be missing: the compiler makes you check
}

Config load(String file) {
    byte[] raw = fs.readFile(file);    // can fail: the error goes up
    return json.parse(string.fromByteArray(raw)) as Config;
}

Config? cfg = load("config.json") catch null;
if (cfg != null) {
    print(`${cfg.host}:${cfg.port}`);    // localhost:8080
}
```

## Performance

The same small programs, written line for line in Nio, C, Go, Java and
Node.js, each checked to print the same result before it is timed.

| | | |
|:---:|---|---|
| **≈ C** | on calls and math | `fib`, `primes` and `mandelbrot` take 0.93 to 0.98 of the time C takes |
| **2×** | faster than C at allocating | `records`: 34 ms, against 72 ms with `malloc` and `free` |
| **10×** | faster async than Go | `chain`: 23 ms, against 226 ms for Go and 54 ms for Node.js |
| **16.7 MB** | for 1,000 HTTP connections | at 139k requests a second; on one core Go uses 42 MB, Node.js 228 MB and Java 397 MB |

Measured on an Apple M4 with the programs in [`benchmark/`](benchmark/). Small
programs that each stress one thing, so a real application will differ. The
full results are on the [website](https://nio-lang.org/#benchmarks).

## Install

Nio needs `clang` to build programs:

| Platform | clang comes from |
|---|---|
| macOS | the Xcode command line tools: `xcode-select --install` |
| Linux | the `clang` package of your distribution |
| Windows | LLVM and the Visual Studio Build Tools |

Then install the compiler. On macOS and Linux:

```sh
curl -fsSL https://github.com/nio-org/nio/releases/latest/download/install.sh | sh
```

On Windows, in PowerShell:

```powershell
irm https://github.com/nio-org/nio/releases/latest/download/install.ps1 | iex
```

Prebuilt releases are available for macOS (Apple Silicon), Linux (x86-64) and
Windows (x86-64). The [installation guide](https://nio-lang.org/docs/installation)
has the details.

## From source to server

```sh
nio run app.nio                       # compile and run while you work
nio build --release app.nio -o app    # build an optimized native program
./app                                 # copy the one file to a server and run it
```

The `nio` command does the rest of the work too:

```sh
nio format .                          # format your code
nio test                              # run every *_test.nio file
nio doc http                          # read a module's documentation
nio get github.com/owner/repo v1.2.3  # add a package
nio lsp                               # the language server for your editor
```

## Build from source

The compiler is written in Nio, and `clang` is the only thing it needs.
`bootstrap/` holds the compiler as portable LLVM IR, so clang can build the
first copy on its own:

```sh
sh bootstrap/build.sh                      # writes ./nio
./nio build --release src/nio.nio -o nio   # the compiler builds itself
./nio run tests/all.nio                    # the full test suite
```

On Windows, run these in Git Bash.

<details>
<summary><strong>What is in this repository</strong></summary>

<br>

| Path | What it holds |
|---|---|
| [`src/`](src/) | the compiler, including the language server |
| [`build/runtime/`](build/runtime/) | the runtime and the standard library written in C |
| [`build/stdlib/`](build/stdlib/) | the standard library modules written in Nio: `http`, `tls`, `x509`, `test` |
| [`bootstrap/`](bootstrap/) | the compiler as LLVM IR, for building it the first time |
| [`tests/`](tests/) | the test suite, written in Nio |
| [`editors/`](editors/) | syntax highlighting and the VS Code extension |
| [`libraries/`](libraries/) | example libraries that bind C code: SQLite, PostgreSQL, a webview |
| [`benchmark/`](benchmark/) | the same programs in Nio, C, C++, Go, Java, JavaScript and Python |

</details>

## Contributing

Contributions are welcome. [CONTRIBUTING.md](CONTRIBUTING.md) explains how the
compiler is put together and the few rules that keep it able to build itself.
[specs.md](specs.md) is the complete language reference, and
[CHANGELOG.md](CHANGELOG.md) records what each release changed.

## License

Nio is licensed under the [Apache License v2.0 with LLVM Exceptions](LICENSE).
See also [NOTICE](NOTICE). The exception means that programs built with Nio,
which contain parts of its runtime, do not have to carry Nio's license notices.
