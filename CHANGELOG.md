# Changelog

## 0.1.0

The first release of Nio, a statically typed language that compiles to native
binaries. Prebuilt for macOS (Apple Silicon), Linux and Windows. You need
`clang` to build programs.

- `nio run app.nio`: compile and run a program
- `nio build app.nio -o app`: build an executable (`--release` for an optimized one)
- `nio test`: run every `*_test.nio`
- `nio format .`: format your code
- `nio doc http`: read a module's documentation
- `nio get <package> <version>`: add a dependency
- `nio lsp`: the language server for your editor

Documentation: https://nio-lang.org/docs
