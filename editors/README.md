# Editor support

Editor integration is split so that nothing an editor needs is written twice,
and so that adding a second editor costs a fraction of the first.

```
editors/
  shared/
    nio.tmLanguage.json.in         the grammar TEMPLATE — edit this
    nio.tmLanguage.json            GENERATED from the template + src/
    language-configuration.json    comments, brackets, indentation
  vscode/                          the VS Code extension
    src/extension.ts               the LSP client: starts `nio lsp`, stops it
  sync.sh                          copies shared/ into each editor's tree
```

The language server, `nio lsp`, is not in this directory. It lives in
[`src/lsp/`](../src/lsp/), inside the compiler, because only there can it
decide whether a program is correct with the same answer `nio build` gives.

The grammar is generated, so the highlighted language and the compiled
language cannot drift apart:

```
src/token.nio            keywords                   ─┐
src/checker.nio          types, modules, reserved   ─┼─► tools/grammar.nio ─► nio.tmLanguage.json ─► each editor
nio.tmLanguage.json.in   structure, scopes, order   ─┘
```

```sh
nio run tools/grammar.nio            # regenerate
nio run tools/grammar.nio --check    # fail if stale
./editors/sync.sh                    # copy into each editor
./editors/sync.sh --check            # fail if a copy is stale
```

VS Code cannot read a grammar from outside the extension directory it packages,
so each editor gets a copy of the grammar, not a reference to it.

## What is generated, and what is not

Only what the compiler holds in a table can be generated. Nine of the
grammar's regex alternations are generated. They cover the keywords (split
into control flow, modifiers, type names, constants, `as`, and the deprecated
`function`), the built-in primitive and capitalized type names, the
always-in-scope functions, and the built-in module names. Add a keyword to
`src/token.nio` and it reaches the editor the next time you run
`tools/grammar.nio`.

Everything else is hand-written in the template, because no table holds it:

- How comments, strings and numbers lex. That is control flow in
  `src/lexer.nio`, not data. The escape set is a `switch` in `readString`, so
  the grammar copies it by hand and says so. A `${` that opens a hole in a
  backtick string comes from the lexer's hole stack. The grammar approximates
  it with a begin/end rule that includes the whole grammar inside it.
- The operators. They are in the lexer's maximal-munch switch.
- Everything structural: `type X extends Y`, `enum` and `union` bodies,
  `case T n:`, `x as T`, and the conventions that a capitalized name is a type
  and an ALL_CAPS name is a constant. These come from the parser's grammar, not
  from a list.
- A `union` body is a block of its own. A member is `Type Tag`, and the
  parameter rule reads that as a parameter whenever a comma follows it, so
  without the block every member but the last gets a different colour from its
  neighbour. A tag appears in four places: the declaration, a `case` label, an
  `as`, and a constructor call. It reads the same in all four. The constructor
  call needs a rule of its own, because otherwise the built-in type rule claims
  `Text.String(...)` and the function-call rule claims `Text.Bytes(...)`, and
  two members of one union get two colours. The constructor form is also what a
  hover shows, so the popover and the file agree.
- **A function value** is `[type] (params) -> body`. This puts a type directly
  before a parenthesized list, which is the shape the function-call rule reads
  as a call. Without a special rule, `http.Response (http.Request r) -> ...`
  colours the return type as the function being called (only the built-in types
  avoid this, because they come earlier in the list). The `->` after the
  closing paren is what tells the two apart (a declaration is
  `Type name(params)`, with a name between), so the rule is a lookahead for it.
  A return type that ends in a bracket or a suffix, such as `Future<T>`,
  `Car[]` or `Car?`, needs no lookahead, since what stands before the paren
  there is not a name.
- **The classification.** That `if` is control flow and `const` is a modifier
  is an editorial choice. It lives in `buckets` in `tools/grammar.nio`.

The classification is where drift could come back, so a gap in it is a hard
error. If `tools/grammar.nio` cannot classify a keyword in `src/token.nio`, the
tool stops and names the keyword. A person who adds a keyword to the language
must decide how it highlights.

## Every way this can drift, and what catches it

The generator catches a keyword added with no decision about it.
`tests/grammar_test.nio`, in the fast half of the suite, catches the rest, in
particular a keyword added without regenerating the grammar:

| Failure | Caught by |
|---|---|
| keyword added, not classified | `tools/grammar.nio` refuses to run |
| keyword added, grammar not regenerated | `tests/grammar_test.nio` |
| grammar hand-edited | `tools/grammar.nio --check` |
| an editor's copy stale | `tests/grammar_test.nio`, `sync.sh --check` |
| contextual keyword renamed in the parser | `tests/grammar_test.nio` |

