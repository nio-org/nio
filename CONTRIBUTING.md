# Contributing to Nio

Thank you for helping. This page explains how to build the compiler, how it is
put together, and the few rules that are easy to break by accident.

[specs.md](specs.md) is the language reference, and it is authoritative: when
the compiler and the specification disagree, the compiler is wrong. The user
documentation is at https://nio-lang.org/docs.

## What you need

`clang` on your `PATH`. Nothing else, except inside `editors/vscode/`, which
uses npm and is optional.

## Getting a compiler

The compiler is written in Nio, so you need a compiler to build one.
`bootstrap/nio.ll` is the compiler as portable LLVM IR, which clang can build
on its own:

```sh
sh bootstrap/build.sh                    # ./nio from bootstrap/nio.ll
./nio build --release src/nio.nio -o nio # the compiler builds itself
```

If that fails, `bootstrap/README.md` explains what to do.

## The edit loop

```sh
nio build src/nio.nio -o nio                   # about 3 seconds without --release
nio run tests/unit_main.nio                    # the fast tests, under a second
nio run tests/unit_main.nio --filter lexer     # only the tests whose title has "lexer"
nio run tests/all.nio                          # everything, about 4.5 minutes
nio format .                                   # after any .nio edit
```

`nio emit file.nio` prints the generated LLVM IR. It is the most useful
debugging tool in the repository.

## How the compiler is put together

```
lexer → parser → checker → codegen → driver (clang + runtime)
```

Each stage is one module at the top level of `src/`. Beside them:

- `src/lsp/`: the language server behind `nio lsp`.
- `src/lib/`: small helpers that several stages share.
- `src/embed/`: two generated files (see rule 1 below).
- `build/runtime/`: the runtime, in C. `runtime.c` is the core (memory,
  strings, arrays, printing, the async scheduler), `gc.c` is the garbage
  collector, and `lib/` has one file per standard library module. A program
  links a `lib/` file only when it imports that module.
- `build/stdlib/`: the standard library modules written in Nio: `test`, `http`,
  `tls` and `x509`.

The checker gives codegen its results in `checker.Info`, a set of tables keyed
by node id. The parser gives each syntax tree node an id, so code that creates
a node must give it one too.

`src/codegen.nio` and `build/runtime/runtime.h` describe how values are laid
out in memory, in their header comments. They are two halves of one contract:
when you change one, change the other in the same commit.

## The rules

**1. Regenerate the artifacts.** Three things are generated, and a test fails
if one is out of date:

| You changed | Run |
| --- | --- |
| anything under `src/` | `nio run tools/bootstrap.nio` |
| `build/runtime/*` or `build/stdlib/*` | `nio run tools/bake.nio`, then `nio run tools/bootstrap.nio` |
| a keyword, built-in type, built-in module or reserved name | `nio run tools/grammar.nio && ./editors/sync.sh` |

The compiler writes a program's runtime from the copy in `src/embed/`, not from
`build/runtime/`. A C change that is not baked has no effect, so re-bake before
you debug.

**2. A new language feature used inside the compiler takes two commits.**
The checked-in `bootstrap/nio.ll` only understands the language of the `src/`
it was generated from. Commit A adds the feature without using it and
re-bootstraps. Commit B uses it. If both happen in one commit, the previous
commit's compiler cannot build the next one, and `tools/walkforward.sh`
reports it.

**3. Keep every live pointer in a root.** The garbage collector is precise: it
finds pointers only where the compiler says they are. In the runtime's C code,
a pointer that is live across an allocation must be in a root. Named values are
roots automatically; intermediate values need an explicit `protect`.
`NIO_GC_STRESS=1` collects at every allocation and finds a missing root, and
`nio run tests/all.nio` must pass with it set.

**4. Format your code.** `nio format .` writes every `.nio` file in the one
canonical style, and a test fails if a file is not formatted. The formatter
changes only whitespace and keeps the line breaks you wrote.

## Tests

The test suite is written in Nio, in `tests/`. The unit half imports the
compiler's modules and calls them directly. The end-to-end half compiles small
programs, runs them, and checks what they print.

- `tests/lang_test.nio` is where the behavior of a language feature is
  recorded. A new feature needs tests there.
- Tests never use a fixed network port. They bind port 0 and ask which port
  they got, so the suite needs no network and can run twice at once.
- `tests/golden/` holds frozen reference answers that cannot be regenerated.
  A change that these files reject changes established behavior, and needs a
  reason.
- Three tests check that the repository can still build itself: `fixpoint`
  (the compiler builds itself twice and both results must be identical),
  `bootstrap` and `bake`.

## Documenting what you write

A doc comment is the block of `//` lines directly above a declaration. It is
plain text, the editor shows it on hover, and `nio doc` prints it.

Comments state only facts that a reader needs, and do not repeat what the code
says. When a decision was made against an obvious alternative, the comment
names the alternative and the reason.

## Sending a change

1. Run `nio format .`.
2. Regenerate every artifact your change touched (see rule 1).
3. Run `nio run tests/all.nio`. It must pass.
4. If the language changed, update `specs.md`. If a user would notice the
   change, add a line to `CHANGELOG.md` and say in the pull request which
   documentation pages it affects.

Nio is before 1.0, so a breaking change is allowed when it makes the language
better. Raise design problems now, while they can still be fixed.

To report a security problem, use GitHub's private vulnerability reporting
instead of a public issue.

## A good first change

- A diagnostic. `src/diag.nio` holds the diagnostic record and `src/render.nio`
  the text a person reads. A message that reads badly is a real defect, and
  `tests/checker_errors_test.nio` makes the change safe.
- A test. `nio run --coverage tests/all.nio` writes `coverage.lcov`, which shows
  what the suite does not reach.

Avoid starting with the garbage collector, the contract between codegen and the
runtime, or the bootstrap. Read the header comments in `src/codegen.nio` and
`build/runtime/runtime.h` first.

## Cutting a release

For maintainers. `sh tools/release.sh` sets the version in `src/version.nio`,
`README.md` and `CHANGELOG.md`, re-bootstraps, runs the tests, then asks before
it commits, tags `vX.Y.Z` and pushes. The tag starts
`.github/workflows/release.yml`, which builds `nio` for macOS, Linux and
Windows and publishes them as a GitHub release, with the changelog entry as the
release notes.
