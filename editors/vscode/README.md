# Nio for VS Code

Syntax highlighting, live error checking, hover, completion and go-to-definition
for the [Nio](https://github.com/nio-org/nio) programming language.

## Features

- **Errors as you type**, from the compiler itself. `nio lsp` is a subcommand
  of `nio`, so what the editor underlines is what `nio build` would refuse.
  Errors in a file you have imported but not saved are included, and appear on
  that file.
- **Hover** shows the type of anything: a variable, a parameter, a field, a
  function's signature, an enum member's constant, a record's whole
  declaration. It also shows the comment written directly above the
  declaration, in whichever file that is.
- **Go to definition** on Cmd+click or `F12`, including into a module you
  imported: a function, a type, a field, a method, an enum member, a local, a
  parameter.
- **Completion**: `string.` lists what the module can do, with real
  signatures. `car.` lists a record's fields and methods, `latest().` lists what
  the call returns, and `Status.` lists an enum's members. A bare name offers
  what is in scope: locals, then the module, then the keywords and built-in
  types.
- Syntax highlighting for `.nio` files, generated from the compiler's own token
  table, not written by hand.
- Function parameters scoped as `variable.parameter`, so `int fib(int n)`
  colours `n` the way your theme colours a parameter in any other language.
- `Cmd+/` line and block comments, bracket matching, auto-closing pairs.
- Indentation rules for braces.
- `function` highlighted as deprecated, because it was removed from the
  language.
- Invalid string escapes highlighted as errors. The escape set is closed:
  ``\" \' \` \\ \$ \n \r \t \0`` and `\xNN` with two hexadecimal digits.

`editors/README.md` has the roadmap and the known limits.

## Requirements

`nio` on your `PATH`. The extension bundles no compiler. A bundled compiler one
release behind would report errors from a different language than the one that
compiles your code, and anyone who writes Nio already has a compiler.

If yours is somewhere else, or you want a specific one:

```jsonc
{
  // working on the compiler itself: serve the build in this workspace
  "nio.server.path": "${workspaceFolder}/nio"
}
```

If `nio` is missing or too old, a notification says so, and the extension does
not fail as a broken extension. The extension asks the binary whether it speaks
the protocol before it points a client at it.

## Settings and commands

| Setting | Default | |
|---|---|---|
| `nio.server.path` | `nio` | which compiler serves this workspace; `${workspaceFolder}` is expanded |
| `nio.trace.server` | `off` | log the conversation with the server, for debugging it |

**Nio: Restart Language Server** (`Cmd+Shift+P`) restarts the server. Use it
after you rebuild `nio`: the running server is the old binary until it is
replaced. Changing `nio.server.path` restarts it for you.

## Install from source

The grammar is generated into this directory from `editors/shared/`, and the
extension has a runtime that must be built, so do both:

```sh
./editors/sync.sh                            # copy the generated grammar in
cd editors/vscode && npm install && npm run build
ln -s "$PWD" ~/.vscode/extensions/nio
```

Then `Cmd+Shift+P` → *Developer: Reload Window*.

`node_modules/` and `out/` are build products and are not in the repository, so
repeat the two `npm` steps after a fresh clone.

## Working on the extension

```sh
npm run watch        # rebuild out/extension.js as src/ changes
npm run typecheck    # esbuild strips types without checking them; this checks
```

`npm run watch` does not reload the extension, because VS Code loads it once.
After a rebuild, run *Developer: Reload Window*, or press `F5` to open an
Extension Development Host that runs the current source.

The client ([`src/extension.ts`](src/extension.ts)) is thin: it starts a
process and stops it. Everything about the language is on the other side of the
pipe, in `src/lsp/`, compiled from the same source as `nio build`. The client
gets hover, completion and go-to-definition with no code of its own: the server
declares the capability and `vscode-languageclient` forwards it. That is why
the extension has this one dependency.

## Editing the grammar

Do not edit `syntaxes/nio.tmLanguage.json` here. It is a copy of a generated
file, two steps from its source:

```sh
# 1. edit the template
$EDITOR editors/shared/nio.tmLanguage.json.in
# 2. regenerate (fills the keyword lists from src/token.nio and src/checker.nio)
nio run tools/grammar.nio
# 3. copy into this extension
./editors/sync.sh
```

The keyword, built-in type and built-in module lists are not in the template.
They are read from the compiler's own tables, so a keyword added to the
language reaches the editor at step 2. See `editors/README.md`.

`language-configuration.json` is a copy from `editors/shared/`, but it is not
generated. Edit it there and run step 3.

To see the scopes the grammar assigns at the cursor, use `Cmd+Shift+P` →
*Developer: Inspect Editor Tokens and Scopes*.

## Packaging

```sh
npm install && npx @vscode/vsce package
```

`vscode:prepublish` typechecks and builds a minified bundle, so packaging cannot
ship a stale or ill-typed `out/`. `.vscodeignore` keeps `src/`, `node_modules/`
and the build files out of the `.vsix`. Everything needed is in the one bundled
file.

Before publishing, make sure `publisher` in `package.json` is the ID of your
marketplace publisher account. The license is declared in `package.json`, but
`vsce` also looks for a LICENSE file in this directory and warns when there is
none. The repository's LICENSE file is at the root.