## What lives where, and why

There are three kinds of editor asset, and each reaches different tools:

| Asset | Kind | Reaches |
|---|---|---|
| `nio.tmLanguage.json` | declarative regex data | VS Code, JetBrains, Sublime, Shiki (for the documentation at [nio-lang.org/docs](https://nio-lang.org/docs)) |
| `language-configuration.json` | declarative data | VS Code, JetBrains |
| `nio lsp` | a `nio` subcommand (`src/lsp/`) | every editor with an LSP client |

The grammar is the minimum level of highlighting, not the long-term answer. It
runs on regexes, so it cannot tell a type from a variable, and it is always
wrong in the places where the language is ambiguous to a regex (see *Known
limits*). In exchange, it highlights with no process running, on a file that
does not parse, and where there is no compiler at all: a markdown fence, a diff
view, a documentation page.

Semantic precision comes from the language server. VS Code puts the server's
semantic tokens on top of the TextMate result and does not replace it. For this
reason the tooling of every mainstream language has both, and this grammar is
worth keeping correct.

## The language server

`nio lsp` speaks the Language Server Protocol on stdin/stdout. It is eleven
small modules under [`src/lsp/`](../src/lsp/), split by what each piece must
get right:

| Module | What it is |
|---|---|
| `rpc.nio` | the frame: `Content-Length`, then exactly that many bytes |
| `pos.nio` | byte columns ↔ UTF-16 characters, and how wide a squiggle is when a diagnostic carries no span |
| `uri.nio` | `file://` URIs ↔ filesystem paths, both ways |
| `find.nio` | a line and column → the name of the tree it is inside |
| `def.nio` | that name → where it was declared, in whichever file that is |
| `doc.nio` | the prose written above a declaration, markers off |
| `sig.nio` | a function written the way a declaration writes it |
| `hover.nio` | what to say about that name |
| `complete.nio` | what may be written at that place |
| `form.nio` | a record or an enum written the way a declaration writes it (`nio doc` uses it too) |
| `server.nio` | the protocol: lifecycle, documents, and the five requests |

**Quick fixes** come from the compiler, not from the editor half. A
`diag.Diagnostic` can carry a `Fix`: the span to replace, the text, and the
title to offer. `textDocument/codeAction` answers with the fixes that the
diagnostics in the requested range hold. An editor applies a code action
without asking, so only a site that knows the whole edit may attach one. Today
that is the missing import: the checker records its insertion line while it
reads the imports of a file. The squiggle uses the same span. A diagnostic
knows where the source it names ends, so `server.wire` does not guess the
length of the word under the position. It still guesses for a diagnostic that
carries only a position, and `pos.wordEnd` is for that case.

Two parts of `src/loader.nio` exist for the server:

- **`checkEntry`** is `compileEntry` stopped before code generation. The time
  saved is small: codegen is about 11% of a check (a full pass over this
  compiler's own 25k lines is 79ms, of which 10ms is codegen). But it means an
  editor never builds 160k lines of IR only to discard them.
- **The open-buffer overlay** is a `path → text` map that the loader reads
  instead of the disk. Without it, when you edit an imported module, the
  diagnostics describe the last saved version, which is not the version on the
  screen.

**Diagnostics**: when a document arrives, the server checks it as if it were
the entry file of a program. Each diagnostic goes to the document of the file
it is about. This includes files that are only imported, so a broken helper
shows its error in the helper.

**Hover and completion** both need to turn a line and a column into part of the
tree. A compile walks a whole program and never asks where anything is, and no
node carries an end position, so there is no span to test a position against.
The token's own text takes the place of a span. Its length is the extent of the
thing worth pointing at: an identifier is its name, and a member's property is
the name after the dot. So the search looks for the name that a position is
inside, not for a node that contains it.

For this, every declaration carries a `nameTok` beside the `String name` it
declares. `tok` on a declaration is where the statement begins (its type or its
keyword), and `nameTok` is where the name itself sits. Without it, a hover on a
name where it is declared would find nothing.

Hover is then a map lookup. `checker.Info` already holds a type for every
expression, and `src/loader.nio` returns it. Nothing is derived a second time,
because a second derivation could give a second answer.

Completion has a problem that hover does not: the editor asks for it when the
document does not compile. `string.` is not a program. So completion reads the
text of the line to decide what is being completed, since no tree describes an
unfinished expression. It reads the compiler's tables to decide what the
answers are, which a text scan cannot know. The tree it consults is the last
one that checked, kept per document, because nearly all the names worth
offering were valid a keystroke earlier.

The line is read with the compiler's own lexer, run over that one line. A
backwards scan over bytes cannot say where an expression began. In `f("a)b").`
the scan must know that the `)` inside the quotes is text, and going backwards
a quote looks the same at both ends. With tokens, a path segment carries the
suffixes written after it (`f()`, `xs[0]`, `c.faster(1)`), and the type after
the dot is what the language says those produce. The lexer also handles
`a . b` correctly.

**Doc comments**: the lexer skips comments, and no parse rule mentions one, so
the grammar needs no "and possibly a comment here". But the lexer also collects
them, in source order, and they reach the tree as `ast.Program.comments`. The
attachment rule needs no annotation: the comment block that ends on the line
directly above, with no blank line between.
`token.Comment.ownLine` completes the rule, so `int n = 1; // how many`
documents nothing below itself.

**Which declaration a hover documents** comes from `def.nio`, the same answer
the jump uses. So the prose shown is the prose at the place where `F12` would
land. A separate derivation would give a second answer to one question, and the
two could disagree, most likely across a module boundary. `def.nio` resolves
nothing by name across such a boundary. A record is found by the id that
`types.NewID()` stamped on its declaration, because two modules may each
declare a `Config` and those are different types. A jump to whichever one the
search reached first would be wrong and would look correct.

**A `union`** is the one declaration the editor must write back instead of
reading (§2.11). The parser rewrites a union into a sealed base and one
generated extender per member, and none of the result is text a program may
write. `sealed type Text {}` shows no members. `type Text$String extends Text { String $v; }`
names three things (the extender, the mark, the payload field) that §1.3 keeps
out of every identifier and that the lexer rejects. Neither is useful to a
reader. So `hover.nio` rebuilds the declaration from the base and its
extenders: `union Text { String, byte[] Bytes }`, with a tag left out where the
member takes one from its type. It writes a member as the name it is reached by
and the type it holds, `Text.Bytes(byte[])`. That is how a value goes in, what a
`case` binds, and what an `as` answers. Both sides read three helpers in
`src/checker.nio` (`unionTagsOf`, `unionPayload`, `unionTag`), so the editor and
the compiler agree on what a member is, and neither tests for a `$`.

Four more things follow from this:

- `find.nio` takes a name apart into as many as three segments, because
  `alias.Union.Tag` is the only name in the language with three. It skips the
  payload field's synthesized name, which sits at the member's type. Otherwise
  a caret on the visible `byte[]` would answer `byte[] $v`.
- `def.nio` jumps to the member's line inside the union. This is how the prose
  above a member becomes the prose a hover shows.
- Completion offers the tags after `Union.` and never the extenders' names,
  since nobody can type those names.
- `tests/lsp_test.nio` sweeps every position of a union program and asserts
  that no hover contains a `$`. This covers paths that nobody listed.

**The four names always in scope** are `print`, `printInline`, `getType` and
`Error` (the checker's `reserved`). The checker checks each one by hand at its
call site, not through `builtinSigs`, so no table above describes them.
`builtins.globalShapes` is their written form, in the same way `builtinShapes`
is for a built-in with no signature, and `tests/lsp_test.nio` holds the two
lists to each other in both directions. An argument that a call may leave off
is marked, not written twice: `Error(String message, int code?)`, and likewise
the trailing options record that several built-ins take
(`builtinOptionalTail`). Two forms of one signature would read as overloading,
which §2.7 does not have. The `?` goes on the parameter's name because a `?` on
the type means something else: `int? code` would say the argument may be null,
and `Error("x", null)` is an error. `Error` is also a type, so the name has two
meanings and the position selects one. In type position, the hover shows the
record (`checker.builtinTypes`), which is where the `code` field is worth
showing.

**The three types spelled with a keyword** are `Future<T>`, `Map<K, V>` and
`Function(...)<R>`. Every other type is written as a name, which the parser
records as an `ast.NamedType` and everything downstream resolves from. These
three are keywords (`src/token.nio`), so their nodes carry no name.
`find.Hit.typeKeyword` is the node itself, and the hover shows the type as
written, not only the keyword under the cursor: `Future` alone is already in
the file, and `Future<http.Response>` is what the reader wants to know.
`def.nio` returns nothing for these types. Its last resort is a jump to the
name's own site, and for a keyword that would give hover the prose above the
statement the keyword sits in, as if it were the keyword's own documentation.

Three choices in the server are intentional:

- **The loop is synchronous and there is no debounce.** Messages queue in the
  pipe, so nothing is lost when they are taken one at a time, and a check takes
  milliseconds. A debounce needs a timer, and this loop cannot wait on one
  without blocking the pipe or a non-blocking read the language does not have.
  If a project outgrows this, the debounce belongs in the client, where a timer
  is one line.
- **Every open document is checked again on every change**, not only the
  changed one. A change to an imported module can make its importers'
  diagnostics wrong, and the loader does not report what a program was made of.
  The cost is linear in the number of open documents.
- **Checking stops at the first module that fails.** A module whose check
  failed exports nothing, so continuing would bury one real error under a page
  of "undefined" errors that follow from it.

**Three tables in `src/builtins.nio`** serve the server. Each looks like a
duplicate of an existing table, but the existing ones cannot answer these
questions. `builtinSigs` holds a `types.FuncType` per built-in, which is what a
call is checked against, and nothing more. It cannot say what may follow
`string.`, it has no row for `array`, `map`, `json` and `async`, and a
`types.FuncType` is a list of types with no place for a parameter's name.
`builtinAvailable` cannot help either: it is prose, the end of a diagnostic.
For `process` it reads "exit, getArgs, and the stdout, stderr, stdin, child
groups", which does not split into names.

So:

- **`builtinMemberNames`** is what each module and namespace holds.
- **`builtinParamNames`** is what §6 calls each parameter, because
  `String path.join(String, ...String)` does not say which of the two is the
  first element.
- **`builtinShapes`** is the sentence §6 writes for the functions whose type is
  not a signature: `json.toText` takes anything serializable, `array.push` is
  generic in its element, and the language has no type variable to write.
  Without this table, a hover on `json.toText` finds no signature and shows
  nothing.

More tables can drift, so `tests/lsp_test.nio` holds them to each other in both
directions. Every signature is a listed member. Every listed member has a
signature, a shape, or members of its own. Every signature names exactly as
many parameters as it has. A built-in added without all of this fails the fast
suite.

### Known limits

- **A squiggle sits where the compiler points.** For some diagnostics that is
  not where a person would put it: `undefinedThing()` reports at the `(`,
  because that is the token the checker reports at. To move it, change the
  token the checker chooses. That is a compiler change with its own test
  changes, not a server change.
- **A diagnostic's width can be a guess.** When a diagnostic carries only a
  position, the extent is read off the line: the word at that column, the
  string literal that starts there, or one character. This is correct wherever
  the compiler pointed at a token, which is nearly everywhere.
- **A file imported by two open documents is published twice** with the same
  content, once per check. This is harmless and idempotent. The alternative is
  a project model, which the server does not have.
- **A crash stops the server.** Nio has no `recover`, so a compiler panic on
  some input ends the process, and the client restarts it. The same input would
  crash `nio build` too, and the fix belongs there.
- **`nio lsp` and `nio lsp --stdio` are the same.** The flag is accepted and
  ignored, because every language client passes it and this server has no other
  transport.
- **A parenthesized head is not completed**: `(await f()).` offers nothing.
  Suffixes are followed through a name (`f().`, `xs[0].`, `c.faster(1).`), and a
  group that begins with `(` has no name to attach them to.
- **Completion reads one line.** An expression split across lines, or a
  position inside a block comment whose `/*` is on an earlier line than the
  last parse saw, is read as if the line stood alone. The comment case is
  answered from `ast.Program.comments`, which spans lines correctly for any
  document that has parsed once.
- **A property written apart from its dot** (`a . b`) is missed by hover and by
  the jump. `find.nio` computes the property's column as the one after the dot,
  because the tree keeps no token for it. Completion does not have this limit,
  because it reads tokens.
- **The grammar tells a union's tag from an import alias by its capital.**
  `Text.String` colours the last segment as a member and `shapes.Circle`
  colours it as a type, because the segment before the dot is capitalized in
  one and not in the other. A module imported under a capitalized alias reads
  the wrong way. This is the same kind of guess as "a capitalized name is a
  type". The server has the tables and gets it right in the hover.
- **A local declared on the same line as the position** is not offered. A
  statement has no end token, so the line stands in for one.
- **A jump into the standard library written in Nio answers nothing.** `test`
  is embedded in the compiler (`src/embed/stdlib.nio`), not on the disk, so
  there is no file for an editor to open. No jump is better than a jump to a
  path the machine does not have.
- **A doc comment is prose, and is rendered as markdown.** Nothing checks that
  what it says is true, and nothing reformats it.
- **A generic built-in's signature is a sentence, not a type.**
  `T[] array.copy(T[] a)` and `String json.toText(any v)` are §6's own words,
  written out in `builtins.builtinShapes`, because the language has no type
  variable and no `any`. The checker checks those functions by hand in
  `src/checker.nio`. The hover repeats what the specification says and is not
  derived from anything.
- **Parameter names come from the declaration, so an imported record's method
  loses them** when its module is not among the modules the check reached. The
  types remain, because a `types.FuncType` carries them.

## Decisions in the hand-written half

Each rule in the template carries its reason as a `comment`. Know these before
you edit it:

- **`async`, `override`, `sealed` and `extends` are contextual.** The lexer
  emits `IDENT` and the parser matches the spelling. So they are listed in
  `tools/grammar.nio`, not read from a table, and the drift test checks that
  `src/parser.nio` still spells them that way. They are scoped as modifiers
  anyway, since a variable named `override` is very rare.
- **`function` is scoped `invalid.deprecated`.** It is no longer part of the
  language. It stays in the token table only so that the parser can report its
  removal, so the editor flags it correctly.
- **Numbers** are digits with an optional `.` and more digits, or `0x`/`0X` and
  hexadecimal digits, which is what `readNumber` in `src/lexer.nio` accepts.
  There is no binary, exponent, digit separator or hexadecimal float, so the
  grammar matches none of them.
- **Escapes** are the closed set ``\" \' \` \\ \$ \n \r \t \0`` and `\xNN` with
  exactly two hexadecimal digits. Anything else is scoped
  `invalid.illegal.unknown-escape`. There is no `\u`.
- **Only a backtick string spans lines.** The `"` and `'` rules end at the
  newline. This stops one unterminated quote from colouring the rest of the
  file wrongly.
- **Built-in module names are recognized only before a `.`**, and even then it
  is a guess: a file that imports no `path` may name a variable `path`, which no
  grammar can know. The rule comes before `#modifiers`, so that `async.run(…)`
  reads as a module and `int async f()` as a modifier.
- **Rule order matters** in three more places. `#keywords` comes before
  `#function-call`, so `if (` is not a call. `#screaming-constants` comes before
  `#type-names`, so `NOT_FOUND` is a constant and not a type. Within
  `#operators`, the longest spelling comes first.

## Known limits

These limits come from regex highlighting. The language server fixes them; a
better grammar would not:

- `<` and `>` in `Map<String, int>` are scoped as comparison operators.
- `!` in a fallible type `T!` is scoped as logical not.
- A capitalized identifier is assumed to be a type, and an ALL_CAPS one a
  constant. That is convention, not grammar.
- A lowercase identifier before `(` is scoped as a function whether it is a
  declaration, a call, or neither.
- A parameter whose type is itself a function type
  (`Function(T, T)<bool> cmp`) is not scoped as a parameter: the parens inside
  the type break the `Type name` pair the rule matches on.

## Adding an editor

1. Put anything shared in `shared/`, never in an editor's directory.
2. Add one `sync_one` line to `sync.sh`.
3. Add the copy to the comparison in `tests/grammar_test.nio`, so a stale copy
   fails the suite.
4. Add a row to the table below.

If an editor needs the vocabulary in a different format (a tree-sitter
grammar, a Vim syntax file, a Prism definition), add an emitter to
`tools/grammar.nio`. Do not write a second list of keywords. `extract()`
returns a `Lexicon`: the classified vocabulary, with no regexes or scope names
in it yet. Build on that.

**JetBrains** imports TextMate bundles directly (Settings → Editor → TextMate
Bundles), so `shared/` is most of the work. A plugin is needed only for deeper
integration. **Sublime Text** reads `.tmLanguage` natively. **Neovim**,
**Helix** and **Zed** highlight with tree-sitter, not TextMate, so they get
highlighting from the language server's semantic tokens, or from a tree-sitter
grammar if one is written. A tree-sitter grammar would be a second parser to
keep in step with `src/parser.nio`, which is why there is none.

| Editor | Highlighting | Diagnostics | Hover | Completion | Go to definition | Format |
|---|---|---|---|---|---|---|
| VS Code | ✅ grammar | ✅ | ✅ | ✅ | ✅ | planned (`nio format` from the CLI works today) |
| Neovim / Helix / Zed | via LSP | ✅ config only | ✅ | ✅ | ✅ | planned (`nio format` from the CLI works today) |
| JetBrains | via `shared/` | ✅ server, client planned | ✅ | ✅ | ✅ | planned (`nio format` from the CLI works today) |

Neovim, Helix and Zed need no plugin. Each takes a command and a file type in
its own config, so `nio lsp` already serves them:

```lua
vim.lsp.config.nio = { cmd = { "nio", "lsp" }, filetypes = { "nio" }, root_markers = { ".git" } }
vim.lsp.enable("nio")
```

VS Code cannot register a server from settings, so `editors/vscode` has a
client. It is a [small file](vscode/src/extension.ts) that starts a process and
stops it. It is the only place in this repository with a JavaScript toolchain:
`npm install && npm run build`, with `out/` and `node_modules/` as build
products, not source. The client gets the whole protocol from
`vscode-languageclient`: document synchronization, restarts, cancellation, and
every request type. So hover, completion and the jump behind Cmd+click needed
no change in the client. The server declares the capability and the client
forwards it.

The client does two things beyond starting a process, because the compiler is
a separate program and the client cannot assume anything about it:

- **It probes before connecting.** `nio lsp` with no input exits 0. A compiler
  older than the command treats `lsp` as a file it cannot find and exits 2. So
  a missing or old `nio` produces a notification that says which, instead of a
  server that will not start (which VS Code reports as a broken extension). The
  check reads the exit status, never a string from the output.
- **It can point at a specific compiler.** `nio.server.path` defaults to `nio`
  on PATH and expands `${workspaceFolder}`. Set it when you work on the
  compiler itself: the build in the workspace is the one to serve, and
  *Nio: Restart Language Server* picks up each rebuild.

## Roadmap

1. **Highlighting**: done. Generated from the compiler's own tables.
2. **Structured diagnostics**: done. `src/diag.nio` is the record. The sentence
   the command line prints is derived from it, so the two cannot disagree
   (`tests/diag_test.nio`).
3. **`nio lsp`, diagnostics**: done. `src/lsp/`, above. `tests/lsp_test.nio`
   checks the pieces and `tests/lsp_e2e_test.nio` drives the real binary
   through a real conversation.
4. **A VS Code client**: done. `editors/vscode/src/extension.ts`, above.
5. **Hover**: done. `src/lsp/find.nio` and `src/lsp/hover.nio`, and
   `src/lsp/doc.nio` for the prose above a declaration.
6. **Completion**: done. `src/lsp/complete.nio`.
7. **Go to definition**: done. `src/lsp/def.nio`, across files.
8. **Formatting**: `nio format` exists as a CLI subcommand (`src/format.nio`),
   so it also serves CI and pre-commit hooks. What remains is LSP's
   `textDocument/formatting` in the server, which will call the formatter and
   not implement it a second time.

The server depends on two language primitives. **`process.stdin.readBytes(n)`**
reads a frame body, which is not a line and has no terminator to read to.
**`process.stdout.flush()`** sends a reply at once. Without it the reply stays
in this program's buffer while the editor waits for it.

The server also depends on three facts in the tree: **`nameTok`** on every
declaration (where the name is, as opposed to where the statement starts),
**`bindTok`** on a type case (where `case Car c:` writes the `c`), and
**`ast.Program.comments`**. None of them changes what a compile does. All three
make a question about a place answerable.

## Publishing

The extension is not published yet. `npm install && npx @vscode/vsce package`
produces a working `.vsix`. `vscode:prepublish` typechecks and builds a
minified bundle first, so packaging cannot ship a stale or ill-typed `out/`,
and `npx` means the packaging tool is never committed. `package.json` names
the publisher (`nio-lang`) and the license. What remains is the publisher
account on the marketplace and a 128×128 icon.

The extension does not bundle a `nio` binary per platform (one `.vsix` per
target, each carrying a compiler). It requires `nio` on PATH.

The reason is correctness. A bundled server one release behind would report
diagnostics from a different language than the one that compiles the user's
code, which is the drift this directory is arranged to prevent. Anyone who
writes Nio already has the compiler, so the usual argument for bundling does
not apply. The client checks the version at activation, so an old `nio` says
so plainly instead of failing as a broken server.

Publish to **Open VSX** as well as Microsoft's marketplace. VSCodium, Cursor
and Gitpod do not use Microsoft's.
