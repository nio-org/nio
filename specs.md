# Nio Language Specification

*v0.1*

A small, statically typed, compiled programming language. Programs are
compiled ahead of time to native executables (see [README.md](README.md) for
toolchain usage). This document specifies the language itself: its lexical
structure, types, expressions, statements, and standard library.

---

## 1. Lexical structure

### 1.1 Source text

Source files are UTF-8 text with the `.nio` extension. Whitespace (spaces,
tabs, newlines) separates tokens but is otherwise insignificant.

Because it is insignificant, there is one canonical way to arrange it, and
`nio format` writes it: four spaces a level, one blank line wherever a file
left one or more, and a trailing comment four spaces past the code it follows.
The formatter has no options. It does not decide where a construct breaks
over lines; the file keeps the breaks it wrote. So it changes no token at all,
except to add the punctuation this grammar lets a file leave out: the `;`
after a statement ending in `}` (§1.7), the `;` after the last field of a body,
and the `,` after the last element of a list whose bracket closes on a line of
its own. The [formatting guide](https://nio-lang.org/docs/formatting)
describes the style.

### 1.2 Comments

```
// a line comment runs from "//" to the end of the line

/* a block comment runs from its opening delimiter to the closing
   one, over as many lines as it needs */

int x = 1 /* and it may sit between tokens */ + 2;
```

Block comments do not nest: the first `*/` closes one, whatever lies between.
A `/*` inside a block comment is therefore ordinary text, and a `*/` inside
one ends it even in what reads as quoted text. A comment is scanned as
characters, not as code. Neither form of comment opens inside a string
literal, and a `//` inside a block comment does not start a line comment. A
block comment that reaches the end of the file without closing is a compile
error.

A comment is not a token and means nothing to a compile: two programs
differing only in their comments produce identical output. Tools read them,
though, and there is one convention for where. A comment block ending on the
line directly above a declaration, with nothing but whitespace before it on
its own lines, documents that declaration. `nio lsp` shows it when you hover
the name, wherever the name is used and from whichever file it was imported.
Either form counts, and consecutive `//` lines are one block. A blank line
ends it, which is how a note about the file stays the file's:

```
// A file comment. The blank line below is what makes it one.

// Adds two numbers.
// Whichever two you pass.
int add(int a, int b) { return a + b; }

int seen = 0;   // a remark about seen, and about nothing after it
```

Nothing checks that what a comment says is true, and nothing requires one.

### 1.3 Identifiers

```
identifier = letter { letter | digit } ;
letter     = "a"…"z" | "A"…"Z" | "_" ;
digit      = "0"…"9" ;
```

Examples: `sum`, `myCar`, `_tmp`, `x9`.

The name of a user-declared type (a record, §2.4, or an enum, §2.5) must
start with an upper-case letter; a name that does not is a compile error.
Everything else (variables, functions, parameters, fields, enum members,
import aliases) is unconstrained. The built-in type names are therefore the
only lower-case names that can appear in a type position. The exception is
`String`, the one built-in type with an upper-case name (the lower-case
`string` names its library module, §6.6).

### 1.4 Keywords

The following words are keywords and cannot be used as identifiers:

```
Function  Future  Map  void  type  enum  return  if  else  switch  case
default  while  for  forEach  break  continue  true  false  null  await
catch  import  as  export  const  function
```

Built-in type names (`int`, `int8`, `int16`, `int32`, `int64`, `uint`,
`uint8`, `byte`, `uint16`, `uint32`, `uint64`, `float`, `float32`,
`float64`, `String`, `bool`, `DateTime`, `Duration`, `Json`, `Error`) are
not keywords, but they cannot be redeclared as record types.
`strict` is not a keyword either: it is a modifier recognized only
immediately before the `as` of an `as` expression (§3.6), so it stays
available as an ordinary name everywhere else.
`async` is not a keyword either, and not a reserved name: the parser
recognizes it as a modifier by its spelling, in the one position where a
modifier can appear (§5.2). So `async` is free to name the module and
anything else, including a function (`async() { … }` declares one).
`extends` and `override` (§2.4) are recognized the same way, each in one
position: between a type's name and its body, and among a method's
modifiers. They are free everywhere else, including as a field's name
(`int override;`) or a method's own (`override() { … }`).
`sealed` (§2.4) is recognized the same way, in the one position
immediately before the `type` keyword of a declaration, and is an
ordinary name everywhere else.
`extern` and `native` (§5.8) are recognized the same way. `extern` is
recognized at the head of a body-less function declaration
(`extern int f(int a);`, the word followed by a return type, a name and
`(`), and `native` immediately before `source` or `flags` at the head of a
native build statement. Both are ordinary names everywhere else, so
`int extern = 3;` still declares a variable and `native = 2;` still assigns
to one.

Keywords are still usable as record field names. A field name appears only
where the grammar already knows one belongs: after the field's type in a
record body (§2.4), before `:` in a record literal, and after `.` or `?.`.
No keyword is ambiguous in those positions:

```
type Item {
    String type;               // "type" is a keyword, and a legal field name
    int for;
}
Item item = { type: "invoice", for: 3 }
print(item.type);
```

Real JSON is full of keys like `"type"`, so refusing them would make whole
payloads impossible to model (§6.3).

`function` is not part of the language, since declarations take no keyword
(§5.2). The word stays reserved so that old code gets a clear migration
error instead of a confusing one.

### 1.5 Reserved names

`print`, `printInline`, `getType`, and `Error` (§2.9) are in scope in every
file and no import introduces them, so declaring one could only hide it: they
cannot be used as variable, function, parameter, or type names.

The standard library's module names (`json`, `time`, `array`, `string`,
`map`, `async`, `os`, `path`, `fs`, `process`) are not reserved. A module's
name is taken only in a file that imports it, where its alias (§5.4) owns it:
no variable, function, type, or parameter there may reuse it, so an alias can
never be shadowed. A file that does not import `path` may name a variable
`path` like any other, and `import 'path' as p` hands the name back to a file
that wants both:

```
String path = "/usr/local";          // no import 'path' here
printInline(path);
```

```
import 'path' as p;
String path = "/usr/local";
printInline(p.join(path, "bin"));
```

Naming a library that the file did not import is an error that says so
(`path is a built-in library; import it first`). The exception is a file that
gave the name a meaning of its own: there, that meaning wins.

This is about names, not types: the built-in type names still cannot be
redeclared as record or enum types (§1.4). The ones that begin with a
capital (`String`, `DateTime`, `Duration`) are exactly the ones whose
libraries are spelled in lower case (`string`, `time`), so a type and a
module never contend for a name. A variable called `String` and the `String`
type can coexist.

None of this reaches a record's members: a field or a method may take any
of these names, since it is only ever reached through a value (`item.print()`
calls the method, `printInline(x)` the built-in).

`self` is reserved by nothing and is an ordinary identifier, except inside
a method body (§2.4), where it is already bound to the receiver.

### 1.6 Literals

| Literal | Form | Examples |
|---|---|---|
| integer | decimal digits, or `0x`/`0X` and hexadecimal digits | `0`, `42`, `0xFF`, `0X10` |
| float | digits `.` digits (the fractional part is required) | `3.14`, `0.5` |
| string | double quotes, single quotes, or backticks | `"hello"`, `'hello'`, `` `hello` ``, `""` |
| bool | `true`, `false` | |
| null | `null` | only meaningful for optional types |

Notes:

- There is no exponent (`1e9`), binary notation, or digit separator yet.
- Hexadecimal is a second spelling of an integer literal, not a second kind
  of value. `0xFF` and `255` are the same literal in every way that matters
  afterwards: the same type, the same range check against a narrower type,
  the same behaviour as a map key, an enum member's value, or a fixed-size
  array's length. Which one to write is a question about the reader. A mask,
  a byte, or a value from a wire format or a published constant table reads
  as hexadecimal, and a count does not.
  Both cases work in the prefix and in the digits, so `0xff`, `0xFF`, `0Xff`
  and `0XFF` are one value. There is no hexadecimal float: the notation
  applies to integers only, so `0x1.8` is `0x1` followed by something the
  parser has no use for.
  A `0x` with no digit after it is an error naming itself, rather than a `0`
  and an identifier.
- A negative number is a unary minus applied to a literal: `-5`, `-1.5`,
  `-0x80`.
- An integer literal is written in the range of an `int64`, whatever type it
  ends up with and whichever notation writes it. Neither an `int64`'s own
  lowest value nor the top half of a `uint64` can be spelled out; those come
  from arithmetic, from JSON (§6.3), or from the outside world. So
  `0x7FFFFFFFFFFFFFFF` is the largest literal there is, and
  `0xFFFFFFFFFFFFFFFF` is out of range rather than a `uint64` with every bit
  set. This matters because hexadecimal is where a program would reach for
  such a mask.
- The three quote styles produce the same kind of value. A string only needs
  to escape the quote character it is delimited by, so `"it's"`, `'it\'s'` and
  `` `it's` `` are the same string.
- String escape sequences: `\"` (double quote), `\'` (single quote), ``\` ``
  (backtick), `\\` (backslash), `\$` (dollar), `\n` (newline), `\r` (carriage
  return), `\t` (tab), `\0` (a NUL byte), and `\xNN` (the byte with that
  two-digit hexadecimal value; both digits are required, and a shorter form
  is a compile error naming itself). A backslash before any other character
  is kept verbatim. `\xNN` writes one byte, not a code point: a string is
  a byte string (§6.6), so `"\xC3\xA9"` is `é` in UTF-8 and `"\xE9"` alone is
  not valid UTF-8.
- Only a backtick string may span lines, and the newlines written in it
  are part of it:

  ```
  String note = `dear reader,

  regards`;
  ```

  Its indentation is part of it too. There is no margin stripping, so a line
  indented to match the surrounding code carries that indentation. A `\r\n` in
  the source becomes a plain `\n` in the string, so a file saved with either
  line ending yields the same value.
- A backtick string interpolates. `${expr}` inside one is replaced by
  the text `string.from` (§6.6) writes for the value, so anything `print`
  takes may sit in a hole and needs no import:

  ```
  String who = "world";
  int n = 3;
  print(`hello ${who}, n = ${n + 1}`);    // hello world, n = 4
  ```

  It is exactly the concatenation it reads as (`` `n = ${n}` `` means
  `"n = " + string.from(n)`), so it costs one allocation however many holes
  it has (§3.2). Braces inside a hole balance, so a record literal or a nested
  backtick string may sit in one. `\${` writes the two characters, a `$` not
  followed by `{` needs no escape, and an empty `${}` is a compile error. The
  other two forms never interpolate: `"${n}"` is those four characters.
- A `"` or `'` string ends at the end of its line: a newline before the
  closing quote is a compile error, and a backslash before that newline does
  not continue the string onto the next line. Nothing is lost: `\n` writes a
  newline, and a backtick string writes one literally. As a result, a
  forgotten closing quote is reported on the line that has the mistake
  instead of swallowing the rest of the file.

### 1.7 Statement terminators

Every statement ends with `;`, except statements whose last token is `}`
(type and enum declarations, function declarations,
`if`/`while`/`for`/`forEach` blocks, and statements ending in a record or
map literal), where the `;` is optional:

```
int sum;                       // ";" required
Car myCar = {
    make: "toyota",
    age: 4
}                              // no ";" needed after "}"
```

`nio format` writes the optional `;` (§1.1), so a formatted file has one after
every statement.

---

## 2. Types

### 2.1 Built-in types

| Type | Description | Zero value |
|---|---|---|
| `int` | signed integer, as wide as the machine (below) | `0` |
| `int8` | 8-bit signed integer, range −128…127 | `0` |
| `int16` | 16-bit signed integer, range −32768…32767 | `0` |
| `int32` | 32-bit signed integer, range −2147483648…2147483647 | `0` |
| `int64` | 64-bit signed integer | `0` |
| `uint` | unsigned integer, as wide as the machine (below) | `0` |
| `uint8` (alias `byte`) | 8-bit unsigned integer, range 0…255 | `0` |
| `uint16` | 16-bit unsigned integer, range 0…65535 | `0` |
| `uint32` | 32-bit unsigned integer, range 0…4294967295 | `0` |
| `uint64` | 64-bit unsigned integer, range 0…18446744073709551615 | `0` |
| `float` | IEEE 754 floating point, as wide as the machine (below) | `0` |
| `float32` | 32-bit IEEE 754 floating point (binary32) | `0` |
| `float64` | 64-bit IEEE 754 floating point (binary64) | `0` |
| `String` | immutable byte string | `""` |
| `Json` | one JSON value of any shape (§6.3) | missing |
| `RegExp` | a compiled regular expression (§6.14) | none; using one is an error |
| `Socket` | one end of a network connection (§6.15) | none; using one is an error |
| `Listener` | something network connections arrive on (§6.15) | none; using one is an error |
| `bool` | `true` or `false` | `false` |
| `DateTime` | an instant on the timeline, UTC, millisecond precision | Unix epoch |
| `Duration` | a signed length of time, in milliseconds | `0` |

A variable declared without an initializer gets its type's zero value.

A `String` is indexed by byte. `s[i]` is the byte at position `i` (a
`byte`, not a one-character string), zero-based and bounds-checked exactly as
an array index is. It allocates nothing. It is how a program looks at text
one byte at a time, and it agrees with the corresponding element of
`string.toByteArray(s)` (`"é"[0]` is 195, since a `byte` is a `uint8`). Every
other index into a string is already a byte offset (`string.length`,
`string.substring`'s bounds, `string.find`'s result), so `i` counts bytes
rather than characters, and indexing into the middle of a multi-byte
character yields one of its bytes.

There is no `s[i] = b`: a `String` is immutable (§6.6), and identical
literals may be one shared value, so a write through an index would reach
every occurrence. Build the bytes with `string.toByteArray` and
`string.fromByteArray` instead.

`int`, `uint` and `float` are as wide as the machine. They are types of
their own, not other names for `int64`, `uint64` and `float64`: a program
built for a 64-bit machine gets 64-bit ones, and one built for a 32-bit
machine gets 32-bit ones. `getType` (§6.2) tells them apart: `getType(1)`
is `int`, never `int64`. Use `int` and `float` by default, and a sized type
where the width is part of what the program means (a wire format, a file
header, a JSON contract).

The unsigned types hold no negative value. `uint`, `uint8`, `uint16`,
`uint32` and `uint64` are the counts, sizes, bit patterns and bytes a
program reads from outside itself. A `byte` is a `uint8`, so every element
of `string.toByteArray`, `fs.readFile` and a child's output is 0…255 rather
than a number that goes negative halfway up. They are a family of their
own: an unsigned value and a signed one never meet in an operator (below),
because the two do not share an order. In everything else they match the
signed family: the same operators, the same widening, the same wrapping on
overflow, and `%` and `/` computed unsigned.

There is no way to write a literal above an `int64`'s range, on either
family (§1.6), so the top half of a `uint64` is reached by arithmetic, by
JSON, or from the outside world rather than by spelling a number out.

The narrow types are storage and interface types. A type narrower than
`int`/`float` holds values and passes them around, but no operator produces
one:

- Number literals are range-checked when used where a narrower type is
  expected: `int8 b = 127;` is valid, `int8 b = 200;` is a compile error,
  and so are `uint8 b = 256;` and `uint b = -1;`.
  A `float32` literal is rounded to the nearest binary32 value, which is
  not an error (`0.1` is inexact in any width), but one too large for a
  binary32 is.
- All arithmetic widens to `int`, `uint` or `float`: `a + b` on two
  `int16`s is an `int`, and on two `uint16`s a `uint`. So is `-a`.
  `++`/`--` on a narrow value is a compile error (§4.3), since the result
  would not fit back. Putting the result back into a narrow type is what
  the explicit conversion of §3.6 is for: `(a + b) as int16` wraps, and it
  is the only way a program computes a narrow value.
- A value is assignable to any numeric type that can hold every value of
  its own. Within a family that is width: `int8` → `int16` → `int32` →
  `int64`, `uint8` → `uint16` → `uint32` → `uint64`, and
  `float32` → `float64`, with `int`, `uint` and `float` sitting wherever
  the machine puts them. Where two types of a family are the same width
  (`int` and `int64` on a 64-bit machine), each is assignable to the other.
  Across the integer families it is width with one bit to spare, since a
  signed type has to keep room for the sign: a `uint8` goes to every signed
  type but `int8`, a `uint32` to `int64` (and to `int` on a 64-bit
  machine), and a `uint64` to none of them. A signed value never assigns to
  an unsigned type, at any width.

Numeric types do not mix. `1 + 1.5` is a compile error; write
`1.0 + 1.5`. There are no implicit conversions between the integer types
and the float types, and none between the signed and unsigned integers
either: `i + u` on an `int` and a `uint` is a compile error, as is `i < u`,
because on a 64-bit machine neither type holds the other's values. Two cases
do work. The first is a pair where one side holds the other outright (a
`byte` and an `int` meet as an `int`). The second is a plain number written
next to an unsigned value, which takes the unsigned type when it fits:
`u == 5`, `n + 1` and `count > 0` all mean what they look like.

Arithmetic keeps every bit it is given. An operator produces the
natural type of its operands' family, or the wider operand's type where
that is wider still: two `int64`s make an `int64` on any machine, and on a
32-bit machine, where `int` is narrower, an `int64` and an `int` make an
`int64`. Where two different types are equally wide, the result is `int`,
`uint` or `float`. Overflow wraps. On an unsigned type it wraps in both
directions, so `u - 1` where `u` is 0 is the type's largest value.

`-x` is not defined on an unsigned type. An unsigned type has no
negative values, so the only thing the negation could mean is the
wrap-around, which a program that wants it can write as the subtraction it
is. `/` and `%` on two unsigned operands divide unsigned, and comparisons
of two unsigned operands compare unsigned, so a value with its top bit set
is the large number it is rather than a negative one.

### 2.2 Arrays: `T[]` and `T[N]`

```
int[] myArray = [2, 5, 4];     // growable; starts with the literal's elements
int[] empty = [];              // empty array (element type from the declaration)
int[5] fixed = [1, 2, 3, 4, 5];   // fixed size: always exactly 5 elements
```

Arrays come in two flavors:

- `T[]` is a **growable** array. Its length changes at run time through the
  `array` standard library module (§6.5): `array.push` appends an element,
  `array.pop` removes the last one. Both mutate the array in place, through
  every reference to it.
- `T[N]` is a **fixed-size** array of exactly `N` elements (`N` is a
  positive integer literal). `N` is the actual length, not a
  maximum: declared without an initializer, a `T[N]` holds `N` zero values;
  a literal for it must have exactly `N` elements; and its length never
  changes. `array.push`/`array.pop` on it are compile errors, as is a
  constant index that can never be in range (`fixed[7]`).

Both flavors behave the same everywhere else:

- `a[i]` reads, `a[i] = v` writes. Indices are `int`, zero-based, and
  bounds-checked at runtime. Elements are mutable.
- `a.length` is the length as an `int` (read-only).
- An empty array literal `[]` is only valid where the element type is known
  from context.

`T[N]` and `T[]` are distinct types, and `T[5]` is distinct from `T[6]`;
none is assignable to any other (a growable alias of a fixed-size array
could change its length). `array.copy` and `array.slice` (§6.5) read either
flavor into a new growable array, which is also the conversion between the two.

### 2.3 Optionals: `T?`

An optional holds either a value of `T` or `null`.

```
String? owner;                 // starts as null
owner = "alice";
owner = null;
bool known = owner != null;
```

- `T` and `null` are both assignable to `T?`.
- `T??` (optional of optional) is not allowed.
- Optionals support `==` and `!=` against `null`, against a plain `T`, or
  against another `T?`. No other operators apply to optionals directly; the
  value comes out through narrowing or optional chaining, below.

**Narrowing.** Where the compiler can see that an optional is not null, the
value is used as a plain `T`, with no unwrapping syntax. Narrowing follows the
flow of the program:

```
type Node { int value; Node? next; }
Node second = { value: 2, next: { value: 1, next: null } };

if (second.next != null) {
    print(second.next.value);    // second.next is a Node here
}

Node? cur = second;
while (cur != null) {
    print(cur.value);              // cur is a Node inside the body
    cur = cur.next;                // assigning a Node? un-narrows it
}
```

What narrows is a *path*: a variable, or a variable followed by field names
(`x`, `x.next.owner`, `m.x` for a module variable). Anything reached
through an index expression or a call is not a path. A path is narrowed:

- in the true branch of `if (p != null)` and the false branch of
  `if (p == null)`, and after the `if` entirely when the branch taken
  otherwise always exits, by `return` or (inside a loop) by `break` or
  `continue`;
- in the body of `while (p != null)`, and after `while (p == null)` unless
  the body can `break` (a `break` leaves with the condition still true, and
  so proves nothing; §4.8);
- to the right of `&&` / `||` where the left side already decided
  (`p != null && p.value > 0`), and through `!`, in any boolean expression;
- in the true branch of `p == v` where `v` can never be null (its type is
  neither optional nor `null`);
- after `p = v` (or `T? p = v;`) where `v` can never be null.

Narrowing ends where the certainty does. Assigning `null` or another
optional to the path, or to any prefix of it, cancels it (assigning `x`
un-narrows `x.f`). A loop body that reassigns the path on some iteration
keeps it un-narrowed throughout the loop. After an `if`, only facts
established on every path that falls through survive. A function body never
inherits narrowing from the surrounding top level, since it runs at call
time. Comparing with `null` always sees the raw optional, so re-checking a
narrowed value is not an error.

One caveat: narrowing trusts the check, but a record field is shared.
Another reference (or a call) can set a narrowed field back to null between
the check and the use. Reading a narrowed path that has become null in this
way is a runtime error (§5.5), never memory corruption.

**Optional chaining (`?.`).** `a?.b` is `null` when `a` is null, and `b`'s
value when present. The result is optional: `T?` for a field of type `T`,
and plain `T?` for a field already optional, so a chain never produces
`T??`. Once a chain starts, the accesses after it (plain members and
indexing alike) continue it, and the whole chain short-circuits to `null`
at the first absent link:

```
print(second.next?.value);         // 1
print(second.next?.next?.value);   // null (chain stops at the second link)
c.boss?.members[0]                 // String? — indexing continues the chain
c.boss?.members.length             // int? — so does .length
```

`?.` requires an optional on its left (`x?.f` on a non-optional `x` is a
compile error, as is `?.` on a module alias). A chain cannot be assigned
through (`a?.b = v` is a compile error), and a function cannot be called
through it.

### 2.4 Record types

Records are user-declared, nominal types, declared at the top level only:

```
type Car {
    String make;
    int age;
    String? owner              // optional field; the last ";" may be omitted
}
```

A field is written type first, then name. This is the same order as a
variable declaration (§4.1) and a parameter (§5.2). The retired `name: type`
form is rejected with a migration error naming the type-first spelling.

- The type's name must start with an upper-case letter (§1.3).
- Field order is significant: it is the order used by `json.toText`.
- Two record types with identical fields are still distinct types.
- Records may reference other records, in any declaration order.
- Record values are references: assigning or passing a record does not copy
  it.
- The `;` between fields is required; after the last field it is optional.

**Renaming a field for a serialization format.** A field's key in JSON is
its declared name unless a string literal after the name gives it another:

```
type Engine {
    float liters;
    int power 'json:engine_power';
}
```

`json.toText` (§6.3) then writes `"engine_power"` where the field is named
`power` in Nio; member access is unaffected (`e.power`). The annotation's
string is `target:key`, split at the first `:`, so a key may itself contain
one. `json` is the only defined target; the prefix reserves room for a
second format without a syntax change.

No keyword introduces the annotation: a string literal is the only thing
that may follow a field name, so the position alone identifies it.

These are compile errors: an annotation with no `:`, an unknown target, an
empty key, a key equal to the field's own name, and two fields of one record
that resolve to the same key.

**Nested and anonymous types.** A field's type can be another record type,
either by name or declared inline without a name. An anonymous record type
may be followed by `[]` and `?` suffixes like any other type, and its fields
may themselves be anonymous record types:

```
type CarWeight {
    int empty;
    int? full
}

type Car {
    String make;
    CarWeight weight;         // a named type used inside another type
    {                 // an anonymous type: exists only as this field
        DateTime date;
        int amount;
        String? piece
    }[]? repairs;
}
```

An anonymous type is only reachable through its field: values are built with
record literals in positions typed by the field (`myCar.repairs =
[{ date: d, amount: 250 }];`) and read back through member access. In
diagnostics it is named by its path, e.g. `Car.repairs`. Like all record
types it is nominal: another anonymous type with identical fields is a
different type. Anonymous record types are only allowed as field types
inside a `type` declaration, not in variable, parameter, or return types.

**Methods.** A type's body may also declare functions, which are called on a
value of the type and reach it through `self`:

```
type Car {
    String make;
    int buildYear;

    int age(int now) {
        return now - self.buildYear;      // self is the receiver
    }

    void rename(String name) {
        self.make = name;                 // records are references (above),
    }                                     // so this changes the caller's car
}

Car myCar = { make: "toyota", buildYear: 1922 };
print(myCar.age(2026));                 // 104
myCar.rename("honda");
```

A method is written exactly like a function declaration (§5.2): return type
first (omitted when it returns nothing), `async` before the name for an
async one, and a variadic last parameter allowed. No terminator is needed
after its body.

- `self` names the receiver, and is bound only inside a method body. Fields
  are reached through it: a body has no implicit field scope, so `make`
  alone is undefined where `self.make` is the field. Outside a method,
  `self` is an ordinary identifier.
- The receiver is the value the method was called on, not a copy of it, so
  a method assigning to `self.field` changes what the caller sees. There is
  no second kind of receiver to choose from.
- Methods are per type, not per value. A value of the type stores nothing
  for them, and which body runs is decided at compile time from the
  receiver's type. They take no part in a record's layout, its JSON form
  (§6.3), or its equality.
- A method's name may not be a field's name of the same record, and two
  methods of one record may not share a name. Method names live in the
  record's own namespace: they may repeat function names, other records'
  method names, and the built-in names of §6.
- Methods may call each other and themselves, in any declaration order, and
  whether one can fail is inferred from its body like any other function
  (§2.9).
- A method is not a value. Like a declared function (§3.4), its name can
  only be called: `f = car.age;` and `car.age = ...` are compile errors.
  Wrap it to get a value: `Function(int)<int> f = int (int n) -> car.age(n);`.
- An `export`ed type carries its methods to importers, who call them on
  values of the type with no extra syntax (§5.4).
- Anonymous record types (below) take fields only.

A function-typed field (§2.6) is a different thing. It is the right choice
when the behavior varies per value rather than per type: it holds a
function value, which captures variables where it was written and knows
nothing about the record holding it.

**Extending a type.** An `extends` clause gives a type a copy of another
type's members before its own:

```
type Car {
    String make;
    String? owner;

    int wheels() { return 4; }

    void describe() {
        print(self.make, "on", self.wheels(), "wheels");
    }
}

type Truck extends Car {
    int load;                                  // added on top

    int override wheels() { return 6; }        // replaces Car's

    int carry(int extra) { return self.load + extra; }
}

Truck t = { make: "volvo", load: 900 };
t.describe();                                  // volvo on 6 wheels
print(t.make, t.load, t.carry(100));           // volvo 900 1000
```

The extending type gets the base's fields first, in the base's order and at
the base's indices, then its own. It gets one method for every method the
base has, plus its own. Optional fields, renamed fields (above), anonymous
field types and inferred fallibility all come along unchanged, because the
copy is of the whole declaration.

- A copied method is a method of the extending type: its body is checked
  and compiled again with `self` typed as that type. So a base method that
  calls `self.wheels()` calls the extending type's `wheels`. `describe`
  above says 6 for a `Truck` and 4 for a `Car`.
- `override` is required to replace an inherited method, and is an error
  where there is nothing to replace. The replacement must keep the
  signature of what it replaces. Only fallibility may differ, since that is
  inferred from the body rather than written (§2.9).
- Fields are not overridable: redeclaring an inherited field name is an
  error, as is a field and a method sharing a name, however each arrived.
- The clause names one type, which must be a `type` declaration (not a
  built-in type, not an enum), and chains: `Van extends Truck extends Car`
  copies from the whole chain. A cycle is an error.
- An extending type is a type of its own. `getType` reports its own name,
  and its equality and JSON form are its own. Extending copies members. On
  its own it does not make one type usable as another, and an extending
  type is not assignable to the type it extends (§2.10). The `sealed`
  modifier below changes that, and only for the type it is written on.

**Extending a type from another file.** The base may be imported, written
`extends alias.Type`, and must be `export`ed like any other imported type
(§5.4). A copied body still resolves its names where it was written. It
keeps calling that module's functions (including the ones it does not
export), reading its variables, and using its import aliases. The extending
file does not have to have any of these, or even be able to name them:

```
// shapes.nio
import 'json';
String label(String s) { return "<" + s + ">"; }    // not exported

export type Shape {
    String name;
    String show() { return label(self.name); }      // reaches label, and json
}
```

```
// main.nio
import './shapes.nio';

type Square extends shapes.Shape {                  // no import 'json' needed
    int side;
}

Square s = { name: "sq", side: 4 };
print(s.show());                                  // <sq>
```

Because `self` is the extending type, a base method that uses its own type
where the receiver goes cannot be inherited. Examples are
`Car me() { return self; }`, or passing `self` to something typed `Car`:
`self` is a `Truck` there, and a `Truck` is not a `Car`. The compiler
reports it against the type that inherited the body, naming the file and
line the body was written in.

**Sealed types.** A type declared `sealed` may be extended only inside the
module that declares it. In exchange, the compiler knows the whole set of
types extending it, and two things follow that a plain `extends` cannot
have: a value of an extending type may be used where the sealed type is
expected (§2.10), and a `switch` over the sealed type can be checked to
cover every case (§4.9).

```
sealed type Expr { int line; }

type IntLit extends Expr { int value; }
type Ident  extends Expr { String name; }
type Binary extends Expr { String op; Expr left; Expr right; }

IntLit two = { line: 1, value: 2 };
Expr   e   = two;                    // an IntLit is usable as an Expr
Expr[] all = [two, { line: 1, name: "x" }];
```

The set has to be closed to be known, and a module cannot see who imports
it. So an exported sealed type may be used and switched on by importers,
but not extended by them. That is the only cost of sealing, and it means
that only the type's own module can add a case to it.

- `sealed` goes immediately before `type`, after `export` when both are
  written: `export sealed type Expr { … }`.
- A sealed type cannot be constructed. It names the types extending it
  rather than being one of them, so `Expr e = { line: 1 };` is an error. If
  it could be made, the value would be in nobody's set of extenders and
  would match no `case` of a switch over them. Then the rule that some
  clause always runs, which lets an exhaustive type switch count as
  terminating (§4.9), would be false. A value carrying only the base's own
  fields is written as a type extending it that adds none. That costs one
  line and gives the value a name, so every switch must handle it and it
  cannot slip through all of them.

  Not being constructible is not the same as having no fields. A sealed
  type's fields are declared, copied into every extending type at the same
  indices, and readable through a value typed as the base (below). That is
  what a shared `line` or `tok` is for. A field, parameter, or array element
  typed as the sealed type is likewise unaffected, and is how a recursive
  tree is written:

```
sealed type TypeNode { token.Token tok; }
type ArrayTypeNode extends TypeNode { TypeNode elem; }   // recursion is fine
```

- A sealed type's zero value is null, like a recursive record's and for
  the same reason: neither can be constructed. A variable declared without
  an initializer therefore holds nothing. Reaching an exhaustive type
  switch with one is a runtime error naming the type (§5.5); no clause
  silently fails to run.
- A type extending a sealed one may not itself be extended. The hierarchy
  is one level deep: a sealed type and the types extending it. A deeper one
  would put values in the hierarchy that no `case` over the extenders
  names, and coverage would stop meaning what it says.
- A method cannot be called on a value whose type is the sealed one.
  Fields can be read and written; methods cannot. A method is chosen by the
  type written down (§2.4), and a sealed-typed value may be of any type
  extending it, so the call would run the sealed type's own body on a value
  of some other type. Narrow first, with `switch` (§4.9) or `as` (§3.6), to
  make the receiver's type certain:

```
sealed type Shape {
    int sides;
    String describe() { return "a shape"; }
}
type Square extends Shape {
    String override describe() { return "a square"; }
}

Square sq = { sides: 4 };
print(sq.describe());              // a square — the type is certain here
Shape s = sq;
// print(s.describe());            // error: match its type first
switch (s) {
    case Square q:
        print(q.describe());       // a square
}
```

- A sealed type's JSON form is the value's own. `json.toText` on a value
  reached through the sealed type writes the fields of the type it actually
  is, not the ones the sealed type declared. Serializing an `Expr` holding
  an `IntLit` writes the `IntLit`.
- `getType` is unchanged, and still answers the static type (§6.2): it
  names what was declared, and a sealed-typed value reports the sealed
  type's name. Narrowing is how a program learns what a value really is.

**Record literals** must appear where the record type is known from context
(a declaration, assignment, argument, return value, or field of another
literal):

```
Car myCar = {
    make: "toyota",
    age: 4                     // optional fields may be omitted (they are null)
}
```

Every non-optional field must be present; unknown or duplicated field names
are compile errors. A trailing comma is allowed.

### 2.5 Enum types

An enum is a user-declared, nominal type whose values are a fixed set of
named integer constants, declared at the top level only:

```
enum HttpResponses {
    OK: 200,
    NOT_FOUND: 404
}

HttpResponses r = HttpResponses.OK;
```

The enum's name must start with an upper-case letter (§1.3). Every member
carries an explicit integer value (a leading `-` negates the literal).
Members are separated by commas, with a trailing comma allowed. Member names
must be distinct within the enum and an enum needs at least one member;
values may repeat (two names for one value compare equal).

- A member is named through the type: `HttpResponses.OK` is a compile-time
  constant of type `HttpResponses`. The enum's name is not a value by
  itself, and a member cannot be assigned to. Because members are reached
  through the enum's name, that name may not be reused by a function or
  variable in the same module.
- Enums are nominal, like records: two enums with identical members are
  distinct types, and an `int` is not assignable to an enum. In the other
  direction an enum value widens: it is assignable to `int` (§2.10), which
  is how the numeric value comes out.
- Enum values support equality and ordering (§3.1, §3.2) against values of
  the same enum and against ints. Values of two different enum types never
  compare. Arithmetic is not defined on enums; widen to `int` first.
- Printing shows the member's name (`print(HttpResponses.OK)` prints `OK`,
  §6.1); `json.toText` serializes the numeric value (§6.3). Enums work in
  optionals, arrays, and record fields like any other scalar.
- The zero value of an enum type is the numeric value `0`: the member with
  value 0 when the enum declares one, and otherwise a value outside the
  members, which prints as its number.

An `export`ed enum is visible to importers as `alias.Name` in type
positions and `alias.Name.MEMBER` in expressions (§5.4).

### 2.6 Function types: `Function(…)<…>`

A function type names the values a function value (§3.4) can take: the
parameter types go in parentheses, the return type in angle brackets, with
`void` marking a function that returns nothing. This is the same keyword a
declaration or a function value writes for one (§5.2, §3.4):

```
Function(int)<int> f;              // takes an int, returns an int
Function()<void> task;             // takes nothing, returns nothing
Function(int, String)<bool> check;
Function(int)<int>[] pipeline;     // array of function values
Function()<int>? maybe;            // optional function value
Function(int)<Function(int)<int>> makeAdder;
Function(int, ...String)<void> log;  // variadic (§5.2)
```

- Function types are structural: two are the same type exactly when their
  parameter lists and return types are the same. Assignability between
  function types is equality, with no widening of parameters or results.
- A `...` on the last parameter type names a **variadic** signature (§5.2),
  and is part of the type's identity: `Function(...String)<void>` and
  `Function(String[])<void>` are different types, even though the body of
  either one binds a `String[]`. Only a call of the first collects its
  arguments.
- A function type can appear anywhere a type can: variables, parameters,
  return types, record fields, array elements, and optionals.
- The zero value of a function type is a **null function**: the only thing
  it does is panic when called (§5.5). Give function variables a value
  before calling them, or declare them `Function(…)<…>?` and narrow.
- Function values cannot be compared (§3.2), printed (§6.1), or
  serialized (§6.3).

### 2.7 Futures: `Future<T>`

A future is the not-yet-available result of calling an `async` function
(§5.2): `Future<int>` from an async function declared `int`, `Future<void>`
from a void one. Futures are ordinary values. A program can store them,
pass them, and put them in fields, arrays, and optionals. They are consumed
with `await` (§3.5) or `async.run` (§6.8):

```
int async sum(int a) { return a; }

Future<int> f = sum(5);        // the body has not run yet
int x = await f;               // runs it now; x is 5
Future<int>[] batch = [sum(1), sum(2)];
Future<int>? maybe = null;
```

- `Future<T>` and `Future<S>` are the same type exactly when `T` and `S`
  are; there is no other assignability between futures.
- The zero value of a future type is a **null future**: awaiting it (or
  handing it to `async.run`) panics (§5.5). Futures a program makes itself
  always come from async calls and are never null.
- Futures cannot be compared (§3.2), printed (§6.1), or serialized (§6.3).
  Await the result and use that instead.

### 2.8 Maps: `Map<K, V>`

A map is a mutable collection of key → value entries with fast lookup by
key (a hash map), iterated in insertion order:

```
Map<String, int> ages;              // starts empty, like a growable array
ages["alice"] = 31;                 // insert
ages["alice"] = 32;                 // overwrite
int? age = ages["alice"];           // lookup yields V?: null when absent
print(ages["carol"]);               // null
print(ages.length);                 // 1
```

- **Keys** are `String` or one of the scalar types whose equality is a
  value comparison: the integer types, `bool`, `DateTime`, `Duration`, and
  enums. Floats cannot key a map, because NaN never equals itself, so a
  float key could be stored and never found. Neither can records, arrays,
  or the other composite types, which compare by reference. String keys
  hash and compare by content: a key built at run time finds an entry
  stored under a literal.
- **Values** may be any type except an optional: `m[k]` already yields
  `V?`, and `V??` does not exist (§2.3). `Map<String, int?>` is a compile
  error; store the inner type.
- `Map<K, V>` and `Map<K2, V2>` are the same type exactly when the key
  and value types are; like futures, there is no other assignability
  between map types.
- Declared without an initializer, a map starts empty. This matches a
  growable array's empty start, not an optional's null.
- Like arrays and records, a map value is a reference: assignment and
  parameter passing share the one map. `map.copy` (§6.7) makes a shallow
  copy.

**Reading and writing.** `m[k]` yields `V?`: an absent key is normal data,
not a bounds error, so lookup composes with narrowing (§2.3) and optional
chaining instead of trapping like an array index. `m[k] = v` stores a
plain `V`, inserting the key or overwriting its value; `null` cannot be
written. A lookup is not a path (§2.3) and never narrows; bind it to a
variable and test that. For the same reason `m[k]++` is a compile error,
and the read-modify-write is spelled out:

```
int? hits = counts["alice"];
if (hits != null) {
    counts["alice"] = hits + 1;
}
```

`m.length` is the number of entries, as an `int` (read-only). Maps cannot
be compared with `==` (§3.2) or printed (§6.1); serialize a String-keyed
map with `json.toText` (§6.3).

**Map literals.** A map literal lists entries in braces, like a record
literal, and the first key tells the two apart. A record field is named by
a bare identifier. A map key is a literal constant: a string literal or a
(possibly negated) number literal. `{}` is the empty map wherever a map
type is expected (the context supplies the type, exactly as it does for
`[]`), and a literal's keys must be distinct.

```
Map<String, int> ages = { "alice": 31, "bob": 27 };
Map<int, String> codes = { 200: "ok", 404: "not found", -1: "minus" };
Map<String, float> scores = {};
Map<String, Car> fleet = {
    "garage-1": { make: "toyota", age: 4 },  // quoted key: map entry
};                                           // bare name: record field
```

Only a literal can key an entry of a literal. Anything computed (a
variable, a bool, an enum member) goes through indexed assignment:
`m[key] = v;`.

**Iteration** goes through the keys, with the ordinary array machinery
(§4.7, §6.7):

```
forEach(map.keys(ages), name) {
    print(name);
}
```

Iteration order is insertion order: the order entries were first inserted.
An overwrite keeps the entry's place, and a removed-then-reinserted key
moves to the end. `map.keys`, `map.values`, and JSON serialization all
follow this order, so output is deterministic.

### 2.9 Errors

A function that can fail returns an **`Error`** instead of its result:

```
String lookupName(int id) {
    if (id < 0) { return Error("negative id"); }
    return "Jean";
}
```

`Error` is a built-in record with two fields, `message String` and
`code int`. It is an ordinary value otherwise: it can be held in a variable,
passed, stored in a field, and returned from a function whose declared type
is `Error`.

A handler dispatches on the code. A message is prose: it varies with the
platform's `strerror` and is written to be read, not matched. So telling
one failure from another goes through the code:

```
import 'fs';

byte[]? data = fs.readFile(p) catch e {
    if (e.code == fs.ErrorCode.NOT_FOUND) { print("using defaults"); }
    else { return e; }                    // anything else is real: rethrow
};
```

- `Error("...")` leaves the code `0`, the "unspecified" code. The second
  argument is optional and most errors have nothing to add beyond a message.
- The code is a plain `int` rather than any particular enum, because no one
  enum could serve both the library and user code (enums are nominal,
  §2.5). A program declares its own and passes a member, which widens by
  rule 5 of §2.10 and compares back by the enum/int equality of §3.2:

  ```
  enum ParseErr { BAD_DIGIT: 1, OVERFLOW: 2 }
  int parse(String s) { return Error("bad digit", ParseErr.BAD_DIGIT); }
  int n = parse(s) catch e {
      if (e.code == ParseErr.BAD_DIGIT) { return 0; }
      return e;
  };
  ```
- The standard library's codes are the members of **`ErrorCode`**, named
  through the module that raises: `fs.ErrorCode`, `string.ErrorCode`,
  `path.ErrorCode`, `process.ErrorCode`, `regexp.ErrorCode`, `net.ErrorCode`,
  `crypto.ErrorCode`. All seven name the same enum, so a `NOT_FOUND` from
  `fs` and one from `process` mean the same thing and compare. Its members
  are `NONE` (0), `OTHER`, `NOT_FOUND`, `PERMISSION`, `EXISTS`,
  `NOT_DIRECTORY`, `IS_DIRECTORY`, `NOT_EMPTY`, `INVALID`, `IO`, `NO_SPACE`,
  `TOO_MANY_FILES`, `NAME_TOO_LONG`, `INTERRUPTED`, `END_OF_FILE`, `LOOP`,
  `READ_ONLY`, and, for the network (§6.15), `CONNECTION_REFUSED`,
  `CONNECTION_RESET`, `CONNECTION_ABORTED`, `NOT_CONNECTED`,
  `ALREADY_CONNECTED`, `ADDRESS_IN_USE`, `ADDRESS_NOT_AVAILABLE`,
  `NETWORK_UNREACHABLE`, `HOST_UNREACHABLE`, `BROKEN_PIPE`,
  `MESSAGE_TOO_LONG`, and `TIMED_OUT`. They are symbolic rather than raw
  `errno` numbers, which differ across platforms the way path separators
  do. An `errno` the mapping does not name becomes `OTHER`, so a program
  never sees a code this list does not have.

**Which functions can fail is inferred, never declared.** A function is
**fallible** when its body can `return Error(...)`, or when it calls a
fallible function without catching it. Nothing in a declaration says so.
The compiler works it out from the body, across the whole call graph,
including functions declared later in the file and mutually recursive ones.

A standard library function counts as a fallible call the same way. The
library's fallible functions are the ones whose failures come from outside
the program: every function of `fs` (§6.11) but `fs.exists`, the three
parsers `string.toInt`, `string.toUint` and `string.toFloat` (§6.6), and the
reads of `process.stdin` (§6.12). A body that reads a file, or parses a
number, without catching it is fallible for that reason alone.
`process.child.run` (§6.12) fails the same way, one step later: its future
is fallible, so the `await` can fail rather than the call.

**An error propagates on its own.** A call that can fail, written without a
`catch` (§3.7), returns from the enclosing function with that error the
moment it happens. This makes the enclosing function fallible in turn.
Nothing is written at the intermediate levels:

```
String greeting(int id) {
    return "hello " + lookupName(id);   // greeting is fallible too
}
String page(int id) {
    return greeting(id) + "\n";         // and so is page
}
```

**An error nobody catches stops the program**, with its message on stderr
and exit code 1, exactly like the runtime errors of §5.5:

```
print(page(-1));    // runtime error: negative id
```

So recovery is opt-in at any depth. Adding a `catch` somewhere up the chain
is the only change needed, and a program that never catches behaves like a
program with no error handling.

**Raising versus returning.** `return Error(...)` is a raise wherever the
declared return type could not hold an Error, including from a `void`
function, which can fail like any other. Where the declared type can hold
one (`Error f()`, `Error? f()`), the same statement is an ordinary return
of a value.

**Errors are not panics.** The conditions in §5.5 (an index out of range,
division by zero, a deadlock) are bugs, not results: they still abort and
cannot be caught. `Error` is for failures a caller can reasonably act on.

**Contracts with no body: the `!` marker.** Inference reads bodies, so the
two types that describe a function without one say it in the type, by
suffixing the result with `!`:

```
Function(int)<String!> gen;    // a function value that may fail
Future<String!> pending;       // the future of a fallible async function
```

- A function type's `!` is part of its identity: `Function(int)<String>`
  and `Function(int)<String!>` are different types, and neither is
  assignable to the other (§2.6 admits no widening between function types).
- A function value written where a fallible function type is expected
  takes that contract on, whether or not its own body can fail, because its
  callers are already compiled against it. A value whose body can fail,
  written where a non-fallible type is expected, is a compile error.
- An async function is inferred like any other. Calling it never raises,
  because the body has not run. The fallibility rides on the future, and
  surfaces at the `await` (§3.5).
- `async.run` (§6.8) does not accept a fallible future: a completion
  callback has no error channel. Await it instead.
- An error raised by a future nobody ever awaits stops the program when it
  runs out of work, rather than being discarded.

### 2.10 Assignability summary

A value of type `S` is assignable to a location of type `T` when:

1. `S` and `T` are the same type, or
2. `T` is `S?` (including `null` to any `T?`), or
3. `S` and `T` are both integer types, or both float types, and `T` is at
   least as wide as `S` (§2.1), or
4. `S` is a number literal that `T` can hold and `T` is a number type, or
5. `S` is an enum type and `T` is an integer type at least as wide as
   `int`, or
6. `T` is a `sealed` record and `S` is one of the types extending it (§2.4),
   or
7. `T` is a `union` and `S` is accepted by exactly one of its members
   (§2.11), in which case the value is wrapped into that member.

Nothing else assigns. In particular a type that `extends` a type which is
not sealed is not assignable to it: extending copies members, and without
sealing the two remain separate types. Rule 6 is what `sealed` adds. It is
confined to a relation the compiler can see in full: the direct extenders
of a sealed type, all declared in its own module.

Rule 7 is the only entry that converts rather than merely permits, and the
only one that composes: `Handler? h = someHandler;` applies rule 7 and then
rule 2. It composes with nothing else and never applies twice, which is why
a union may not hold a union. "Exactly one" is decided at each use rather
than at the declaration. So two members that accept the same value form a
legal union, whose values are named rather than inferred (§2.11).

One conversion happens outside this list, at one syntactic place. Storing
into a `Json` member (`doc[k] = v`, and `json.push`) turns `v` into a JSON
value, so every type with a JSON form (§6.3) may be written there, not only
a `Json`. It is a conversion into a document, not an assignment between two
Nio types, which is why it is stated there and not here.

### 2.11 Union types

A `union` is a value that is exactly one of a fixed set of types, each named
by a **tag**:

```
union Text {
    String,
    byte[] Bytes
}
```

A member is a type followed by a tag, in the same type-then-name order as a
field (§2.4), a variable (§4.1) and a parameter (§5.2). Members are separated
by commas, with a trailing comma allowed, and a union needs at least one.

A member whose type is a single capitalized name takes its tag from the
type, so `String` above is the `Text.String` member and needs no tag of its
own. Everything else (`byte[]`, `int`, a function type, a map) has no name
to borrow and must be tagged:

```
union Opcode  { ADC, AND }                     // tags ADC and AND
union Handler {
    Function(Request)<Response!>        Sync,
    Function(Request)<Future<Response>> Async
}
```

A union is a record type in every other respect. It is nominal (two unions
with identical members are different types), it may be `export`ed, it may
sit in fields, arrays, maps and optionals, and a value of it costs one heap
block of two slots.

A value goes in by §2.10 rule 7, which wraps it into the one member that
accepts it:

```
Text a = "hi";                    // the String member
Text b = string.toByteArray("x"); // the Bytes member
```

Or it goes in by naming the member, `Union.Tag(value)`. This is required
whenever more than one member accepts the value: two members may share a
type, and then the tag tells them apart:

```
union Distance { float Meters, float Feet }

Distance d = 3.0;                  // error: Meters and Feet both accept a float
Distance d = Distance.Meters(3.0); // fine
```

A value comes out by matching its type (§4.9), and the clause binds what
the member holds:

```
String render(Text t) {
    switch (t) {
        case Text.String v: return "chars(" + v + ")";
        case Text.Bytes  v: return "bytes(" + string.fromByteArray(v) + ")";
    }
}
```

With no `default` the clauses must cover every member, so adding one is a
compile error at every switch that does not handle it. `as` (§3.6) is the
one-off form, and answers what the member holds:

```
String? s = t as Text.String;
if (s != null) { print(s); }
```

- Tags must be distinct within the union and start with an upper-case
  letter, like enum members (§2.5). Two members may share a type; the tags
  distinguish them.
- A member may not be optional. `Text?` is how a union expresses absence,
  and it means one thing; an optional member would give a `Text?` two ways
  to be nothing.
- A member may not be a union, may not be `void`, and a union may neither
  `extends` a type nor be extended.
- A union has no printed form (§6.1), no equality (§3.2) and is not a map
  key (§2.8); match it first. `getType` answers the union's own name, since
  it reports the static type (§6.2).
- A union meets JSON by kind (§6.3). It serializes as the member's payload,
  bare, and a parse picks the member by the value's JSON kind. So a document
  field that is sometimes a string and sometimes a number is a union, and a
  parse target may not have two members read from the same kind.
- A member is reached through the union's name, so an imported union's
  member takes all three segments: `alias.Union.Tag`. It is the only name in
  the language that does.

A union is `sealed` underneath (§2.4): the compiler declares the union as a
sealed type and one type extending it per member, each holding that
member's value. Everything above follows from that and is not a separate
mechanism: coverage checking, the single pointer comparison a match costs,
precise tracing of what a member holds, and the rule that only the
declaring module may add a member. The generated names cannot be spelled in
source, and every diagnostic names the union and the tag that were written.

### 2.12 `noalloc`

Memory is reclaimed by a collector that runs at an allocation (§5.5), so a
function that allocates nothing can never start one. `noalloc` is how a
declaration says so, and the compiler checks it:

```
int noalloc ctSelect(int cond, int a, int b) {
    int mask = -cond;                    // 0 or -1
    return (a & mask) | (b & ~mask);     // no branch, no allocation
}

void noalloc xorInto(byte[] buf, byte[] key) {
    for (int i = 0; i < buf.length; i++) {
        buf[i] = (buf[i] ^ key[i % key.length]) as byte;
    }
}
```

The modifier goes between the return type and the name, where `async` goes,
and the two may be written in either order. Like `async`, `override` and
`extends`, it is contextual: `noalloc` is not a keyword and stays usable
as an ordinary name.

**What a `noalloc` body may contain.** The rule is a whitelist. Anything not
known to be allocation-free counts as an allocation:

- literals, including a string literal, which is a constant emitted with the
  program rather than a string built now;
- reading a variable, a field, `.length`, or an element of an array or string
  the caller already owns, and writing into one;
- arithmetic, the bitwise operators, comparison, and `as` between numbers;
- `if`, `while`, `for`, `forEach`, `switch`, `break`, `continue`, `return`;
- calls to other functions and methods declared `noalloc`;
- `getType`, and `print`/`printInline` of anything but a `DateTime`;
- the built-ins that allocate nothing: `string.length`, `string.contains`,
  `string.find`, `string.equalsConstantTime`,
  `string.bytesEqualConstantTime`, and `time.milliseconds`.

Everything else is refused, and the diagnostic names what allocates: a
record, array or map literal; joining strings with `+`; a function value,
which allocates the block holding what it captured; an `await`; an `Error`.

**No optionals.** A `T?` holds its value in a box, and the box is an
allocation. A `noalloc` body may not use an optional type at all: not as a
parameter, not as the return type, not as a local. This one rule replaces
separate rules for each place a box can be created.

**It is declared, not inferred.** A `noalloc` function may only call
functions that also say `noalloc`, even where the callee happens to allocate
nothing. Inference would let the property hold silently and then break when
an unrelated body changed. As a contract it is visible at the call site and
at the declaration, and the error names the callee to fix.

**A function value never carries it.** `noalloc` is not part of a function
type (§2.6), so a `noalloc` function still assigns to an ordinary function
type. Calling through a value is therefore an allocation, however the value
was obtained. Making it part of the type would mean two function types per
signature and a conversion between them.

`async` and `noalloc` cannot both hold: an async function allocates the frame
its locals live in before its body runs (§3.5).

**What it does not promise.** `noalloc` is about this program's heap. It
says nothing about how long the body takes. A body that allocates nothing
can still take data-dependent time, through a branch, a table lookup whose
index is secret, or a hardware divide. It is one of the things
constant-time code needs, not all of them.

---

## 3. Expressions

### 3.1 Operators and precedence

From lowest to highest precedence; all binary operators are
left-associative:

| Level | Operators | Operand types → result |
|---|---|---|
| 1 | `x catch …` | handles an error raised anywhere in `x` (§3.7) |
| 2 | `\|\|` | `bool × bool → bool` (short-circuit) |
| 3 | `&&` | `bool × bool → bool` (short-circuit) |
| 4 | `==` `!=` | see §3.2 → `bool` |
| 5 | `<` `>` `<=` `>=` | ints, floats, strings, `DateTime`s, `Duration`s, enums (§2.5) → `bool` |
| 6 | `+` `-` `\|` `^` | ints → `int`; floats → `float` (§ 2.1); `+` also `String × String → String`; `\|` `^` int-only |
| 7 | `*` `/` `%` `&` `<<` `>>` | ints → `int`; `*` `/` floats → `float` (§ 2.1) (`%` `&` `<<` `>>` are int-only) |
| 8 | unary `-`, `!`, `~`, `await` | `-` on numerics; `!` on `bool`; `~` on ints; `await` on futures (§3.5) |
| 9 | `x as T` | reads JSON, narrows a sealed type, or converts a number (§3.6) |
| 10 | `f(args)`, `a[i]`, `x.member`, `x?.member` | calls, indexing, member access, optional chaining (§2.3) |

Parentheses group as usual. Operands are evaluated left to right. `&&` and
`\|\|` do not evaluate their right operand when the left operand decides the
result.

**The bitwise operators** (`&`, `|`, `^`, `~`, `<<`, `>>`) read a value as
bits rather than as a number, and are defined on the integer types only. A
float has bits, but they are a sign, an exponent and a mantissa, so masking
them is never what a program means. `&` on a float is a compile error that
says so.

`&`, `|` and `^` take a pair and widen it exactly as `+` does (§2.1), so they
are defined on precisely the pairs `+` is. This includes the rule that the
two integer families do not mix.

A **shift** is not a pair. Its right operand is a count, the same kind of
operand as the number scaling a `Duration` (§6.4), so it carries no family of
its own and may be any integer type. The result is the left operand's own
widened type. That type decides how `>>` fills: with the sign for a signed
left operand, with zeros for an unsigned one.

Every count is defined, including the ones the hardware has no answer for:

- A count at or above the result type's width shifts every bit out, giving
  `0`. For `>>` on a negative signed value it gives `-1`, which is what
  shifting one place at a time that many times would give.
- A negative count is a runtime error (`negative shift count`). It is the
  only count with no reading: a shift in the direction the program did not
  ask for.

The bitwise operators bind tighter than comparisons: `|` and `^` bind like
`+`, and `&`, `<<` and `>>` like `*`. So `x & 1 == 0` means `(x & 1) == 0`,
which is what it looks like. The cost is that a shift next to a sum needs
parentheses (`(a + b) << 2`).

`>>` must be written as two `>` with nothing between them. `a > > b` is a
parse error, not a shift.

### 3.2 Equality

`==` and `!=` are defined on:

- two values of the same primitive type: any two integer types in any
  combination, any two float types in any combination, or two `String`s,
  `bool`s, `DateTime`s or `Duration`s;
- two values of the same enum type, and an enum value and any integer
  (the enum compares as its numeric value; two different enum types never
  compare, §2.5);
- an optional and `null`;
- an optional and a value of its inner type;
- two optionals of the same type (`null == null` is `true`).

Records, arrays, and functions cannot be compared.

This list is also exactly what a `switch` (§4.9) can dispatch on: its
clauses are matched with `==`, not with a rule of their own.

### 3.3 Arithmetic semantics

- Integer arithmetic is two's complement at the width of the result type
  (§2.1); overflow wraps.
- Integer division truncates toward zero: `-7 / 2 == -3`. The remainder
  takes the sign of the dividend: `-7 % 2 == -1`.
- Integer division or remainder by zero is a runtime error.
- Float arithmetic follows IEEE 754 at the width of the result type
  (division by zero yields ±Inf).
- String `+` concatenates. String comparisons are byte-wise
  lexicographic. A chain of `+` is one operation, not a cascade:
  `a + b + c + d` evaluates its operands left to right and builds the
  result in a single step, so a chain of *n* pieces costs one allocation
  and copies each piece once (§6.6).

**Instants and durations.** `DateTime` and `Duration` (§2.1, §6.4) are both
counts of milliseconds, and the operators defined on them are exactly the
ones whose units work out:

| Expression | Result | |
|---|---|---|
| `DateTime - DateTime` | `Duration` | how long between the two |
| `DateTime + Duration`, `Duration + DateTime` | `DateTime` | that far after |
| `DateTime - Duration` | `DateTime` | that far before |
| `Duration + Duration`, `Duration - Duration` | `Duration` | |
| `Duration * int`, `int * Duration`, `Duration / int` | `Duration` | scaling by a count |
| `-Duration` | `Duration` | |

Every other combination is a compile error rather than a silent
reinterpretation of the units. The main example is `DateTime + DateTime`,
which adds two positions on a timeline and means nothing. Comparisons (`<`,
`==`, …) are defined between two values of the same one of the two types.

A whole-number literal written where a `Duration` is expected means that
many milliseconds: `time.sleep(50)`, `d == 1000`, `Duration d = -250;`,
`time.now() + 5000`. This holds for a literal only. An `int` variable is
never implicitly a `Duration`, which keeps the unit in the type rather than
in a comment. It also does not hold for `*` or `/`, where the number scaling
a duration is a count rather than another duration. `time.milliseconds`
(§6.4) is the conversion the other way.

### 3.4 Function values

A **function value** is an anonymous function used as a value: assigned,
passed, returned, stored in fields and arrays, and called through whatever
holds it. It is written like a function declaration without the name, with
`->` before the body:

```
func-lit = ret-type "(" [ param { "," param } ] ")" "->" ( expression | block ) ;
```

The leading type is the return type, written `void` for a value that
returns nothing. Parameters are always typed. The two body forms:

```
Function(int)<int> double = int (int x) -> x * 2;        // expression body
Function()<void> hello = void () -> { print("hi"); };    // block body

print(double(21));      // 42
hello();                // hi
```

- `-> expression` returns the expression. In a void function value the
  expression is evaluated and its result discarded, like an expression
  statement.
- `-> { … }` is an ordinary function body: `return` returns from the
  function value (not from the enclosing function), and a non-void body
  that falls off the end fails at runtime, as in §5.2.
- After `->`, a `{` always starts a block. To return a record literal from
  an expression body, parenthesize it: `Car () -> ({ make: "a", age: 1 })`.
- A function value's type is exactly its written signature; no context is
  needed. The return type is never inferred from the body or the slot the
  value is assigned to: `void (int x) -> x + 1` discards the sum, and
  `int (int x) -> x + 1` returns it.
- The last parameter may be variadic, exactly as in a declaration (§5.2):
  `Function(...String)<void> shout = void (...String parts) -> { … };`

**Closures.** A function value may use variables from the function (or
top-level code) it appears in. Captures are by reference: the value and
its surroundings share the variable, and either side sees the other's
assignments, including after the enclosing function has returned:

```
Function()<int> counter() {
    int n = 0;
    return int () -> {
        n = n + 1;      // updates the one shared n
        return n;
    };
}

Function()<int> c = counter();
print(c());           // 1
print(c());           // 2
```

- Each evaluation of a function value captures the bindings visible where
  it appears. `forEach` bindings are per iteration, so closures made in
  different iterations see different elements. A `for` init variable is one
  binding for the whole loop, shared by every closure made in it.
- Module-level variables are not captured. A function value reads and
  writes them directly, like any function body.
- A captured variable is shared state. Like a record field (§2.3), a
  narrowed optional capture can be set back to null by a call between the
  check and the use. That is the runtime error of §5.5, never memory
  corruption.

**Calling.** Anything of function type can be called: a variable (`f(2)`),
a field (`h.run(2)`), an element (`funcs[0](2)`), a call's result
(`makeAdder(1)(2)`), an exported variable (`m.op(2)`). Calling the zero
value of a function type is a runtime error (§5.5). An optional function
value must be narrowed before the call:

```
Function()<void>? task;
if (task != null) {
    task();
}
```

**Declared functions are not values.** The name of a declared function can
only be used to call it: `print(add)` or `Function(int,int)<int> f
= add;` are compile errors. Wrap it in a function value when needed:
`f = int (int a, int b) -> add(a, b);`. The same holds for a method (§2.4),
where the wrapper binds the receiver: `f = int () -> car.age(2026);`.

### 3.5 The await expression

`await f` takes a `Future<T>` (§2.7) and yields its `T` result. What
happens while the result is not ready depends on where the await stands:

- **Inside an async function**, `await` is a **suspension point**: the
  function parks until `f` completes, and other ready tasks run in the
  meantime (§5.2). This is what lets async functions interleave.
- **Anywhere else** (at the top level, or in an ordinary function),
  `await` drives the scheduler until `f` is done, stepping ready tasks in
  FIFO order. Earlier-started tasks get their turn on the way.

Either way the result is remembered: awaiting the same future again yields
the same value without re-running anything. Awaiting a `Future<void>`
yields nothing and is only useful as an expression statement (`await t;`).

```
int async sum(int a) { return a; }

int x = await sum(5);          // drives the scheduler until sum is done
Future<int> f = sum(6);
print(await f + await f);    // 12 — the body ran once
```

- `await` binds like the other unary operators: `await sum(5) + 1` is
  `(await sum(5)) + 1`.
- The operand must be a plain future. An optional `Future<T>?` must be
  narrowed first (§2.3), and awaiting a null future (the zero value of a
  future type) is a runtime error (§5.5).
- A cycle of awaits, where every pending future waits on another, is a
  deadlock and stops the program (§5.5).

### 3.6 The as expression

`expression as type` gives a value at a type of the program's choosing. It
has three forms, told apart by what sits on its left. Each answers a
different shape, so they cannot be confused at a use site:

| Left operand | What it does | Answer |
|---|---|---|
| a `Json`, or `json.parse(text)` | reads a document into the type | `T`, or raises |
| a `sealed` record (§2.4) | asks whether it is a `T` | `T?` |
| a number (§2.1) | converts between number types | `T`, always |

The first two do not reinterpret anything: what comes back either really is a
value of that type, or the expression says it could not be had. The third
does reinterpret, and is described under **Converting a number** below.

**Narrowing a sealed type.** When the left side is a value of a `sealed`
record type, `as T` asks whether it is a `T`, and yields a `T?`:

```
sealed type Expr { int line; }
type IntLit extends Expr { int value; }

Expr e = someExpr;
IntLit? lit = e as IntLit;
if (lit != null) {
    print(lit.value);            // narrowed like any other optional (§2.3)
}
```

`T` must be one of the types extending the value's type; anything else is a
compile error naming the ones that are. Nothing is converted. When the
answer is not null it is the same value, at a type that says more about it.

This form yields an optional rather than raising, because its failure is of
a different kind from the JSON form's. Reading a document can fail for many
reasons and each is worth reporting, so that form raises an `Error`
carrying one. A narrowing can fail for exactly one reason (the value is not
a `T`), so there is nothing to report, and `null` says it exactly.

On a union (§2.11) the target is one of its members, `t as Text.Bytes`, and
the answer is what that member holds (`byte[]?`), since the record holding
it is the compiler's and has nothing a program can read.

**Converting a number.** When the left side is a number, `as T` gives its
value at another number type. It always succeeds, and it wraps: the
result keeps `T`'s low bits and is read back in `T`'s family, which is the
rule §2.1 already gives overflow.

```
byte a = 200;
byte b = 55;
byte sum = (a + b) as byte;      // 255
byte over = (a + b + 1) as byte; // 256 wraps to 0

int i = -1;
uint u = i as uint;              // 18446744073709551615, the same bits

float f = 0.1;
float32 g = f as float32;        // narrowed to binary32 precision
```

This is the only way to put a computed value back into a narrow type.
Every operator widens (§2.1), so `a + b` on two `byte`s is a `uint`, and no
assignment accepts a wider type. Without `as`, a program could not compute
a `byte` at all.

It covers three things that are one operation on the bits: narrowing
(`n as byte`), reinterpreting between the integer families at one width
(`i as uint`), and widening (`b as int`, which assignment would also have
done). A redundant conversion is allowed rather than an error, because
whether one is redundant depends on the host: `i as int64` is a no-op on a
64-bit machine and is not on a 32-bit one (§2.1). A diagnostic that changes
with the machine is worse than a no-op a reader can see is a no-op.

Two conversions are refused:

- **Between the integers and the floats.** That is a different operation: it
  asks about rounding, and about what to do with a value no integer can
  hold. It is refused rather than guessed at.
- **From a `DateTime` or a `Duration`.** Both are an `i64` count of
  milliseconds (§6.4), so the bits would need no work at all. For that
  reason it must not be spelled as a conversion. The unit is what the type
  means, and `time.milliseconds(x)` already says which one is meant.

`strict` (below) belongs to the JSON form; on a conversion or a narrowing it
is a compile error, since there is no document to be strict about.

**`as` only narrows.** `"hello" as Text` is a compile error, not a way of
putting a value into a union. That direction always succeeds, so it is
written by writing the value (§2.10 rule 7), and it would have to yield a
`Text` where every other `as` yields an optional or raises. Both forms of
the expression ask a question whose answer may be no, and that is what
distinguishes an `as` from an assignment.

For matching a value against several types at once, and for being told when
a case is missing, use a `switch` (§4.9). `as` is the one-off form, and
gives no coverage checking.

**Reading JSON.** On a `Json`, `as` reads the document into a type:

```
Truck t = json.parse(text) as Truck;      // from text
Circle c = someJson as Circle;            // from a Json already in hand
```

Two things produce JSON, and so there are two forms: `json.parse(text)`
(§6.3), and any expression of type `Json`. `as` on anything that is neither
of those nor a sealed record is a compile error. This form is not a cast:
no value is reinterpreted, and nothing already typed is given a different
type. It is a read, driven by the type it is given.

`json.parse` takes a `String` or a `byte[]`. A document that arrives as
bytes, such as a file (`fs.readFile`, §6.10) or a request body (`http`,
§6.16), is read in place, without the copy `string.fromByteArray` (§6.6)
would make first. The two spellings are otherwise the same parse with the
same failures.

**`as` can fail**, because what it reads did not come from the program: a
field the type requires and the document lacks, a value of the wrong shape,
or (with `strict`) a key the type does not declare. It is fallible like any
other outside-the-program operation (§2.9) and is caught the same way:

```
Truck? t = json.parse(text) as Truck catch e { print(e.message); };
```

**`strict as T`** refuses a document carrying any key the target type does
not declare, at every depth:

```
Limits l = json.parse(text) strict as Limits;   // {"rat":1} -> unknown key "rat"
```

`strict` is a modifier of the parse, not of the type, because the same type
is read strictly from a file the program owns and leniently from a payload
it does not. It is recognized only immediately before an `as`, which no expression can
otherwise be followed by, so the pair is unambiguous and `strict` remains an
ordinary identifier everywhere else (§1.4). A type that collects unknown
keys into a `json*` field (§6.3) has nothing to refuse, so the two together
are a compile error rather than a silent winner.

`as` binds tighter than every operator but looser than a call, an index or a
member access, so `json.parse(t) as Car` reads the call's result, and
reaching into that result needs parentheses: `(json.parse(t) as Car).make`.

### 3.7 The catch expression

`catch` handles an error raised while the expression on its left is
evaluated (§2.9), instead of letting it propagate. It comes in two forms.

**With a default value.** This form is an expression, so it nests anywhere:

```
String name = lookupName(id) catch "anonymous";
print((lookupName(id) catch "anonymous") + "!");
```

The result type is the one both paths satisfy: the expression's own type
normally, or the declared type of what it is being assigned to when both
the value and the default fit it. That last case is what makes
`String? n = lookupName(id) catch null;` a `String?`.

**With a block.** The `Error` is bound to a name and the block runs on the
failure path:

```
String name = lookupName(id) catch e {
    print("lookup failed: " + e.message);
    return "unknown";
};
```

The binding is visible only inside the block, and holds an ordinary `Error`
value. Because the block runs outside the guarded expression, an error
raised inside it propagates like any other. So `return e` is a rethrow.

**A block bound to a value must not fall through.** The variable would have
no value on that path, and Nio never invents one. So the block has to leave
the enclosing scope (`return`, `break`, or `continue`) unless one of two
things is true:

- **the target is optional**, in which case falling out of the block leaves
  it `null`, which is the reason to declare it optional:

  ```
  String? name = lookupName(id) catch e { print(e.message); };
  print(name);                // null when the lookup failed
  ```

- **nothing is bound**: the catch stands alone as a statement, so there is
  no value to supply and the handler just runs:

  ```
  greeting(id) catch e { print("ignored: " + e.message); };
  ```

Top-level code has no `return`, so a block there must fall through (into an
optional, or in statement position) or the default form must be used.

**Other rules.**

- `catch` binds looser than every operator, so it guards the whole
  expression on its left: one `catch` covers `f(x) + g(y)` whichever call
  fails. Parenthesize to narrow it.
- `catch` on an expression that cannot fail is a compile error, like `?.` on
  a non-optional (§2.3): the handler would be dead code. Deleting the last
  `return Error(...)` from a function then shows where its callers stopped
  needing one.
- A call returning nothing has no value for a default to replace: use the
  block form.

---

## 4. Statements

### 4.1 Variable declarations

```
int x;                 // zero value
int y = 5;
Car? maybe = null;
String const greeting = "ciao";   // const: the binding cannot be reassigned
```

A name may not be redeclared in the same scope; inner blocks may shadow
outer names. Variables must be declared before use.

`const` between the type and the name freezes the binding: a const
variable cannot be the target of `=`, `++`, or `--`, and it must be
initialized in its declaration. Const-ness is a property of the name, not
the value. The array or record a const variable refers to stays mutable:

```
int[] const nums = [1, 2, 3];
nums[0] = 9;             // fine: element write
array.push(nums, 4);     // fine: the array itself is not frozen
nums = [];               // compile error: cannot assign to "nums"

Car const c = { make: "fiat", age: 4 };
c.age++;                 // fine: field write
c = otherCar;            // compile error
```

A const may be shadowed like any other name, and the shadowing declaration
need not be const. Function parameters cannot be declared const.

### 4.2 Assignment

The target of `=` is a variable, an array element, or a record field:

```
x = x + 1;
a[0] = 5;
myCar.owner = "bob";
```

Assigning to a variable declared `const` (§4.1) is a compile error.

### 4.3 Increment and decrement

```
i++;                   // i = i + 1;
a[0]--;                // a[0] = a[0] - 1;
myCar.age++;
```

`++` and `--` are statements, not expressions: `int x = i++;` is a syntax
error, and there is no prefix form. The target follows the same rules as an
assignment target (so a `const` variable is rejected) and must be an `int`,
a `uint` or a `float`, or a sized type as wide as one of them. A narrowed
`int?` or `float?` (§2.3) counts, behaves like `x = x + 1`, and leaves the
target narrowed. The target is evaluated once: `a[f()]++` calls `f` one
time. Narrower targets such as `int8` and `uint8` are rejected: their
arithmetic widens (§2.1), so the result would not fit back.

### 4.4 `if` / `else`

```
if (x > 10) {
    print("big");
} else if (x > 5) {
    print("medium");
} else {
    print("small");
}
```

The condition must be a `bool` and is always parenthesized. Braces are
always required.

### 4.5 `while`

```
while (i < 10) {
    i = i + 1;
}
```

The condition must be a `bool` and is always parenthesized. `break` and
`continue` (§4.8) work here as in every loop.

### 4.6 `for`

```
for (int i = 0; i < myVariable; i++) {
    print(i);
}
```

The parenthesized header is `init; condition; post`:

- **init** runs once, before the first condition check. It is a variable
  declaration, an assignment, an increment/decrement, a call, or empty.
  A variable it declares is visible in the rest of the header and in the
  body; it may shadow an outer name, may not be redeclared by the body,
  and goes out of scope when the loop ends.
- **condition** must be a `bool` and is evaluated before each iteration;
  the loop runs while it is true. It cannot be empty. Write
  `while (true) { ... }` for an endless loop.
- **post** runs after each iteration, including one cut short by
  `continue` (§4.8). A `return` or a `break` skips it. It takes the same
  forms as init except a declaration.

Braces are always required.

### 4.7 `forEach`

`forEach` runs its block once per array element, in order:

```
forEach(myArray, element, i) {    // forEach(array, element, index)
    print(i);
    print(element);
}

forEach(myArray, element) {       // the index binding is optional
    print(element);
}
```

- The first argument is any expression of array type (not `T[]?`: compare
  an optional with `null` first; there is no narrowing yet, §7), or a
  `String` (below). A map is iterated through its keys:
  `forEach(map.keys(m), key) { ... }` (§6.7).
- The second and third arguments are names, not expressions: they declare
  the element and index bindings, visible only inside the block. The index
  is an `int` starting at 0.
- The bindings follow the same naming rules as parameters (no reserved
  names or import aliases) and must be distinct; they may shadow outer
  variables, and the body may not redeclare them.
- The element binding holds a copy of the element: assigning to it does not
  write into the array. Write through the index instead: `a[i] = v;`.
  (Record elements are references as usual, so `element.field = v;` mutates
  the record the array points to.)
- The array expression is evaluated once, before the loop. `return` inside
  the block returns from the enclosing function; `break` and `continue`
  (§4.8) leave the loop and skip to the next element.

**Over a `String`.** `forEach(s, r, i)` walks the string as UTF-8: `r` is
the code point, an `int`, and `i` the byte offset it starts at. The
offsets step by each character's width: `0, 1, 3, 4, 5` over `"héllo"`,
because `é` is two bytes. A byte that does not begin a valid sequence
yields `0xFFFD` and advances one byte, so the loop visits every byte of any
input exactly once and never splits a character. This is the only way to
iterate a string by code point. `s[i]` is a byte (§2.1) and stays one.
There is no function that answers the code points as an array, since a
loop that decodes as it walks costs nothing and is shorter to write.

```
forEach("héllo", r, i) {
    print(string.from(i) + " " + string.fromRunes([r]));    // 0 h, 1 é, 3 l, 4 l, 5 o
}
```

### 4.8 `break` and `continue`

`break` leaves the innermost enclosing loop or `switch` (§4.9);
`continue` skips the rest of the current iteration and moves on to the next
one. Both work in `while`, `for`, and `forEach` alike, and both take a `;`:

```
forEach(myArray, element) {
    if (element == 0) { continue; }    // on to the next element
    if (element > 100) { break; }      // done with the loop
    print(element);
}
```

- The loop they bind to is the innermost one they are written inside. There
  are no loop labels: to leave two loops at once, break the inner one and
  test for it in the outer.
- A `switch` (§4.9) binds a `break` but never a `continue`, because a
  switch is a choice and does not iterate. So a `break` written in a clause of a switch inside a
  loop leaves the switch and the loop keeps going, while a `continue` there
  belongs to the loop, as if the switch were not around it.
- What `continue` does next is the loop's own next step: `while`
  re-evaluates the condition, `for` runs its post clause and then the
  condition, `forEach` moves to the next element.
- `break` skips everything left in the iteration, the `for` post clause
  included, and resumes after the loop.
- Statements after a `break` or `continue` in the same block are
  unreachable; the compiler allows them, like it does after a `return`.
- With nothing to bind to, they are compile errors ("break outside a loop
  or switch", "continue outside a loop"). This is the case at the top
  level, in a function with no loop or switch around the statement, and in
  the body of a function value written inside one. A function value's body
  runs at call time, so a loop around the value is not one its body can
  leave.
- A loop that can `break` does not prove its condition false afterwards,
  so it does not narrow an optional the way §2.3 describes:

```
int? x = maybe();
while (x == null) { x = maybe(); }
print(x + 1);          // fine: the loop only ends when x != null

while (x == null) { break; }
print(x + 1);          // error: the break could have left x null
```

### 4.9 `switch`

`switch` matches one value against a list of constants and runs the clause
that matches:

```
switch (statusCode) {
    case 200:
        print("ok");
        break;
    case 404:
        print("not found");
        break;
    default:
        print("something else");
}
```

The subject is parenthesized and evaluated once. Each `case` label is
compared against it with the rules of `==` (§3.2), in the order written,
and the first one that matches runs its clause. If none does, a `default`
clause runs. With no `default`, the whole statement does nothing.

A clause does not fall into the next one. When its last statement
finishes, the switch is over. The `break` above is therefore optional. It
leaves the clause early, from the middle of one, and otherwise does
exactly what running off the end already does:

```
switch (n) {
    case 1:
        print("one");        // no break needed
    case 2:
        if (skip) { break; } // leaves the switch from the middle
        print("two");
    default:
        print("many");
}
```

Several labels share a clause by stacking. That is what an empty clause
body means: it runs whatever the clause after it runs.

```
switch (day) {
    case "sat":
    case "sun":
        print("weekend");
    default:
        print("weekday");
}
```

- The subject may be any type `==` accepts (§3.2): the integer and
  float types, `String`, `bool`, `DateTime`, `Duration`, an enum, and an
  optional of any of those. Records, arrays, maps, futures, function values
  and `Json` have no equality and cannot be switched on.
- A label must be a constant: a number, string, or boolean literal, a
  negated number, `null`, or an enum member. This keeps a switch a choice
  among known values (a comparison against something computed is what
  `if` / `else if` is for). It also lets the compiler reject a label
  written twice, which could only be dead code.
- A label must be comparable with the subject, by the same rule `==`
  uses. A label that could never match is a compile error, so no clause
  silently never runs. So an enum subject takes both its own members
  and plain integers (§2.5), a `Duration` subject takes a bare millisecond
  count (§6.4), and an optional subject takes `null`:

```
int? found = lookup();
switch (found) {
    case null:
        print("nothing");
    case 0:
        print("zero");
    default:
        print("something");
}
```

- `default` comes last, and at most once. A clause after it would be
  unreachable in the usual reading of a switch, so it is an error.
- Each clause body is its own scope. A name declared in one is not
  visible in another, so two clauses may declare the same one.
- A clause with an empty body and no clause after it to run is an error.
  Write `break;` to match a value and do nothing.
- `break` in a clause leaves the switch (§4.8); `continue` in one belongs
  to the loop around the switch, and is an error with no loop there.
- A switch establishes no narrowing of its own, and the facts §2.3 tracks
  survive it as they survive a loop: what was known before it still holds
  after, unless some clause assigns the path.

**Matching a type.** When the subject is a value of a `sealed` record type
(§2.4), a clause may match the subject's type instead of its value, and
bind it at that type for the clause's body:

```
sealed type Expr { int line; }
type IntLit extends Expr { int value; }
type Ident  extends Expr { String name; }
type Binary extends Expr { String op; Expr left; Expr right; }

String render(Expr e) {
    switch (e) {
        case IntLit n:
            return string.from(n.value);
        case Ident n:
            return n.name;
        case Binary n:
            return "(" + render(n.left) + " " + n.op + " " + render(n.right) + ")";
    }
}
```

Without a `default`, the clauses must cover every type extending the
subject's, and the compiler reports the ones missing. This is the purpose
of the form: adding `type Unary extends Expr` later is a compile error at
every switch that does not handle it, naming `Unary`. No case quietly falls
through. Writing a `default` is how a switch opts out. A switch that cares
about three of nine types says so, and is not asked about the other six.

- The binding is required: `case IntLit n:`, never `case IntLit:`. The
  clause exists to use it, and requiring it keeps a bare `case Foo:` a
  value label whatever `Foo` names.
- A label may name a union's member (§2.11): `case Text.Bytes b:`, and
  `case tx.Text.Bytes b:` for an imported one. The binding is then what the
  member holds, not the record holding it.
- The two kinds of label do not mix. A switch matching types cannot also
  match values, and a clause matching a type cannot stack with another (a
  stack shares one body, and each of these names the value differently).
- Each type is matched at most once. A `default` on a switch that already
  covers every type is an error, because nothing could reach it. This is
  the same reason a clause written after `default` is an error.
- Only a sealed subject can be matched this way. Any other type has no
  known set of types extending it, so coverage could not be checked.
- An exhaustive type switch counts as terminating. The set is closed and
  every member is named, so some clause always runs. A function whose
  clauses all return therefore needs no return after the switch, as
  `render` above shows.
- Each clause body is its own scope, as for any switch, so every clause may
  bind the same name.

### 4.10 `return`

`return expr;` in a function with a return type; bare `return;` in a void
function. `return` is not allowed at the top level.

---

## 5. Declarations and program structure

### 5.1 Programs

A program is one or more files. The file handed to the compiler is the
**entry file**; other files enter the program by being imported (§5.4).
Each file is a sequence of imports followed by top-level statements.
Execution runs each imported module's top-level statements once
(dependencies first, §5.4), then the entry file's top-level statements in
order, then any still-pending async work (§5.2) in call order, then exits
with code 0.

A top-level statement cannot reach a variable declared below it. Since
statements run in order, a top-level variable has no value until its own
declaration runs, so reading one earlier is a compile error. Naming it
directly is the ordinary case, because the name is not in scope yet. The
rule holds through function calls too:

```
print(a());              // error: a() reads "rows", declared later on line 2
String[] rows = ["x"];
int a() { return b(); }
int b() { return rows.length; }
```

Every line there is individually in order: `b` names `rows`, which is
declared above `b`, and `a` names `b`, which a body may do because mutual
recursion needs it. The rule looks at the reach of the statement: every
variable any function it calls can get to, however many calls away, must
be declared above it. A function value counts as reaching what its body
reaches at the point it is created, not the point it is called.

Reordering is always the fix: move the declaration above the statement, or
the statement below the declaration. The rule does not restrict two
things. A function body may name a function declared anywhere in the file,
which makes mutual recursion possible. A variable in another module is
never a problem, because a dependency's top-level code has finished before
any importing file's begins (§5.4).

### 5.2 Function declarations

The return type comes first, like in a variable declaration, and is
required: a function that returns nothing writes `void`. There is no
`function` keyword:

```
int add(int a, int b) {
    return a + b;
}

void log(String msg) {          // returns nothing
    print(msg);
}
```

- Functions are declared at the top level only, and each name once. The
  only other place a function body may be written is a type declaration,
  whose methods (§2.4) follow every rule here and add a receiver.
- Functions are *hoisted*: a function may be called from code that appears
  before its declaration, and functions may recurse (including mutually).
- A function body sees global variables declared above it in the file.
- Parameters follow assignability (§2.10); arguments are evaluated left to
  right.
- The compiler does not verify that every path returns; a non-void function
  that falls off the end of its body fails at runtime (§5.5).

**Variadic parameters.** `...` before the type of the last parameter
makes it collect the trailing arguments of every call:

```
void customPrint(int myInt, ...String logs) {
    print(myInt);
    forEach(logs, log) {           // logs is a String[]
        print(log);
    }
}
customPrint(0, "hello", "world", "!");   // logs is ["hello", "world", "!"]
customPrint(7);                          // logs is []
```

- Inside the body the parameter is an array of the written type:
  `...String logs` binds `logs` as a `String[]`, with `.length`, indexing,
  `forEach`, and everything else arrays have (§2.2). It is an ordinary
  parameter in every other way: it can be captured (§3.4), reassigned, and
  passed on.
- Only the last parameter may be variadic, and only one may be. A function
  with a variadic parameter takes at least its fixed parameters. The call
  collects everything after them, and none at all is an empty array.
- Each collected argument is checked against the element type under the
  usual assignability rules (§2.10); the array is built at the call site,
  left to right with the other arguments.
- There is no spread: an existing array cannot be handed to a variadic
  parameter as its arguments (§7). Pass a `String[]` parameter instead when
  that is what callers have.
- Variadic works on function declarations and function values alike, and on
  `async` and fallible ones; the type that names such a signature writes the
  same `...` (§2.6).

**Async functions.** `async` between the return type and the name declares
an *async* function:

```
int async sum(int a) {
    return a;                  // the body returns the declared type as usual
}

void async log(String m) {     // void async
    print(m);
}
```

An async function's body is compiled into a **state machine**: a resumable
task whose
locals live on the heap, split at its `await` expressions. Calling the
function does not run the body. The arguments are evaluated and captured
at the call, the call returns a `Future<int>` (`Future<void>` for a void
one, §2.7) immediately, and the task joins the scheduler's ready queue.

The **scheduler** steps ready tasks in FIFO order. A task runs until its
body completes or reaches an `await` of a still-pending future. Then it
*suspends*: it parks on that future and gives the scheduler back, and
other ready tasks run. When the awaited future completes, the parked task
returns to the ready queue and later resumes exactly where it stopped.
Independent tasks therefore interleave at their await points:

```
void async task(String name) {
    print(name + ": step 1");
    await announce(name + ": helper");   // suspends; the other task runs
    print(name + ": step 2");
}
task("A");
task("B");
// A: step 1, B: step 1, A: helper, B: helper, A: step 2, B: step 2
```

The scheduler runs when something drives it. An `await` outside an async
function (§3.5) steps tasks until its future completes. After the entry
file's last top-level statement, the runtime drains the queue, so an async
call whose future is dropped still runs ("fire and forget"). Work created
during the drain joins it. Completion callbacks are registered with
`async.run` (§6.8). Everything is single-threaded and deterministic:
tasks interleave only at awaits, never preempt, and never run in parallel.
Work outstanding outside the program does not change that, such as a
`time.sleep` timer (§6.4) or a child started by `process.child.run`
(§6.12). When nothing is ready to run, the scheduler waits on all of it at
once, so several children make progress simultaneously while the
program's own code still runs one statement at a time. If every live task
is parked on another (a cycle of awaits), nothing can complete, and the
program stops with a deadlock error (§5.5).

This is permanent, not a limitation of v0.1. Parallelism in Nio is
process-level. Threads over a shared heap are refused, because they would
cost the language its guarantee that no data race is expressible. §7.1 has
the reasoning and what to do instead.

Inside the body, parameters hold the values captured at the call, while
global variables are read when the body actually runs, like in any
function. In an expression, operands evaluated before an `await` are
captured before the suspension, left to right. A variable read before an
await is not re-read after it. Everything else about an async function is
ordinary: `export` works, the body may await other futures (including
recursively), and `return` checks against the declared type.

### 5.3 Scoping

Scoping is lexical. Each block (`{ … }`) introduces a scope. A declared
function's body sees only its parameters, its own locals, global variables,
and other functions. Declarations do not nest. A *function value* (§3.4)
additionally closes over the variables visible where it is written,
capturing them by reference.

### 5.4 Modules and imports

Every file is a **module** with its own scope. Nothing in a file is visible
to other files unless it is exported, and even then it is only reachable
through an import alias. So two files may both declare (and export) an
`int x` without conflict.

**Imports** appear at the top of a file, before any other statement:

```
import 'time';                 // built-in module, alias "time"
import 'json' as j;            // explicit alias
import 'geo';                  // ./geo.nio, alias "geo"
import 'lib/helpers' as h;     // ./lib/helpers.nio, alias "h"
```

- The path is a string literal. `'time'`, `'json'`, `'array'`, `'string'`,
  `'map'`, `'async'`, `'os'`, `'path'`, and `'fs'` name the
  built-in standard library modules (§6); every other path names a `.nio` file,
  resolved relative to the importing file's directory (the `.nio`
  extension may be omitted). Use `'./time'` to import a local file named
  `time.nio`.
- Without `as`, the alias is the file's base name without the extension
  (`'lib/helpers'` → `helpers`). If that is not a valid identifier
  (`'string-utils'`), the import must use `as`.
- Aliases must be unique within the file, and may not be `print`,
  `printInline`, `getType`, or a built-in type name. No library needs an
  exception: the built-in type names begin with a capital (`String`,
  `DateTime`, `Duration`, `Json`, `RegExp`, `Socket`, `Listener`), and the
  names of their libraries (`string`, `time`, `json`, `regexp`, `net`) are
  lower case.
- An import alias is reserved within its file: no variable, function, type,
  or parameter may reuse it, so aliases can never be shadowed. This holds
  only within its file. A module's name is an ordinary identifier
  everywhere it is not imported (§1.5), so `import 'path' as p` leaves
  `path` free to name something else here, and a file with no
  `import 'path'` at all is free to begin with.
- Importing the same file twice under different aliases is allowed; both
  aliases denote the same module. Import cycles are compile errors.

**Exports.** `export` before a top-level declaration makes it available to
importing files:

```
export int counter = 0;
export int add(int a, int b) { return a + b; }
export type Point { int x; int y; }
export enum Status { OK: 200, NOT_FOUND: 404 }
```

An importer accesses exports through the alias:

- `m.f(args)` calls an exported function; `m.f` without a call is a
  compile error (§3.4).
- `m.x` reads an exported variable and `m.x = v` assigns to it (all
  importers and the module itself see the same variable). If the variable
  is declared `const` (§4.1), assigning to it (from the importer or the
  defining module) is a compile error.
- `m.Point` names an exported record type in type positions:
  `m.Point p = { x: 1, y: 2 }`, `m.Point[]`, `m.Point?`. Its methods (§2.4)
  come with it and are called on the value, not through the alias:
  `p.distance()`.
- `m.Status` names an exported enum the same way, and `m.Status.OK` names
  one of its constants in expressions (§2.5).

Record and enum types are nominal per declaration: a `Point` exported by
`geo` is the same type everywhere it flows, and distinct from any other
`Point`.
Values of an unexported record type may flow through exported functions and
be used normally; only naming the type requires an export.

A built-in module's alias works the same way where it has something to name:
`os.Cpu` (§6.9) is the one type any built-in module exports. Because these
types are reached through the alias, their names stay available to user
code. A program may declare its own `Cpu`, and it is a different type.

**Re-export.** A module republishes part of an imported module's surface as
its own, so that a group of files can present one surface to importers:

```
import './der' as der;

export der;                    // everything der exports
export der show Tlv, readTlv;  // only these
export der hide readLength;    // everything except these
```

- The statement names an import alias, not a path, so the path is
  written once and a re-export adds no dependency of its own. It follows
  that a re-export can form no import cycle that the imports do not already
  have.
- `show` and `hide` are contextual: neither is a keyword, and a file is free
  to declare something named `show` or `hide`.
- A re-export publishes names and brings none into scope. A file that also
  uses the module keeps using it through the alias its import bound. The
  re-export changes only what importers of this file see.
- A republished name is the same declaration, not a copy. A record keeps
  the identity its declaration gave it (§2.10), so a value passes freely
  between the two names, and a function keeps the body the declaring module
  emitted.
- Republishing carries through a chain: if `mid` republishes `prim` and
  `top` republishes `mid`, then `top` publishes `prim`'s names.
- Each of these is a compile error: naming a module the file does not
  import; naming a built-in module, which every file can import for itself;
  naming something the target does not export; and publishing one name from
  two places, or from a re-export and a declaration of this file.

`export` on a declaration and a re-export therefore do two different jobs.
The first states what this file offers its neighbours. The second states
what a group of files offers the world.

**Initialization order.** Each module's top-level statements run exactly
once, before any module that imports it (the compiler orders modules by
their imports; the entry file runs last). A module imported by several
files is initialized only once.

### 5.5 Runtime errors

These conditions abort the program with a message on stderr
(`runtime error: …`) and exit code 1:

- array index out of range;
- `array.pop` on an empty array;
- integer division or remainder by zero;
- a non-void function (declared or function value) falling off the end
  without returning;
- calling a function value that was never assigned (the zero value of a
  function type, §2.6);
- awaiting a future that was never assigned, or passing one to
  `async.run` (the zero value of a future type, §2.7);
- reaching an exhaustive type switch with a sealed value that was never
  assigned (the zero value of a sealed type, §2.4). It is reported at the
  switch, since a switch with no `default` promises that some clause
  always runs there;
- a deadlock: every pending future is awaiting another (a cycle of awaits,
  including a future awaiting its own result, §3.5);
- reading a narrowed optional whose value was set back to null after its
  null check, through an alias or a call (§2.3);
- `time.date.fromText` with malformed input, and `time.duration` with a
  string that is not a count and a unit (§6.4);
- `string.split` with an empty separator (§6.6);
- `string.substring` with out-of-range bounds (§6.6);
- serializing a non-finite float to JSON;
- an `Error` that reached the top of the program with nothing catching it
  (§2.9). Its own message is the message. An error raised by a future
  nobody awaited is reported the same way when the program runs out of
  work.

The conditions above are bugs, and none of them can be caught: `catch`
(§3.7) handles the `Error` a function chose to return, not these.

The message is followed by the call chain, innermost first:

```
runtime error: index 99 out of range (array length 3)

in:
  deepest
  middle
  top
  main
```

A method is named `Type.name`, and a function literal by where it was
written (`<function literal in outer>`). The chain is partial. It is read
from the frames the collector already keeps, not from a mechanism of its
own, and that decides what it can name. Three things therefore do not
appear:

- a function holding no heap value (one doing only arithmetic) contributes
  no entry, because it needs no frame. A chain can have gaps in the middle
  for this reason;
- a standard-library function that fails inside its own C implementation
  contributes no entry, so the chain names the Nio function that called it;
- an `async` body contributes no entry, because its locals live in a frame
  the scheduler owns rather than on the stack. An await chain therefore
  names what drove the scheduler.

For an uncaught `Error`, the chain means something different from the
other entries in the list above. Errors are values and propagation is a
`return` (§2.9), so every frame the error passed through has already been
popped by the time nothing is left to catch it. The chain therefore names
where the program stopped, not where the error was made. For an error that
reached the top, that is the top. An `Error` carries the `message` and
`code` a handler was meant to act on, and no origin beyond them.

Set `NIO_NO_TRACE=1` to print the message alone, which suits a program
that compares stderr exactly. The chain is always emitted (there is no
build flag to turn it off) and costs a running program nothing, since it
names frames that are pushed for the collector either way.

The chain is what a program reports about itself as it stops. A debugger
reads different information, and that is requested separately:
`nio build -g` describes the program in DWARF. It gives every function
under the name written here, every statement at its file, line and column,
and every parameter, local and top-level variable named and typed,
including one in an `async` body across an `await`. This description is
complete where the chain is partial, because none of it costs anything
while the program runs. Without `-g` none of it is emitted.

### 5.6 Memory

Strings, arrays, records, optionals and function values live on the heap
and are managed by a garbage collector; there is no way, and no need, to
free a value explicitly. Memory is reclaimed once a value is unreachable,
that is, once it can no longer be named through any variable in scope, any
global, or any field or element of a value that is itself reachable.

The collector is precise (it reclaims everything unreachable, including
values that refer to each other in a cycle) and non-moving (a value keeps
its address for its whole lifetime). Collection happens during allocation,
so a program that does not allocate never pauses. Nothing about it is
observable from the language: there is no finalizer, no destructor, and no
guaranteed point at which a given value is reclaimed.

Setting `NIO_GC_STRESS=1` in the environment makes a compiled program
collect at every allocation. It is many times slower and is meant for
testing the compiler itself. `NIO_GC_STATS=1` writes one line to standard
error as the program exits, reporting how many collections ran and how long
they spent marking and sweeping; both settings are diagnostics, and neither
changes what a program computes.

### 5.7 Representation

Every value a program can name occupies its natural width, in every
container it can be named in. The width is the type's, not the machine's:

| type | width |
| --- | --- |
| `bool` | 1 byte (a byte, not a bit) |
| `int8`, `uint8` / `byte` | 1 byte |
| `int16`, `uint16` | 2 bytes |
| `int32`, `uint32`, `float32` | 4 bytes |
| `int64`, `uint64`, `float64` | 8 bytes |
| `DateTime`, `Duration` | 8 bytes (§2.1: both are a count of milliseconds) |
| an enum (§2.5) | as `int` |
| `int`, `uint`, `float` | the machine's word (§2.1) |
| `String`, `T[]`, `T[N]`, `T?`, `Map<K, V>`, `Future<T>`, a record, a function value, `Json`, `RegExp`, `Socket`, `Listener` | one pointer |

The containers are a record's fields, an array's elements, a map's keys and
its values, and the box an optional points at. A `byte[1000]` is a thousand
bytes and a `Map<String, int32>` spends four bytes per value, in the same
way a `String`'s bytes are its bytes.

Nothing is packed below a byte, so an array of `bool` is one byte per
element rather than one bit. This keeps every element of an array a value
in its own right rather than a bit position in a word.

A record's fields are laid out in declaration order and never reordered,
each at the next offset that is a multiple of its own width, and the record
is aligned to its widest field. A type that `extends` another (§2.4) lays
the base's fields out first, in the base's own order, so the two agree on
the prefix. Declaration order costs padding that reordering would recover:
`{ uint8 a; int64 b; uint8 c; }` spends 24 bytes where a reordering
implementation would spend 16. Declaration order is also what calling C
needs. A language that reorders has to grow a second layout rule for the
values it hands across the boundary. Nio has one layout rule.

None of this is observable from within the language. There is no
`sizeof`, no address-of, no pointer arithmetic and no reinterpreting cast,
and §5.6 makes the collector unobservable too. Representation therefore
decides how much memory a program uses and nothing about what it
computes.

The rule covers the values a program can name. The blocks a compiler
builds for its own purposes, such as the frame of a suspended async
function (§3.5) or the captures behind a function value (§3.4), are not
types, hold nothing a program can take the width of, and are not covered.

### 5.8 Native interop

A module may bind functions written in C (or Objective-C). This lets a
library reach a system capability the built-in modules do not cover (a
windowing system, a database's client library) without the compiler
knowing it exists. The mechanism is three statements, and all three words
are contextual (§1.4):

```
native source './webview_native.m';
native flags darwin '-framework Cocoa -framework WebKit';

extern int nw_create(int width, int height, String title);
```

**`extern`** declares a function whose definition is in a native source.
The shape is a function declaration with no body. The name is the C
symbol, unmangled, so it must be unique across the whole program the way
any C symbol is. Two modules may both declare a symbol, and then their
signatures must agree, which the compiler checks. Names beginning `rt_`,
and `main`, are rejected: every program already links the runtime. An
extern function is called like any other function of its module. It
cannot be `async` and cannot be variadic. It is never fallible, because it
has no body to raise from, and the error convention at the C boundary
belongs to the built-in modules. It cannot be exported: a library wraps
its externs in ordinary Nio functions, which is also where raising,
records and optionals live.

The types that cross are the ones whose unit C can hold directly
(§5.7, runtime.h): the integer families as `int64_t` (`bool` included,
0 or 1; the narrow types are in range but not narrowed), the float
family as `double`, `String` as `Str*`, and arrays of any of those as
`Arr*`. The return type may also be `void`. Records, optionals, maps,
function values, futures and the opaque types do not cross; a wrapper
converts on this side of the boundary.

**`native source`** names a file compiled and linked into the program.
The path resolves relative to the declaring file, exactly as an import's
does, and must end `.c`, `.m` or `.h`. At build time each named file is
written under its base name into a directory belonging to the module that
declared it. A module's own sources therefore sit together and one may
`#include` another by name, while two modules may each ship a `util.c`
without collision. Two sources of one module may not share a base name.
The `.c` and `.m` files are handed to clang with the same flags as the
runtime's own; a `.h` is written and only informs. The file may
`#include "runtime.h"`, which the build puts on the include path. There is
no per-OS form of the statement: a file that differs by platform selects
with the preprocessor, the way the runtime's own per-OS bodies do.

**`native flags`** adds linker arguments, split on spaces, when the named
platform (`darwin`, `linux`, `windows`) is the one compiling. The compiler
builds only for its host (§5.7), so the choice is made at compile time and
nothing platform-conditional survives into the build. With no platform
named the flags apply everywhere.

The C side inherits the runtime's contracts. Its functions run on the
program's thread, under the program's collector:

- a pointer value (`Str*`, `Arr*`) held across anything that allocates
  must be rooted in a `GCFrame` exactly as the built-in modules' own C does
  (§5.6, runtime.h);
- memory answered to the program must come from the runtime's allocators
  (`rt_str_alloc`, `rt_arr_new`), never `malloc` for a language value;
- a C function must not keep a language pointer after it returns unless it
  also arranges to root it.

Nothing checks any of this. The runtime itself relies on the same
unchecked rules, and that is the cost of allowing native code at all.

---

## 6. Standard library

The standard library is `print`, `printInline`, and `getType` plus nineteen
built-in modules, `json`, `time`, `array`, `string`, `map`, `async`, `os`,
`path`, `fs`, `process`, `test`, `regexp`, `net`, `http`, `crypto`, `tls`,
`x509`, `math` and `random`. The three functions are always available. The
modules must be imported before use (§5.4):

```
import 'json';
import 'time';           // or: import 'time' as t;
import 'array';
import 'string';
import 'map';
import 'async';
import 'os';
import 'path';
import 'fs';
import 'process';
import 'test';
import 'regexp';
import 'net';
import 'http';
import 'crypto';
import 'tls';
import 'x509';
import 'math';
import 'random';
```

Fifteen of them are implemented in C, in the runtime the compiler links into
every program. `test` (§6.13), `http` (§6.16), `tls` (§6.18) and `x509`
(§6.19) are implemented in Nio. They need nothing the language does not
already have, so each is compiled from source like any module a program of
its own would import, and the C files it needs are the ones its own imports
pull in. Whether a module is written in C or in Nio changes nothing about how
it is imported or used.

Built-in modules export functions, and four of them also export types
(`os.Cpu`, §6.9, `fs.Stat` and `fs.DeleteOptions`, §6.11,
`process.ChildRunResult`, §6.12, and `net.Options` and `net.Datagram`,
§6.15); no other built-in module names a type of its own. The types
`DateTime` and `Duration` (§2.1) are part of the language and need no import.
The same holds for `Json` (§6.3.1), `RegExp` (§6.14), `Socket` and
`Listener` (§6.15), the `Map` type (§2.8), async functions, futures, and
`await` (§5.2). Only the module functions (`map.keys`, `async.run`,
`regexp.create`, `net.read`, …) need their imports. A `RegExp`, `Socket` or
`Listener` declared in a file that does not import the module that makes one
is legal and useless, since nothing else can make one.

Almost nothing in the library can fail. A standard library function either
answers or, in the few cases where the program has already gone wrong, stops
it with a runtime error (§5.5). The exceptions are the functions whose
failures come from outside the program. These are *fallible* (§2.9):

- every function of `fs` except `fs.exists`, because a file that is not
  there is not a mistake in the program;
- the parsers `string.toInt`, `string.toUint` and `string.toFloat` (§6.6),
  because text that is not a number is the expected case for a parser;
- `regexp.create` (§6.14), for the same reason, since a pattern is text too;
- the reads of `process.stdin` (§6.12), because a stream the operating
  system refuses is not something the program did wrong;
- every function of `net` (§6.15) except `net.close`, because a network is
  outside the program almost by definition;
- the decoders `crypto.base64Decode` and `crypto.hexDecode` (§6.17), which
  read text the same way the `string` parsers do;
- `crypto.chacha20Poly1305Open` and `crypto.x25519SharedSecret` (§6.17).
  Their failures (a message that does not authenticate, a peer key that
  would fix the shared secret) are the security checks, and raising them
  makes the checks impossible to skip. The same applies to
  `crypto.rsaVerifyPkcs1v15`, `crypto.rsaVerifyPss` and
  `crypto.ecdsaVerify`: they answer nothing and fail instead, so there is no
  boolean to forget to test;
- every function of `x509` (§6.19), whose subject is bytes somebody else
  chose.

`process.child.run` has the same kind of failure (a program the system
cannot start), but it is carried on a future rather than returned, so it is
caught at the `await` (§6.12).

### 6.1 `print(values…)` and `printInline(values…)`

Writes values to standard output. `print` ends the line; `printInline`
writes nothing around them.

```
printInline("hello, ");
print("world");           // hello, world
print("x", 1, true);      // x 1 true
print();                  // an empty line
```

- Both take any number of arguments, written left to right with a single
  space between two of them, and nothing before the first or after the
  last, except `print`'s newline. `printInline()` writes nothing at all;
  `print()` writes just the newline.
- Every argument must be a scalar (`String`, any number type, `bool`,
  `DateTime`, `Duration`, or an enum) or an optional of one of these.
- Strings and `DateTime`s print raw (unquoted): `printInline("hi")` → `hi`.
  A `Duration` prints as the number of milliseconds it is.
- An enum value prints its member's name (`print(HttpResponses.OK)` →
  `OK`). A value no member has (the zero value of an enum without a
  0-valued member) prints as its number (§2.5).
- Floats print in the shortest form that round-trips at their own width, so
  a `float32` holding `0.1` prints `0.1` where the `float64` nearest to it
  prints `0.10000000149011612`; bools print `true` / `false`.
- A present optional prints its inner value; an absent one prints `null`.
- Records, arrays, and maps cannot be printed directly. Serialize them
  first: `print(json.toText(myCar))` → `{"make":"toyota","age":4}`.
- Function values cannot be printed.
- The arguments are all evaluated, left to right, before anything is
  written: an argument that raises (§2.9) propagates out of the call and
  leaves nothing on the line.
- `string.from(v)` (§6.6) is this same text as a string rather than on
  standard output, for every value listed here and by these same rules.

### 6.2 `getType(value)`

Returns the name of `value`'s type as a `String`.

```
int8[] bytes = [1, 2];
print(getType(bytes));            // int8[]
print(getType(bytes[0]));         // int8
print(getType("hi") + "!");       // String!
```

- Takes exactly one argument, of any type but `void`. Unlike `print`,
  records, arrays, maps, function values and futures are all accepted.
- The name is the type's canonical spelling, the same one the compiler's
  diagnostics use: `int8[]`, `String?`, `int[3]`, `Function(int)<int>`,
  `Future<int>`, `Map<String, int>`, `null`.
- `byte` is an alias for `uint8` (§2.1), and the alias is not remembered: a
  `byte` value reports `uint8`. `int`, `uint` and `float` are not aliases,
  and report themselves: `getType(1)` is `int` on every machine, whatever
  width that machine gives it.
- A type declared in another module is named by that module, not by the
  importer's alias (§5.4): with `import 'shapes' as sh;`, an `sh.Circle`
  reports `shapes.Circle`. A module inside a package is named by the package
  (the last segment of its path, without a leading `nio-`), followed by its
  path below the package when it is not the package's entry: `log.Logger`,
  `log/rotate.Handler`. When the build holds two bands of that package, the
  version follows the package name (`log@v0.2.5.Logger`), since the two are
  different types and the name is what tells them apart. An anonymous record
  type (§2.4) reports the field path that gives it its identity, such as
  `Car.repairs`.
- The argument is not flow-narrowed (§2.3): a value declared `String?`
  reports `String?` even inside an `if (s != null)` where it is otherwise
  usable as a `String`. `getType` names what was declared, never less.
- The answer is fixed at compile time. It is the static type, and there is
  no run-time type information behind it. The argument is still evaluated,
  so any side effects in it still happen; only its value is discarded.

### 6.3 `json`

| Function | Signature | Description |
|---|---|---|
| `json.toText(v)` | `any → String` | Serializes `v` to a JSON string. |
| `json.parse(s) as T` | `String → T!` | Reads a JSON string into a value of type `T` (§3.6). |
| `json.parse(s)` | `String → Json!` | Reads a JSON string into a `Json`, whatever shape it has. |
| `json.parse(b) as T` | `byte[] → T!` | The same read from bytes, in place. |
| `json.parse(b)` | `byte[] → Json!` | The same read from bytes, in place. |
| `json.getType(v)` | `Json → json.Type` | Which of the seven shapes `v` has. |
| `json.length(v)` | `Json → int` | An array's or object's size; `0` for anything else. |
| `json.keys(v)` | `Json → String[]` | An object's keys in document order; empty for anything else. |
| `json.remove(v, k)` | `Json, String → void` | Drops key `k` from an object. Absent is not an error. |
| `json.push(v, x)` | `Json, any → void` | Appends `x` to an array. |
| `json.asInt(v)` | `Json → int!` | The number `v` is, as a whole number. |
| `json.asFloat(v)` | `Json → float!` | The number `v` is. |
| `json.asText(v)` | `Json → String!` | The string `v` is. |
| `json.asBool(v)` | `Json → bool!` | The bool `v` is. |

Type mapping:

| Nio value | JSON |
|---|---|
| `int`, `int8`, `int16`, `int32`, `int64` | number (parsing rejects one outside the type's range) |
| `uint`, `uint8`, `uint16`, `uint32`, `uint64` | number (parsing rejects a negative one, and one outside the type's range; a value above an `int64`'s range is written and read back exactly) |
| `float`, `float32`, `float64` | number (shortest form that round-trips at that width; non-finite is a runtime error, and parsing rejects one too large for a `float32`) |
| `String` | string (with `\" \\ \n \t \r \b \f \uXXXX` escaping) |
| `bool` | `true` / `false` |
| `DateTime` | string, `"YYYY-MM-DDTHH:MM:SSZ"` |
| `Duration` | number, the count of milliseconds |
| enum | number (the member's value, §2.5) |
| `T[]` | array |
| `T?` | inner value, or `null` |
| `Map<String, V>` | object, entries in insertion order (§2.8). JSON object keys are strings, so a map with any other key type has no JSON form: serializing one on its own is a compile error, and as a record field it is omitted (below) |
| `Json` | whatever shape the value has |
| union (§2.11) | the member's payload, bare (the member is representation and never appears); parsing picks the member by the value's kind (below) |
| record | object, fields in declaration order, each keyed by its `'json:…'` name when it has one (§2.4); fields whose optional value is `null` are omitted, as are fields with no JSON form (below) |

`json.toText(null)` is a compile error.

Function values have no JSON form, and neither does a map keyed by anything
but `String`. A record field that holds one (directly, or through arrays,
optionals, and map values) is omitted from the object, as a null optional
field is. The field is still readable in Nio; it does not appear in the
output. A record all of whose fields are omitted serializes as `{}`.

Serializing such a value where there is nothing to omit it from is a compile
error: on its own (`json.toText(f)`), or as the element of an array or
optional (`Function()<int>[]`, `Function()<int>?`), where dropping it would
leave no value to write in its place.

```
import 'json';
Car myCar = { make: "toyota", age: 4 }
json.toText(myCar)      // {"make":"toyota","age":4}
myCar.owner = "alice";
json.toText(myCar)      // {"make":"toyota","age":4,"owner":"alice"}
```

`json.parse` reads text back into a value. It has no type of its own, so it
must be given one with `as` (§3.6); the type drives the parse:

```
import 'json';

type Engine { float liters; int power 'json:engine_power'; }
type Truck  { String make; Engine engine; String? nickname; }

String text = '{"make":"volvo","engine":{"liters":12.8,"engine_power":550}}';
Truck t = json.parse(text) as Truck;
```

- Object keys the type does not declare are dropped, however deeply they
  nest, so a program may name only the fields it uses out of a large
  payload.
- A field renamed with `'json:…'` (§2.4) is read under its JSON key, the
  same key `json.toText` writes.
- An optional field may be absent or `null`; it becomes `null`. Every other
  field must be present, since a non-optional field is guaranteed to hold a
  value.
- A function-typed field is never read (it has no JSON form, above) and
  keeps its zero value.
- A `Map<String, V>` target is the shape for objects whose keys are data
  rather than declared fields (`{"user-1": …, "user-2": …}`): every key of
  the object becomes an entry, in document order. A duplicate key in the
  document overwrites the earlier entry, as `m[k] = v` would. A map keyed by
  anything but `String` cannot be parsed.
- The type mapping above is read in reverse: a `DateTime` accepts the two
  forms `time.date.fromText` accepts (§6.4), an enum accepts its member's
  number, and an `int` accepts only a whole number.

The target type may be any type with a JSON form, except that `T[N]` is
rejected: a fixed-size array shares its type descriptor with `T[]`, so the
length cannot be enforced. `as` on anything but a `json.parse` call is a
compile error.

Anything the text cannot supply is a failure the caller can catch (§2.9). It
is not a zero value and it does not stop the program. Such failures are a
malformed document, a missing non-optional field, `null` for a non-optional
type, a value of the wrong shape, and trailing text after the value. The
message names the JSON path at fault, e.g.
`cannot parse JSON: expected a number at parts[1].n`. Text a program reads
came from outside it, so a document it cannot use is the same category of
failure as a file that is not there. This is why `string.toInt` raises too.

**Unknown keys: the three policies.** What happens to a key the target type
does not declare is the choice a program most often needs to make. Each
choice is spelled according to what it costs:

| | written | why there |
|---|---|---|
| **drop** | nothing (the default) | no storage and no behaviour, so nothing to write |
| **reject** | `strict as T` (§3.6) | no storage → a property of the parse |
| **preserve** | a `json*` field | needs a slot → a field on the type |

```
type Config { String name; int port; }

json.parse(text) as Config           // "owner" is dropped
json.parse(text) strict as Config    // "owner" is an error
```

**Preserving unknown keys: the `json*` field.** A record may declare one
field annotated `'json*'`, of type `Json`. It is not a key of its own. On the
way in it collects every key the record does not declare, and on the way out
its entries are written beside the declared fields, not nested under a name
of their own. This lets a program read a document, change one thing, and
write it back without losing the rest of it:

```
import 'json';

type Tls    { bool enabled; String cert;  Json extra 'json*'; }
type Config { String name; int port; Tls tls; Json extra 'json*'; }

Config c = json.parse(text) as Config;
c.port = c.port + 1;                 // typed: checked at compile time
c.tls.enabled = true;
c.extra["migrated"] = true;          // a key the document never had
print(json.toText(c));               // every unknown key comes back out
```

- A type may have at most one, and it must be a `Json`, since what it
  collects are values of whatever shape the document had.
- Declared fields are written first, in declaration order, then the
  collected keys in document order. That is the order they were inserted,
  which §2.8 guarantees for every map in the language.
- A declared field wins: a key put into the catch-all by hand that the
  record also declares is skipped on the way out, so the document never
  carries it twice.
- Preservation is per record. For unknown keys inside `tls` to survive,
  `Tls` needs a catch-all of its own, as it has above. A nested type without
  one still drops what it does not declare.
- `strict` and a `json*` field on the same read are a compile error: one
  says refuse every unknown key and the other says keep them all.

**Unions: choosing a member by kind.** A union (§2.11) serializes as its
member's payload, bare: the member record is representation, and its
generated field never appears in the output. Parsing reads the same fact
backwards. The JSON kind of the value (string, number, `true`/`false`,
array, object) chooses the member, so a field that is sometimes a string and
sometimes a number round-trips exactly as the document wrote it:

```
import 'json';

union Price { String, int Amount }
type Item  { Price price; String name; }

String doc = '[{"price":"$4.99","name":"a"},{"price":100,"name":"b"}]';
Item[] items = json.parse(doc) as Item[];
switch (items[1].price) {
    case Price.String s: print("marked " + s);
    case Price.Amount n: print(n);                 // 100
}
print(json.toText(items) == doc);                  // true
```

For the choice to be sound it must be unambiguous. This is checked where the
`as` is written: no two members of the union may be read from the same kind.
`String` and `DateTime` both arrive as strings; every integer and float
type, `Duration`, and an enum arrive as numbers; every record and
`Map<String, V>` arrives as an object. A union with two members of one class
is refused, and the error names the pair. A `Json` member is refused too (it
accepts every kind, so no other member could ever be chosen), as is a member
that is an optional or itself a union. A hand-written `sealed` type is not a
parse target at all: a document names no type to build, and §2.4 forbids
constructing the base.

JSON `null` chooses no member: a union field that may be null is a `U?`
field, as with any other type. A value of a kind no member takes is a
catchable parse failure at the value's path, naming the kinds the union does
take (`expected a string or a number at [0].price`).

### 6.3.1 `Json`: a value of unknown shape

`json.parse(s)` written without `as` reads a document into a `Json`: one
JSON value of whatever shape the text had. It is the type to use when the
shape is not known in advance, such as a payload from a service, a config
file with sections this program does not own, or a document being passed
through.

`Json` is a built-in type name like `String` and `DateTime`, in scope
everywhere; only the module's functions need `import 'json'`.

The seven shapes are the members of `json.Type`, which `json.getType`
answers with:

```
MISSING  NULL  BOOL  NUMBER  STRING  ARRAY  OBJECT
```

`MISSING` is distinct from `NULL` because JSON itself distinguishes a key
that is absent from one present and null, and a document being round-tripped
must not conflate them. It is also the zero value of a `Json`, and what
indexing yields for a key that is not there.

Indexing navigates. A `String` key reaches into an object and an `int` into
an array; the result is always a `Json`, never an optional:

```
import 'json';

Json? doc = json.parse(text) catch e { print(e.message); };
if (doc != null) {
    print(json.asText(doc["name"]) catch "?");
    print(json.asInt(doc["tags"][0]) catch 0);
    print(json.getType(doc["a"]["b"]["c"]) == json.Type.MISSING);  // no trap
}
```

A missing link yields `MISSING` instead of failing. This lets a chain be
written without a guard at every step. Guards would otherwise be the common
case, since the shape is what the program is trying to find out. Note this
is a third indexing rule (§3.1): an array index yields `T` and traps out of
range, a map lookup yields `V?`, and a `Json` yields a `Json`.

Reading a value out goes through `json.asInt`, `asFloat`, `asText`, or
`asBool`. Each is fallible: asking a string-valued key for its number is a
failure the caller can act on, so it is catchable rather than fatal.

A `Json` is writable. Assignment inserts a key that was not there:

```
doc["port"] = 8081;                   // any value with a JSON form (above)
doc["when"] = time.now();             // a DateTime becomes its ISO string
doc["tags"] = ["a", "b"];
doc["nothing"] = null;                // JSON null: the key is present
json.remove(doc, "stale");            // this is what removes a key
json.push(doc["list"], 4);            // arrays grow through push
```

The values that may be written are the values that have a JSON form. This is
the same set `json.toText` accepts, so a function value or a map with
non-`String` keys is the same compile error in both places.

Two rules govern what a write does:

- **A read aliases into the tree.** `Json inner = doc["tls"];` is a view, so
  `inner["on"] = true;` changes `doc`. This is what makes navigation useful.
- **A write copies into it.** `doc["a"] = other;` stores a deep copy, so the
  two cannot afterwards be changed through one another, and `doc["self"] =
  doc` is a finite document rather than a cycle.

There is no auto-vivification. Writing through a `MISSING` link, or into a
value that is not the shape the write assumed, is a runtime error (§5.5). It
does not create a subtree out of a typo. Build the container first:

```
doc["tls"] = {};                      // an empty object; [] for an array
doc["tls"]["enabled"] = true;
```

`doc[k]++` is a compile error for the same reason `m[k]++` is (§2.8), and one
more: the value may not be a number. Read it out and store the result back.

`Json` composes. It is an ordinary element type, so a heterogeneous array or
an object of mixed values needs no feature of its own:

```
type Log { int id; Json[] events; Map<String, Json> meta; }
```

`as T` on a `Json` (§3.6) reads one back into a declared type. This handles
the tagged-union shape without a round trip through text:

```
forEach(items, it) {
    if (json.asText(it["type"]) catch "" == "circle") {
        Circle? c = it as Circle catch e { print(e.message); };
    }
}
```

Numbers keep their exactness. A number the document wrote as an integer is
held as one, so a value beyond a `double`'s exact range survives a round
trip rather than being rounded on the way in.

### 6.4 `time`

A `DateTime` is an instant on the universal timeline: UTC, millisecond
precision, carrying no zone of its own. A `Duration` is a signed length of
time in milliseconds, anchored to nothing. Both compare with `==`, `!=`,
`<`, `<=`, `>`, `>=`, and §3.3 gives the arithmetic relating them.

The module has two levels. Its own functions deal in instants and spans as
such. Everything about the calendar date an instant falls on sits under
`time.date`. That is a group of functions, not a value, and naming it on its
own is a compile error.

| Function | Signature | Description |
|---|---|---|
| `time.now()` | `→ DateTime` | The current instant. |
| `time.duration(s)` | `String → Duration` | Parses `"<count><unit>"`, unit one of `s`, `m`, `h`, `d`. The count is decimal digits with an optional sign; anything else is a runtime error. |
| `time.milliseconds(d)` | `Duration → int` | The count of milliseconds the duration is. |
| `time.sleep(d)` | `Duration → Future<void>` | A future that completes `d` from now (§6.8). |
| `time.date.fromText(s)` | `String → DateTime` | Parses `"YYYY-MM-DD"` (midnight UTC) or `"YYYY-MM-DDTHH:MM:SSZ"`. Anything else is a runtime error. |
| `time.date.toText(d)` | `DateTime → String` | Formats as `"YYYY-MM-DDTHH:MM:SSZ"`. |
| `time.date.year(d)` | `DateTime → int` | Calendar year (UTC). |
| `time.date.month(d)` | `DateTime → int` | Month, 1 to 12. |
| `time.date.day(d)` | `DateTime → int` | Day of month, 1 to 31. |
| `time.date.addDays(d, n)` | `DateTime, int → DateTime` | Adds exactly `n × 24` hours (`n` may be negative). |

```
import 'time';
DateTime d = time.date.fromText("2026-07-25");
time.date.year(d)                             // 2026
time.date.toText(time.date.addDays(d, 30))    // "2026-08-24T00:00:00Z"
```

A duration is its count of milliseconds: `time.duration("1s")` is 1000,
prints as `1000`, and serializes to JSON as `1000`:

```
import 'time';
time.duration("1s")                           // 1000
time.duration("60s") == time.duration("1m")   // true
time.duration("1d") > time.duration("23h")    // true
time.milliseconds(time.duration("2h"))        // 7200000
```

`"d"` is exactly 24 hours. A civil day is not always that long, but the
ambiguity needs a calendar and a time zone to arise, and a `Duration` is
anchored to neither.

Neither type carries a time zone. An instant does not need one. A zone
matters for rendering an instant, and for civil arithmetic across a
daylight-saving boundary, and a value does not have to store one for either.
This is what keeps a `DateTime` one 8-byte scalar.

### 6.5 `array`

Functions over arrays. `push` and `pop` change the array's length, so they
require a growable array (`T[]`, §2.2). A fixed-size array's length is part
of its type, and calling them on one is a compile error. Everything else
accepts either flavor: `sort` reorders, `indexOf` looks, `slice` and `copy`
read, and none of them makes an array longer or shorter.

`push`, `pop` and `sort` mutate the array in place, so every reference to it
sees the change. `copy` and `slice` return a new array, always a growable
one, which is also the conversion from a fixed-size array to a growable one.

| Function | Signature | Description |
|---|---|---|
| `array.push(a, v)` | `T[], T →` | Appends `v`; `a.length` grows by 1. |
| `array.pop(a)` | `T[] → T` | Removes and returns the last element. Popping an empty array is a runtime error. |
| `array.copy(a)` | `T[] or T[N] → T[]` | A new growable array with the same elements. The copy is shallow: record elements are shared, not duplicated. |
| `array.slice(a, start, end)` | `T[] or T[N], int, int → T[]` | A new growable array of the elements in `[start, end)`. |
| `array.slice(a, start)` | `T[] or T[N], int → T[]` | The same, to the end of the array. |
| `array.indexOf(a, v)` | `T[] or T[N], T → int` | The first index holding a value equal to `v`, or `-1`. |
| `array.sort(a)` | `T[] or T[N] →` | Sorts in place, ascending, by `T`'s own order. |
| `array.sort(a, cmp)` | `T[] or T[N], Function(T, T)<bool> →` | Sorts in place, `cmp(x, y)` meaning "`x` belongs before `y`". |
| `array.map(a, f)` | `T[] or T[N], Function(T)<U> → U[]` | A new array of `f(v)` for each element, in order. |
| `array.filter(a, keep)` | `T[] or T[N], Function(T)<bool> → T[]` | A new array of the elements `keep` answers `true` for. |
| `array.reduce(a, start, f)` | `T[] or T[N], U, Function(U, T)<U> → U` | `f(f(start, a[0]), a[1])…`; `start` for an empty array. |
| `array.find(a, test)` | `T[] or T[N], Function(T)<bool> → T?` | The first element `test` answers `true` for, or null. |
| `array.some(a, test)` | `T[] or T[N], Function(T)<bool> → bool` | Whether `test` is true for some element; false on an empty array. |
| `array.every(a, test)` | `T[] or T[N], Function(T)<bool> → bool` | Whether `test` is true for every element; true on an empty array. |
| `array.contains(a, v)` | `T[] or T[N], T → bool` | Whether some element equals `v`, by `==`'s rule. |
| `array.reverse(a)` | `T[] or T[N] →` | Reverses in place. |
| `array.fill(a, v)` | `T[] or T[N], T →` | Sets every element to `v`, in place. |
| `array.concat(a, b)` | `T[] or T[N], T[] or T[N] → T[]` | A new array of `a`'s elements followed by `b`'s. |
| `array.flatten(a)` | `T[][] → T[]` | A new array of every inner array's elements, in order; one level. |

```
import 'array';
int[] xs = [1, 2];
array.push(xs, 3);            // xs is [1, 2, 3]
int last = array.pop(xs);     // 3; xs is [1, 2] again

int[5] fixed = [1, 2, 3, 4, 5];
int[] loose = array.copy(fixed);   // growable copy of a fixed-size array
```

The six functions that take a function value (`map`, `filter`, `reduce`,
`find`, `some`, `every`) call it once per element, in order, and stop early
where the answer is decided (`find`, `some`, `every`). The value must not be
fallible (the loop has no way to carry a raise out, as with `sort`'s
comparator). It must not change the array under the loop either, which is a
defect here as it is under `sort`. `map` is the one whose result type no
signature can name: it is an array of whatever the function answers, so the
function is written with its types (`int (int n) -> n * 2`) and may not
answer void. `reduce` takes the type of its starting value as the type of
the result and of the function's first parameter, so `array.reduce(nums, "",
…)` folds into a `String`. `contains` follows `==` and `indexOf`: the
element type must be one `==` accepts. `reverse` and `fill` work in place
and keep the length, which makes `fill` the way to reset a fixed-size array.
`concat` and `flatten` answer new growable arrays and change neither
argument, and `flatten` removes one level only. None of the eleven is
generic in the language's sense. Like `push` and `sort`, each is a function
the compiler knows and types per call.

`array.slice` copies out a range. `start` and `end` follow exactly the
convention `string.substring` uses (§6.6): `end` is exclusive, the
two-argument form runs to the end, and an index outside
`0 ≤ start ≤ end ≤ a.length` is a runtime error rather than a clamp. The
indices are the caller's own arithmetic, so one out of range is a mistake in
the program, as with indexing. Both forms may produce an empty array, and
the result never shares storage with `a`.

`array.indexOf` finds the first equal element, or reports `-1`. "Equal"
means what `==` means (§3.2): a `String` matches by content, an optional
through its box. So the element types it accepts are the ones `==` accepts.
Records and arrays are not among them, and for the same reason they are not
comparable at all: comparing by reference, a search could only find the
value the caller already had.

`array.sort` sorts in place, ascending, and is **stable**: elements that
compare equal keep the order they were in. A caller cannot restore that
property afterwards, and it is what makes sorting by two keys work as two
passes: sort by the minor key, then by the major one.

Without a comparator the order is `T`'s own, which means `T` must be one of
the types `<` is defined on (§3.1): the integer and float types, `String`,
`DateTime`, `Duration`, and enums, which order by their members' numbers.
Anything else (a record, an array, an optional, a `bool`) is a compile error
pointing at the second form. Such a type does not lack an order by
oversight; only the program knows what the order should be. Among floats,
`NaN` precedes every number, because leaving it where it fell would mean the
result was not sorted at all.

```
import 'array';

int[] ns = [5, 3, 9, 1];
array.sort(ns);                                       // [1, 3, 5, 9]

String[] ws = ["pear", "apple", "Fig"];
array.sort(ws);                                       // ["Fig", "apple", "pear"]
```

With a comparator the order is whatever `cmp` says, and `T` is
unconstrained. This is what makes sorting records possible:

```
import 'array';

type Person { String name; int age; }
Person[] people = [{ name: "cara", age: 31 }, { name: "abe", age: 44 }];
array.sort(people, bool (Person x, Person y) -> x.age < y.age);

int[] down = [5, 3, 9, 1];
array.sort(down, bool (int x, int y) -> x > y);        // [9, 5, 3, 1]
```

`cmp` answers whether `x` belongs before `y`. Answering `false` both ways
means the two are equal, which is where stability applies. It may be a
closure, and it may allocate. There are two things it may not do. It may not
be fallible, which is a compile error (a sort has nowhere to put an `Error`
raised inside it; catch it in the comparator). And it may not `push` or
`pop` the array it is sorting, which is a runtime error: the sort walks
indices it read before the call, so the result would describe neither the
old array nor the new one.

### 6.6 `string`

Functions over strings. Strings are immutable (§2.1), so every function
leaves its arguments untouched and returns its result as a new string. There
is no in-place variant of anything. Positions and lengths count bytes: a
multi-byte UTF-8 character is as many elements of `toByteArray`, and as much
of `length` and `substring`, as it has bytes.

Reading a single byte needs no function here: `s[i]` is that byte (§2.1),
and it allocates nothing. `toByteArray` is for when a program wants the
bytes as a `byte[]` it can hand around or rebuild a string from. It copies
all of them (§5.7 makes the copy byte-for-byte), so it is the wrong tool for
a scan of something already in hand as a `String`.

| Function | Signature | Description |
|---|---|---|
| `string.append(s, t)` | `String, String → String` | `s` followed by `t`, the same operation as `s + t`. |
| `string.bytesEqualConstantTime(a, b)` | `byte[], byte[] → bool` | Whether `a` and `b` hold the same bytes, in time depending only on their length. See below. |
| `string.contains(s, sub)` | `String, String → bool` | Whether `sub` occurs in `s`. The empty string occurs in every string. |
| `string.copy(s)` | `String → String` | A new string with the same bytes. |
| `string.equalsConstantTime(a, b)` | `String, String → bool` | Whether `a` and `b` hold the same bytes, in time depending only on their length. See below. |
| `string.find(s, sub)` | `String, String → int` | Byte index of the first occurrence of `sub` in `s`, or `-1` when there is none. An empty `sub` is found at 0. |
| `string.from(v)` | `printable → String` | The text `print` writes for `v` (§6.1): any string, number, `bool`, `DateTime`, `Duration`, enum, or optional of one. |
| `string.fromByteArray(bytes)` | `byte[] → String` | A string of those bytes; the inverse of `toByteArray`. |
| `string.join(parts, sep)` | `String[], String → String` | The elements of `parts` in order with `sep` between each adjacent pair; the inverse of `split`. |
| `string.length(s)` | `String → int` | The length of `s` in bytes. |
| `string.replace(s, old, new)` | `String, String, String → String` | `s` with the first occurrence of `old` replaced by `new`. |
| `string.replaceAll(s, old, new)` | `String, String, String → String` | `s` with every occurrence of `old` replaced by `new`, scanning left to right past each replacement (the replacements themselves are never rescanned). |
| `string.split(s, sep)` | `String, String → String[]` | The pieces of `s` between occurrences of `sep`, separators omitted. An empty `sep` is a runtime error. |
| `string.substring(s, start, end)` | `String, int, int → String` | The bytes `start` (inclusive) through `end` (exclusive). Bounds outside `0 ≤ start ≤ end ≤ length` are a runtime error. |
| `string.toByteArray(s)` | `String → byte[]` | The bytes of `s`, one `byte` element each. |
| `string.toFloat(s)` | `String → float`, fallible | The float `s` spells. Text that is not a number is an `Error` (§2.9), not a runtime error. |
| `string.toInt(s)` | `String → int`, fallible | The integer `s` spells. Text that is not an integer, or one that does not fit, is an `Error` (§2.9). |
| `string.toLowerCaseAscii(s)` | `String → String` | `s` with ASCII `A` to `Z` mapped to `a` to `z` and every other byte untouched, as the name says. |
| `string.toUint(s)` | `String → uint`, fallible | The unsigned integer `s` spells. Text that is not an integer, and one that is negative or does not fit, is an `Error` (§2.9). |
| `string.toUpperCaseAscii(s)` | `String → String` | `s` with ASCII `a` to `z` mapped to `A` to `Z`, every other byte untouched. Full Unicode case mapping is a table that belongs in a package. |
| `string.trim(s)` | `String → String` | `s` without leading and trailing whitespace (space, `\t`, `\n`, `\r`, vertical tab, form feed). |

The four functions below are all of the module's Unicode support, and the
set is kept small. A string is bytes, UTF-8 is how code points are laid out
in them, and nothing here knows what a code point means: no case tables, no
normalization, no width. `forEach` over a string (§4.7) is how a program
walks code points; these functions answer the questions a walk does not.

| Function | Signature | Description |
|---|---|---|
| `string.runeCount(s)` | `String → int` | How many code points `forEach` would visit: each valid UTF-8 sequence is one, and each byte outside one is one. `string.runeCount("café")` is 4 where `length` is 5. |
| `string.runeAt(s, i)` | `String, int → int` | The code point that starts at byte offset `i`, which is what `forEach` binds there; `0xFFFD` for a byte that begins no valid sequence. `i` outside `0 ≤ i < length` is a runtime error. It is a byte offset so that it costs nothing: the code point at a code-point index needs a walk, and is written as one. |
| `string.fromRunes(rs)` | `int[] → String` | The UTF-8 for each code point in turn. A value that is not a code point (negative, above `0x10FFFF`, or a surrogate) is written as `0xFFFD`, so the result is always valid UTF-8. |
| `string.isValidUtf8(s)` | `String → bool` | Whether every byte of `s` belongs to a valid sequence: no overlong forms, no surrogates, nothing above `0x10FFFF`, no truncated tail. `substring` can produce a string this refuses by cutting a character in half. |

```
import 'string';

print(string.trim("  hi  "));                    // hi
print(string.toUpperCaseAscii("nio"));           // NIO
print(string.find("my findValue", "findValue")); // 3
print(string.replaceAll("a-b-c", "-", "+"));     // a+b+c
print(string.substring("hello world", 6, 11));   // world
String[] parts = string.split("hello - world", "-");
// parts is ["hello ", " world"]
print(string.join(parts, "-"));                  // hello - world — split undone
print(string.join(["a", "b", "c"], ", "));       // a, b, c

int n = string.toInt("42") catch 0;                // 42
uint u = string.toUint("42") catch 0;              // 42
float f = string.toFloat("2.5e2") catch 0.0;       // 250
string.toInt("12x") catch e {
    print(e.message);          // string.toInt "12x": not an integer
}
string.toUint("-1") catch e {
    print(e.message);          // string.toUint "-1": out of range
}

print(string.from(42) + "!");                    // 42!
print(string.from(0.1));                         // 0.1
print(string.toFloat(string.from(f)) catch 0.0); // 250 — the same float back
```

**The two constant-time comparisons.** `==` on strings stops at the first
differing byte. That is the right implementation of equality and the wrong
one for a secret: the time it takes shows where the two differ, so an
attacker holding an oracle can recover a MAC or a session token one byte at
a time. `string.equalsConstantTime` and `string.bytesEqualConstantTime`
answer the same question by combining every byte and testing once at the
end, with no early exit and no branch inside the loop.

```
import 'string';

// A token check that leaks nothing about where a wrong guess went wrong.
if (string.equalsConstantTime(presented, expected)) {
    print("ok");
}
```

The length is not hidden: two values of different lengths are unequal
without reading a byte of either. This is intended, because the length of a
MAC or a token is a public constant. A caller whose length is the secret
needs padding, which is a decision about the protocol.

The remaining caveats belong to the machine, and no library can discharge
them: cache and branch-predictor state, and the optimizer's freedom to
rewrite. Both functions are allocation-free, so a `noalloc` body (§2.12) may
call them.

`string.join` is how a string is built out of many pieces. Strings are
immutable, so `s = s + piece` in a loop copies everything accumulated so far
on every round: building *n* pieces that way costs time quadratic in the
result's length, and leaves *n* dead intermediates behind for the collector.
`join` sums the lengths first and copies each byte once, into one
allocation. Collect the pieces in a `String[]` and join at the end. A chain
written on one line needs no such care: `a + b + c + d` is one
concatenation, not three. It allocates once and copies each operand once,
so it costs what the equivalent `join` costs. `join` is for the pieces a
chain cannot spell out: pieces accumulated in a loop, or however many there
turn out to be.

```
import 'string';
import 'array';

String[] lines = [];
for (int i = 0; i < 3; i++) {
    array.push(lines, "line " + string.from(i));
}
print(string.join(lines, "\n"));   // one pass, one allocation
```

`string.from` is the way out of a number, as `toInt` and `toFloat` are the
way in. It is defined by printing (§6.1): `string.from(v)` is the text
`printInline(v)` writes, and it takes exactly what `printInline` takes. That
is a string, a number, a `bool`, a `DateTime`, a `Duration`, an enum member,
or an optional of one, where an absent optional gives `"null"`. Records,
arrays and maps are not among them; `json.toText` (§6.3) serializes those,
and the error says so. Formatting cannot fail, so `from` is total and there
is nothing to catch.

Floats print in the shortest form that reads back as the same value at their
own width (§6.1), so `string.toFloat(string.from(f))` is `f` exactly, and
`string.toInt(string.from(n))` is `n`. The only text that does not survive
the trip is a non-finite float: `from` writes `inf` or `nan`, as
`printInline` does, and `toFloat` accepts neither. (`json.toText` refuses
such a value outright, because JSON has no syntax for it. `from` has no such
constraint, and a total function is worth more here than a guarantee about
values a program rarely formats.)

`string.toInt`, `string.toUint` and `string.toFloat` are fallible (§2.9).
They are the only three functions in the module that are. Text that does
not spell a number is the expected case for a parser, not a bug in the
caller. So it is an `Error` a `catch` can handle (or a default can answer:
`string.toInt(s) catch 0`), and an uncaught failure propagates like any
other. `toInt` accepts an optional sign followed by decimal digits and
nothing else: no whitespace (`trim` first) and no separators. A value
outside the 64-bit range is an error, not a wrap. `toUint` is the same
parser in the unsigned family. It exists because `toInt` cannot stand in
for it: an `int` does not assign to a `uint` (§2.1), and the top half of a
`uint64` has no `int` to come back as. It accepts an optional `+` and
decimal digits. A well-formed negative is out of range rather than badly
shaped, since `-1` is a valid integer that this type does not have.
`toFloat` accepts sign, digits with an optional `.` fraction (either side of
the dot may be empty, not both), and an optional `e`/`E` exponent. A value
too large for a float is an error, while one too small to distinguish from
zero rounds to zero. `substring`, by contrast, panics on bad bounds as array
indexing does (§5.5): the indices are the caller's own arithmetic.

Notes:

- `replace` and `replaceAll` with an `old` that is empty or does not occur
  return `s` unchanged.
- `split` keeps empty pieces: a leading, trailing, or doubled separator
  contributes an empty string (`string.split("a--b", "-")` is
  `["a", "", "b"]`), and splitting never drops information:
  `string.join(string.split(s, sep), sep)` is `s` for any non-empty `sep`.
- `join` puts `sep` between pieces, so it appears one time fewer than there
  are elements: an empty array joins to `""` and a one-element array to that
  element, neither of them touching `sep`. Unlike `split`, an empty `sep` is
  fine: it concatenates.
- `byte` is a `uint8` (§2.1), so `toByteArray` reports every byte as the
  number it is, 0…255, including the bytes above 127 that make up a
  multi-byte UTF-8 character. `fromByteArray` takes them back the same way,
  so the two round-trip any string, and reading a file's bytes as text
  (§6.11) is its main use. It reads only the low byte of each element, which
  for a `byte` is the whole value.
- A string is a counted byte string, so a `0` element is an ordinary byte in
  the result: `fromByteArray` neither stops at it nor drops it, and
  `string.length(string.fromByteArray([97, 0, 98]))` is 3. The escape `\0`
  (§1.6) also writes a NUL. `fs` refuses a path that contains one (§6.11).
- Case mapping is ASCII-only; bytes outside `a` to `z` and `A` to `Z` pass
  through unchanged.

### 6.7 `map`

Functions over maps (§2.8). Like the array module they are generic: the
map argument fixes `Map<K, V>`, and the key argument and result types
follow from it. The map type itself (declarations, literals, indexing,
`.length`) is part of the language and needs no import; only these
functions do.

| Function | Signature | Description |
|---|---|---|
| `map.copy(m)` | `Map<K, V> → Map<K, V>` | A new map with the same entries, in the same order. The copy is shallow: record values are shared, not duplicated. |
| `map.has(m, k)` | `Map<K, V>, K → bool` | Whether `k` is present. |
| `map.keys(m)` | `Map<K, V> → K[]` | The keys, in insertion order, as a new growable array. |
| `map.remove(m, k)` | `Map<K, V>, K → bool` | Removes `k`'s entry, in place, through every reference; whether it was present. |
| `map.values(m)` | `Map<K, V> → V[]` | The values, in the same order as `map.keys`. |

```
import 'map';

Map<String, int> ages = { "alice": 31, "bob": 27 };
print(map.has(ages, "bob"));     // true
map.remove(ages, "bob");
forEach(map.keys(ages), name) {
    print(name);                 // alice
}
Map<String, int> dup = map.copy(ages);
```

Notes:

- `has` and `remove` check the key like an assignment would: the key
  argument must be assignable to `K`.
- `keys` and `values` return snapshots: changing the map afterwards does
  not change an array already returned, and writing into the array never
  writes back into the map.
- `remove` preserves the order of the remaining entries, at O(n) in the
  map's size; lookup, insert, and overwrite are O(1) on average.

### 6.8 `async`

Callback-style consumption of futures (§2.7); the futures themselves come
from calling async functions (§5.2) and need no import.

| Function | Signature | Description |
|---|---|---|
| `async.run(f, cb)` | `Future<T>, Function(T)<void> →` | Runs `cb` with `f`'s result when `f` completes. |
| `async.race(f, …)` | `Future<any>, … → Future<int>` | The index of the first argument to complete. |

The callback's parameter matches the future's element type; for a
`Future<void>` it takes none (`Function()<void>`). If `f` is already done,
`cb` runs immediately, inside the `async.run` call. Otherwise it runs when
`f` completes, whether `f` is awaited or reached by the end-of-program drain
(§5.2). Callbacks on one future run in registration order, and a callback
may itself await, register callbacks, or start new async work.

`async.run` refuses a fallible future (`Future<T!>`): a callback is handed a
result and there would be nowhere for a failure to go. Await one instead,
where `catch` applies (§2.9).

#### `async.race`

`async.race` takes one or more futures and answers a `Future<int>`: the
position of whichever finishes first. It lets a program wait for two things
at once:

```
import 'async';
import 'net';
import 'time';

Future<byte[]!> reply = net.read(sock, 4096);
Future<void> giveUp = time.sleep(5000);
if (await async.race(reply, giveUp) == 0) {
    byte[] b = await reply catch e { return; };
    // ...
}
```

It answers an index rather than a result because the arguments need not
share a type (racing a read against a timer is the main use), and
"whichever finished" has no type an answer could be given in. Awaiting the
winner afterwards costs nothing: awaiting a future that is already done never
suspends.

The arguments may be fallible, unlike `async.run`'s: a race hands back an
index and leaves every future exactly as it was, so an error is still waiting
at the await that collects it.

The losers keep running. There is nothing general to cancel, and §2.9 still
applies to a fallible future nobody collects. So a program that abandons one
must still await it. The example above does not have to, but only because
`time.sleep` cannot fail. An operation that must stop when it loses takes a
timeout of its own instead; every `net` call does (§6.15).

An argument that has already completed settles the race before any of the
others is consulted, and ties go to the earliest argument. Racing zero
futures is a compile error, and so is racing anything that is not a future.

**`time.sleep`** (§6.4) is the scheduler's timer: awaiting its future
inside an async function suspends that task for the duration while other
tasks run. It lives in `time` rather than here, because it is about the
clock and `async` is about tasks. But it produces an ordinary `Future<void>`
and composes with `async.run` like any other. Timers measure a monotonic
clock (wall-clock adjustments cannot fire one early). A non-positive `ms`
completes on the scheduler's next free turn. Timers complete in deadline
order, and timers with equal deadlines complete in creation order. A pending
timer counts as pending work: the end-of-program drain (§5.2) waits for it,
and a program sleeping toward a deadline is never a deadlock (§5.5). A
program using `time.sleep` is deterministic in its ordering (deadlines
decide). Its interleaving with respect to real time depends on how long the
work between deadlines takes.

```
import 'async';

int async sum(int a) { return a; }

Future<int> mySum = sum(5);
async.run(mySum, void (int i) -> print(i));
print("registered");       // prints before the callback's 5:
                             // the body runs at the drain
```

### 6.9 `os`

Facts about the machine the program runs on.

| Function | Signature | Description |
|---|---|---|
| `os.getArch()` | `→ String` | The processor architecture. |
| `os.getCpus()` | `→ os.Cpu[]` | One entry per logical processor. |
| `os.getPlatform()` | `→ String` | The operating system. |

**`os.getArch`** returns one of `"arm"` (64-bit ARM), `"arm32"`, `"x64"`,
`"x86"`, or `"unknown"`. **`os.getPlatform`** returns one of `"darwin"`,
`"win32"`, `"linux"`, `"openbsd"`, `"freebsd"`, or `"unknown"`. Both are
fixed when the program is compiled, not detected as it runs: a Nio binary is
built for the machine that compiles it (§5.6), so the two cannot disagree
with the code that was generated.

**`os.Cpu`** is the record `os.getCpus` returns, exported by the module and
named through the alias in type positions (`os.Cpu c`, `os.Cpu[]`):

| Field | Type | Description |
|---|---|---|
| `model` | `String?` | The processor's model name, or null if unreported. |
| `speed` | `int?` | Its clock speed in MHz, or null if unreported. |

Both fields are optional because not every system reports both. An Apple
Silicon Mac publishes no clock speed, and an ARM Linux kernel publishes no
model name. Null says the value is unavailable rather than standing in for
it. `getCpus` always returns at least one entry, and every entry reports the
same model and speed. No platform breaks its answer down per core, so a
machine with mixed core types is not described core by core. `length`
(§6.20) is therefore how many logical processors there are.

```
import 'os';

print(os.getPlatform());        // darwin
print(os.getArch());            // arm

os.Cpu[] cpus = os.getCpus();
print(cpus.length);             // 10
forEach(cpus, c) {
    if (c.speed != null) {
        print(c.model, c.speed);
    } else {
        print(c.model);         // Apple M4
    }
}
```

### 6.10 `path`

File-system paths as text. The separator is the host's: `\` on Windows, `/`
everywhere else.

| Function | Signature | Description |
|---|---|---|
| `path.getCwd()` | `→ String` | The current working directory, as an absolute path. |
| `path.join(first, more…)` | `String, String… → String` | The elements joined by the host separator and cleaned. |
| `path.localize(p)` | `String → String` | `p` with every separator rewritten as the host's. |
| `path.userData(app)` | `String → String` | Where the platform says an application named `app` keeps its data. |

**`path.join`** joins one element or more (`join()` with nothing to join is
a compile error) and then cleans the result. Separators collapse, `.`
elements disappear, and a `..` element removes the one before it, so
`join(cwd, "../", "file.txt")` names a file beside the working directory
rather than under it. An empty element contributes nothing. The result never
ends in a separator unless it is the root itself, and an empty result is
`"."`, the path meaning "here".

```
import 'path';

print(path.join("a", "b", "c"));               // a/b/c
print(path.join("/usr/local", "../bin", "n")); // /usr/bin/n
print(path.join("a/b/c", "../../d"));          // a/d
print(path.join("a", "", "b"));                // a/b
print(path.join("a//b///c/"));                 // a/b/c
print(path.join("x", ".."));                   // .
```

A `..` with nothing left to remove is kept in a relative path
(`join("a", "..", "..")` is `".."`) and dropped in an absolute one
(`join("/a", "..", "..")` is `"/"`), because the root is its own parent. An
element that is itself absolute is joined as an ordinary element, not treated
as a fresh start: `join("a", "/b")` is `"a/b"`.

**`path.localize`** rewrites separators and does nothing else. It does not
collapse, resolve, or clean, so `localize("a//b")` keeps both separators and
`localize("../up")` keeps the `..`. It turns a path written in source with
forward slashes into one the host spells natively:

```
import 'path';

print(path.localize("./thisdir/file"));    // ./thisdir/file  (\ on Windows)
```

**`path.getCwd`** asks the operating system where the program is running.
`join` and `localize` are purely lexical: they read only the text they are
given and never touch the file system, so they answer the same way whether or
not the path exists. If the working directory cannot be read at all, `getCwd`
is a runtime error (§5.5).

**`path.userData`** answers the platform's convention for where an
application keeps its data, with the application's name as the last element:
`$HOME/Library/Application Support/<app>` on macOS, `%APPDATA%\<app>` on
Windows, and `$XDG_DATA_HOME/<app>` (falling back to
`$HOME/.local/share/<app>`) everywhere else. It answers the path only:
nothing is created, and `fs.createDir` (§6.11) is how the directory comes to
exist. It is the module's one fallible function (§2.9), because the base
location comes from the environment, which is entitled not to have it set.
That case raises `path.ErrorCode.NOT_FOUND` naming the variable. The app name
must be a single path element. A name that is empty or contains a separator
raises `path.ErrorCode.INVALID`, because a name with a separator in it would
silently answer a deeper path than the caller meant to name.

```
import 'path';
import 'fs';

String dir = path.userData("myapp") catch e { … };
// /Users/me/Library/Application Support/myapp   (this host's answer)
if (!fs.exists(dir)) {
    fs.createDir(dir);
}
```

Both `/` and `\` count as separators on every platform, so a path written
either way joins and localizes correctly. The cost is that a POSIX file name
that contains a backslash cannot be expressed through this module. Build
such a path with `+` instead.

Nothing here resolves symlinks, checks existence, or reads directories. A
path is text until something else opens it, and `fs` (§6.11) is that
something else.

### 6.11 `fs`

Reading and writing files and directories. Paths are text, as `path` (§6.10)
builds them; relative paths resolve against the working directory
(`path.getCwd()`).

| Function | Signature | Description |
|---|---|---|
| `fs.copy(from, to)` | `String, String → void` | Copies file `from`'s bytes to `to`, creating or replacing it. |
| `fs.createDir(p)` | `String → void` | Creates the directory `p`, whose parent must already exist. |
| `fs.createTempDir(prefix)` | `String → String` | Creates a directory nothing else has, under the system's temporary one, and reports its path. |
| `fs.delete(p)` | `String → void` | Removes the file, or the empty directory, at `p`. |
| `fs.delete(p, options)` | `String, fs.DeleteOptions → void` | The same, with `{ recursive: true }` removing a directory's contents too. |
| `fs.exists(p)` | `String → bool` | Whether anything is at `p`. |
| `fs.readDir(p)` | `String → String[]` | The names of the entries in directory `p`. |
| `fs.readFile(p)` | `String → byte[]` | The contents of file `p`. |
| `fs.rename(from, to)` | `String, String → void` | Moves the entry at `from` to `to`, replacing anything there. |
| `fs.stat(p)` | `String → fs.Stat` | What the system reports about `p`. |
| `fs.writeFile(p, content)` | `String, byte[] → void` | Writes `content` to `p`, creating or replacing it. |

Every function here but `fs.exists` can fail (§2.9). A missing file, a
directory the program may not read, a full disk: none of these is a mistake in
the program, so each is an `Error` the caller can catch rather than a runtime
error that stops it. `fs` is the library's most thoroughly fallible module.
Only the `string` parsers (§6.6), `process`'s stdin reads (§6.12), and
`process.child.run` at its await share the trait. A call therefore needs a
`catch`, or a caller willing to be fallible itself:

```
import 'fs';
import 'string';

byte[] bytes = fs.readFile("notes.txt") catch [];      // a default
print(string.fromByteArray(bytes));

fs.readFile("notes.txt") catch e {
    print(e.message);    // fs.readFile "notes.txt": No such file or directory
}

// Fallibility is inferred, so slurp can fail because readFile can, and
// nothing in its declaration says so.
String slurp(String p) {
    return string.fromByteArray(fs.readFile(p));
}
print(slurp("notes.txt") catch "(unreadable)");
```

An `Error`'s message names the function, quotes the path, and gives the reason
the operating system reported, so its exact wording is the host's.

**`fs.readFile` and `fs.writeFile`** deal in bytes, not text, so a file
holding anything (an image, a binary format, UTF-8) survives the round trip
unchanged. `string.toByteArray` and `string.fromByteArray` (§6.6) are how text
crosses in either direction:

```
import 'fs';
import 'string';

fs.writeFile("greeting.txt", string.toByteArray("hello\n"));
print(string.fromByteArray(fs.readFile("greeting.txt")));    // hello
```

`writeFile` creates the file if it is not there and truncates it if it is.
There is no append. It does not create missing parent directories: writing to
`a/b/c.txt` when `a/b` does not exist fails. Writing an empty array creates an
empty file.

**`fs.readDir`** returns the entry names, not paths: no directory prefix, and
no `.` or `..`. Hidden entries are included. No order is promised (neither
platform promises one), so a program that needs a particular order must
impose it. Build a usable path with `path.join` (§6.10):

```
import 'fs';
import 'path';

String[] names = fs.readDir("src") catch [];
forEach(names, name) {
    print(path.join("src", name));
}
```

**`fs.rename`** moves one entry (file or directory) to a new name, replacing
anything already at the destination. On POSIX that replacement is atomic.
This makes write-then-rename the idiom for saving a file a crash can never
leave half-written: write the new content beside the real file, then rename
it over the top, and a reader sees either the old file or the new one, never
a mixture. (On Windows the C runtime cannot replace atomically, so there the
destination is removed and the rename retried. The final state is the same,
without the guarantee.) Renaming across file systems is whatever the
operating system says it is, usually a failure: `rename` moves a name and
does not copy bytes.

```
import 'fs';
import 'string';

// The atomic save: state.json is never half-written, whatever happens.
fs.writeFile("state.json.tmp", string.toByteArray(text));
fs.rename("state.json.tmp", "state.json");
```

**`fs.copy`** copies one file's bytes to the destination, creating or
replacing it. It has `writeFile` semantics, with the content coming from a
file rather than from memory. The copy is streamed, so a large file costs no
more memory than a small one. The source must be a file: a directory raises
`IS_DIRECTORY`, exactly as `readFile` answers one. Metadata (permissions,
times) is not copied, as `writeFile` sets none; the copy is a new file with
the same bytes. Both failures name both paths, since a two-path operation
that reports only one points at the wrong file half the time.

**`fs.createDir`** makes one directory. Its parent must already exist, so
creating `a/b/c` when `a/b` does not is a failure rather than three
directories. A program that wants a chain writes the loop, as `writeFile`
refuses to build the path it is handed. Anything already at `p`, directory or
not, is a failure too. Only the operating system can ask whether an entry is
there and create it in the same step. Treating "it was already there" as
success would let a program that met a file believe it had a directory. A
caller that does not mind asks `fs.exists` first.

New directories get the permissions the system would give any other, narrowed
by the process's umask; nothing here chooses a mode.

```
import 'fs';
import 'path';
import 'string';

String out = path.join(path.getCwd(), "out");
if (!fs.exists(out)) {
    fs.createDir(out);
}
fs.writeFile(path.join(out, "report.txt"), string.toByteArray("done\n"));
```

**`fs.createTempDir`** makes a directory nothing else has, under the system's
temporary directory, and reports where it is. It exists because it is the
library's reliable source of a unique name: a name a program composes itself,
from `random` or the clock, can still collide with one another program chose.
Here the operating system
picks the name and creates the directory in one indivisible step, so two
programs racing for the same one cannot both be told they got it.

`prefix` begins the name, which is otherwise the system's to choose. It makes
the directory recognisable to a person looking at the temporary area. It is a
name, not a path: a separator in it is a failure rather than a directory
somewhere else. An empty prefix is allowed and means no particular name. The
location comes from the environment (`TMPDIR` on POSIX, `TMP` then `TEMP` on
Windows), read at each call, so a program that sets it for itself is obeyed.

Nothing removes what it makes: not the end of the program, not the system
before the next restart. A program that wants it gone says so, and since a
directory in use is rarely empty, that is usually the recursive form:

```
import 'fs';
import 'path';
import 'string';

String work = fs.createTempDir("build-") catch e { … };
fs.writeFile(path.join(work, "input.c"), string.toByteArray(source));
// …
fs.delete(work, { recursive: true });
```

**`fs.delete`** removes one entry: a file, or a directory that is already
empty. A directory with anything in it is refused by the operating system and
reported like any other failure. A program does not delete a tree by accident
through a function that names a single path.

A program can ask for it explicitly. `fs.delete` takes an optional second
argument, an **`fs.DeleteOptions`** record:

| Field | Type | Description |
|---|---|---|
| `recursive` | `bool?` | When true and `p` is a directory, remove what is under it as well. |

The field is optional, and that makes the argument optional: a record literal
may leave an optional field out, so `fs.delete(p)`, `fs.delete(p, {})` and
`fs.delete(p, { recursive: null })` all mean the same thing. They use the
defaults, which give the refusal above.

```
import 'fs';

fs.delete("build") catch e {
    print(e.message);       // fs.delete "build": Directory not empty
}
fs.delete("build", { recursive: true });    // the directory and all of it
```

`recursive` says what to do about a directory's contents; everything else is
unchanged by it. A path naming a file is removed either way, and a path naming
nothing fails either way.

A recursive delete is the only operation in `fs` that follows a path further
than the caller wrote it, so two things are worth stating exactly:

- **A symlink is removed, not followed.** The link goes; what it pointed at
  does not, even when it is a directory. Deleting a tree never reaches outside
  that tree.
- **It stops at the first entry it cannot remove**, and the `Error` names that
  entry rather than the path the call was given, since a permission a program
  lacks is somewhere under the tree rather than at its root. Whatever was
  removed before that point stays removed: there is no undo, and a failed
  recursive delete leaves a partly deleted tree.

**`fs.exists`** is the only function here that cannot fail, because it has
nothing to report: a path either resolves or does not, and "does not" is the
answer. Every reason the entry cannot be reached (it is missing, a parent is
not a directory, the program may not search a parent) is `false`. A `catch`
over it is therefore a compile error, there being nothing to handle.

Note that `exists` answers about the past: between the check and whatever it
guards, another program may create or remove the file. Where that matters,
attempt the operation and catch its failure instead of asking first.

**`fs.Stat`** is the record `fs.stat` returns, exported by the module and named
through the alias in type positions (`fs.Stat s`, `fs.Stat[]`):

| Field | Type | Description |
|---|---|---|
| `name` | `String` | The final element of the path, with no directory and no trailing separator. |
| `extension` | `String` | The last `.` of `name` and what follows it (`".gz"`), or `""` when there is none. |
| `size` | `int` | Size in bytes. |
| `isDirectory` | `bool` | Whether it is a directory. |
| `isFile` | `bool` | Whether it is a regular file. |
| `permissions` | `String` | The nine permission bits as `ls` writes them: `"rw-r--r--"`. |
| `modified` | `DateTime` | When it was last modified, to the second. |

No field is optional: a `stat` that answers at all answers completely, and one
that cannot raises instead. A path that is neither a directory nor a regular
file (a device, a socket, a named pipe) reports `false` for both flags.

```
import 'fs';
import 'time';

// An optional target is what lets a catch fall through at the top level, where
// there is no enclosing function to return from.
fs.Stat? s = fs.stat("report.tar.gz") catch null;
if (s != null) {
    print(s.name);                    // report.tar.gz
    print(s.extension);               // .gz    (the last one, not ".tar.gz")
    print(s.size, s.isFile);          // 4096 true
    print(s.permissions);             // rw-r--r--
    print(time.date.toText(s.modified));   // 2026-07-27T09:31:04Z
}
```

`name` and `extension` are read from the path text, so they follow the same
rules `path` does: a trailing separator does not cost a directory its name
(`stat("dir/").name` is `"dir"`), a leading dot is a hidden name rather than an
extension (`".gitignore"` has none), and a name ending in `.` has none either.

`permissions` is text rather than a number because Nio has no octal literals
(§1.6): a mode of `420` could not be written the way every other tool spells
it, `0644`. The bitwise operators (§3.1) can mask a number, but they do not
solve the spelling, and text is how the permissions of a file are usually
read and written anyway. On Windows only the read-only bit is real, so the
answer there is one of two values.

Notes:

- A path containing a `0` byte is refused rather than truncated at it, since
  the prefix could name a different file than the caller built.
  `string.fromByteArray` (§6.6) is the only way to make such a path.
- Nothing here follows a path recursively unless it is asked to: `readDir`
  lists one directory, `createDir` makes one, and `delete` removes one entry
  except when given `{ recursive: true }`. Recursion is otherwise a program's
  own loop.
- Symlinks are followed, as the operating system's own `stat` does, so
  `fs.stat` describes the target rather than the link.

### 6.12 `process`

The program's half of its contract with the operating system: the command
line it was started with, a chosen exit status, the three standard streams,
and starting another program. `getArgs` and `exit` are functions of the
module itself. Everything else groups its functions under a sub-name, the
way `time` groups the calendar under `time.date` (§6.4). `process.stdout`
is not a value, and `process.stdout.write(s)` is one call.

| Function | Signature | Description |
|---|---|---|
| `process.exit(code)` | `int →` | Ends the program now, with `code` as its exit status. |
| `process.getArgs()` | `→ String[]` | The command-line arguments, after the program name. |
| `process.stdout.write(s)` | `String →` | Writes `s` to standard output, exactly as given. |
| `process.stdout.flush()` | `→` | Sends everything written to standard output on to whatever it is connected to. |
| `process.stderr.write(s)` | `String →` | Writes `s` to standard error, exactly as given. |
| `process.stderr.flush()` | `→` | The same, for standard error. |
| `process.stdin.read()` | `→ byte[]`, fallible | Everything standard input has, to its end. |
| `process.stdin.readBytes(n)` | `int → byte[]`, fallible | Up to `n` bytes; fewer only when the input ended first, and empty at its end. |
| `process.stdin.readLine()` | `→ String?`, fallible | The next line, or `null` when the input has ended. |
| `process.child.run(cmd, args)` | `String, String[] → Future<process.ChildRunResult!>` | Starts `cmd` with `args`; `await` the future for its result. |
| `process.child.run(cmd, args, options)` | `String, String[], process.ChildRunOptions → Future<process.ChildRunResult!>` | The same, choosing the input, directory, environment, or streams the child is given. |

Why `child` is a group and not a bare function: every other member of this
module has this process as its subject (this program's arguments, this
program's streams, this program exits). A bare verb would inherit that
subject, and `process.run(...)` would read as running something inside this
process. POSIX `exec()` has exactly that reading: there, the current process
is the subject, and the call replaces the caller. Naming the child gives the
verb a subject other than the current process.

**`process.getArgs`** answers with the arguments the program was started
with, in order, one string each, and without the program's own name. What
most languages put in `argv[0]` is whatever the parent chose to write there.
It is not reliably a path to anything (under `nio run` it names a temporary
binary), and every real use starts at the first argument, so Nio does too. A
program started with no arguments gets an empty array. Each call returns a
fresh snapshot; writing into it changes nothing.

```
import 'process';

// $ ./prog build main.nio -o main
String[] args = process.getArgs();
print(args.length);        // 3
print(args[0]);            // build
```

**`process.exit`** ends the program the moment it is called. Nothing after
it runs, including pending async work the end-of-program drain (§5.2) would
otherwise have finished; exiting is how a program declines that. Output
already printed is flushed first. The status is what the parent process
sees. The host keeps only its low 8 bits on POSIX systems, so 0 through 255
mean what they say and 0 remains the conventional success. A program that
never calls it exits 0 on its own (§5.1), or 1 on a runtime error (§5.5).

The writes append nothing and translate nothing: `write("a")` puts one byte
on the stream, and a line needs its own `"\n"`. `print` and `printInline`
(§6.1) write to the same standard output, so the two interleave in the order
they were made. What separates the streams is the reader: standard output is
for the program's product and standard error for its commentary, and `2>` in
a shell collects one without the other. Like `print`, the writes cannot fail
(§2.9 has no channel for a failure a caller could act on); a write the
operating system refuses is silently dropped. Text is bytes here as
everywhere (§2.1), so `string.fromByteArray` (§6.6) is how raw bytes reach a
stream.

The flushes decide when what was written arrives, which is a separate
question from whether it was written. The host holds output back and sends it
on in blocks. How much it holds depends on what the stream is connected to: a
standard output going to a terminal is sent on at each newline, and one going
to a pipe or a file is held until several thousand bytes have collected.
Neither choice is observable to a program that runs and ends. The output is
sent on when the program exits, in the order it was made, so the reader sees
the same bytes either way, and `flush` is unnecessary in every program that
only produces output. It is necessary in exactly one situation, and there it
decides whether the program works or hangs: a program that writes something
and then waits for an answer to it. Under a pipe, the request is still in
this program's buffer, so the reader on the other end has nothing to answer
and both sides wait. The fix is to write the request, flush, and then read.
This is what makes `nio lsp` possible. Both streams have a flush because the
host's two rules differ: a standard error is not held in blocks, but it may
be held to the end of a line, which is the same problem for a prompt written
without one.

```
import 'process';

process.stdout.write("name? ");
process.stdout.flush();                       // or the prompt is never asked
String? answer = process.stdin.readLine() catch null;
```

The reads are fallible (§2.9), like `fs`: a stream the operating system
refuses to read is nothing the program did wrong, so it is an `Error` a
`catch` can handle. End of input is an answer, not a failure: `read` answers
it with an empty `byte[]` and `readLine` with `null`. That is why the result
of `readLine` is optional and narrows like any other (§2.3):

```
import 'process';
import 'string';

String? line = process.stdin.readLine() catch null;
while (line != null) {
    process.stdout.write("> " + line + "\n");
    line = process.stdin.readLine() catch null;
}
```

**`process.stdin.read`** is the `fs.readFile` of streams: the bytes come
back as the 0…255 values they are, and text crosses back through
`string.fromByteArray`. It consumes the input whole; after it, `readLine`
answers `null`. **`process.stdin.readLine`** returns the bytes up to the
next `"\n"`, without it. A `"\r"` before that `"\n"` is dropped too, so a
CRLF stream reads the same as an LF one. The last line of an input that
ends without a newline is still a line. A program driven interactively sees
each line as it is entered; one fed by a pipe sees the pipe's bytes.

```
import 'process';
import 'string';

byte[] input = process.stdin.read() catch [];
process.stderr.write("read " + string.from(input.length) + " bytes\n");
process.exit(input.length);
```

**`process.stdin.readBytes`** is for input whose shape is counted rather
than delimited. It answers up to `n` bytes, short only when the input ended
before `n` of them, and empty exactly at the end. That is what tells "the
stream is finished" from "the stream had less than I asked for". A negative
`n` is a bug in the caller, like a `string.substring` out of range, so it is
a runtime error (§5.5) and not an `Error`.

It exists because neither of the other two can read a framed message. A
frame states its length and then gives that many bytes with nothing between
it and the next one. So `read` (which consumes everything) is too much, and
`readLine` (which needs a `"\n"` that is not there) either swallows the
following frame's header or waits for a byte that is not coming. The
Language Server Protocol frames its messages this way, and reading one of
them looks like this:

```
import 'process';
import 'array';

// The header is delimited, so readLine suits it; the body is counted, so it
// does not. Loop, because a short answer is only an ended input.
byte[] body(int n) {
    byte[] out = [];
    while (out.length < n) {
        byte[] chunk = process.stdin.readBytes(n - out.length) catch [];
        if (chunk.length == 0) {
            return out;          // the input ended early
        }
        forEach(chunk, b) {
            array.push(out, b);
        }
    }
    return out;
}
```

**`process.child.run`** starts another program. `cmd` is found the way a
shell finds it (through `PATH`, or used as a path when it contains a
separator), and `args` become its arguments exactly as given. No shell is
involved, so nothing is quoted, split, or expanded. The result reports what
the program wrote and how it exited. Its output is collected, not shared with
the program's own streams, and by default its standard input is empty.

A shell, when one is wanted, is a program like any other:
`process.child.run("/bin/sh", ["-c", line])` asks for one explicitly. The
caller then chooses quoting and expansion instead of getting them by
surprise.

It does not wait. The call starts the child and hands back a
`Future<process.ChildRunResult!>` (§2.7), exactly as calling an async function
does; `await` is what waits:

```
process.ChildRunResult r = await process.child.run("git", ["status"]) catch e { … };
```

This is `time.sleep`'s shape (§6.4), for the same reason: waiting is the
caller's decision, and a function that always waited could not be used any
other way. As a result, children run concurrently. Every `run` whose future
has not been awaited yet is already running, and the scheduler waits on all
of them at once:

```
import 'process';
import 'array';

Future<process.ChildRunResult!>[] running = [];
forEach(["a.nio", "b.nio", "c.nio"], file) {
    array.push(running, process.child.run("nio", ["run", file])); // all three start
}
forEach(running, f) {
    process.ChildRunResult? r = await f catch null;                  // collect
}
```

No async function is needed for that: `await` outside one drives the
scheduler (§3.5), so a plain top-level loop interleaves three children just
as an async one would. Inside an async function the await is a suspension
point like any other, and other tasks run while the child does. A future
nobody awaits still runs, and the end-of-program drain waits for it (§5.2).

There is exactly one kind of failure: the program could not be started (not
found, not executable, a directory that cannot be entered, a string
containing a NUL byte). It rides on the future rather than the call, which is
why the future is `Future<…!>` and the call itself is not fallible. It
surfaces at the `await`, where `catch` applies (§3.7), and an uncaught one
propagates from there like any other error. A failure on a future nobody
awaited stops the program at the drain, as an async function's does (§2.9).
A program that ran and exited nonzero is a result, not a failure. Its exit
code is data for the caller to judge, so a call that tolerates a nonzero exit
is usable without a `catch`. A child ended by a signal reports
`128 + signal`, as shells do.

Because the future is fallible, `async.run` (§6.8) does not accept it: a
completion callback has no error channel. Await it instead.

How long a child lives: the operating system ties a child to its parent in
one sense only. This program is the only one that can wait for it, and the
only one told how it exited. The child does not end when this program does.
POSIX hands an orphan to `init` and it keeps running, and no system cascades
a kill downwards. In Nio, §5.2 prevents orphans: a future nobody awaits is
still waited for at the end of the program, so a child outstanding at that
point is finished before the program exits. There are no orphans and no
zombies. `process.exit` is the exception, as it is for all pending async
work. A child still running when it is called is left running, and whatever
it writes from then on goes nowhere.

**`process.ChildRunOptions`** is the record the third argument takes,
exported by the module and named through the alias like any other. Both of
`run`'s records are named at the module, not under `child`. A namespace
groups functions and holds no types, and the type grammar is one optional dot
(Appendix A), so `process.child.ChildRunOptions` could not be written. They
carry `Child` in the name instead, because a type is read far from any call
(in a declaration, an array element, a future's parameter), where the
namespace that would have said what is being run is not in sight. Naming
each after the call it belongs to also leaves `process.Child` free for the
live-child handle of §7.

Every field is optional, and that makes the argument itself optional: `{}`
and a call with only two arguments are the same call, and every field left
out means what the child would have inherited anyway.

| Field | Type | Description |
|---|---|---|
| `stdin` | `byte[]?` | The bytes the child reads as its standard input. Absent is an empty input. |
| `cwd` | `String?` | The directory the child starts in. Absent is the program's own. |
| `env` | `Map<String, String>?` | Environment variables, added to what the child would inherit. |
| `clearEnv` | `bool?` | When true, `env` is the child's whole environment rather than an addition. |
| `inherit` | `bool?` | When true, the child shares this program's three standard streams instead of its output being collected. |

**`stdin`** is the whole input, decided in advance. The bytes are written
before the child starts, so a child that reads all of them and a child that
reads none both work, and neither can block against a program that is not
draining it. It is `byte[]` for the reason every other stream here is
(§2.1), so text goes in through `string.toByteArray` (§6.6):

```
import 'process';
import 'string';

process.ChildRunResult r = await process.child.run("clang",
    ["-x", "c", "-", "-o", "-"],
    { stdin: string.toByteArray("void probe(void) {}\n") }
) catch e { … };
```

**`cwd`** is where the child starts, and it applies to `cmd` too. A program
named by a relative path is looked for from the directory the child will run
in, which is what a shell would do given the same two instructions. The
program's own working directory is unchanged; nothing in Nio moves it.

**`env`** adds to the environment rather than replacing it, so a child that
needs one variable set does not lose `PATH` for it. An entry whose name the
child would have inherited replaces that one rather than joining it, so
there is never more than one entry per name. `clearEnv` asks for the other
behaviour: with it, `env` is the entire environment the child gets, and
`{ clearEnv: true }` alone gives it none at all. A name containing `=` is
refused rather than passed on, since it would silently name a different
variable than the one written.

```
import 'process';

// One variable added, everything else inherited.
await process.child.run("make", ["all"], { env: { "CC": "clang" } }) catch e { … };

// Exactly one variable, and nothing else.
await process.child.run("/usr/bin/env", [],
    { env: { "TZ": "UTC" }, clearEnv: true }) catch e { … };
```

Finding `cmd` through `PATH` uses the program's own `PATH`, not the one in
`env`, because the lookup happens on this side of the start. A child whose
environment is cleared is therefore still found normally; name it by path if
that is not wanted.

**`inherit`** is the only option that changes what comes back. The other
four describe what the child is given. This one says the child's three
standard streams are not collected at all but are this program's own. The
child reads the input this program was started with and writes where this
program writes, as it happens rather than at the end. A program that runs
other programs on the user's behalf wants this: a compiler's own output
should appear while it is compiling, not after.

Nothing is captured, so `stdout` and `stderr` of the result are empty. The
exit code is what remains, and it is usually what the caller needs:

```
import 'process';

print("building…");
process.ChildRunResult r = await process.child.run("make", ["all"],
    { inherit: true }) catch e { … };   // make's output appears here, as it runs
if (r.code != 0) { process.exit(r.code); }
```

Output the program itself buffered is written out before the child starts,
so the two do not appear out of order. `stdin` given alongside `inherit` is
ignored: the child is reading this program's input, and there is nowhere for
the bytes to go. That combination is not an error, because both fields are
ordinary optionals that may be set from variables, which the compiler cannot
see. A run that failed at the moment of starting is a worse answer than a
documented one.

**`process.ChildRunResult`** is the record `run`'s future carries, exported by the
module and named through the alias in type positions (`process.ChildRunResult
r`, `process.ChildRunResult[]`):

| Field | Type | Description |
|---|---|---|
| `stdout` | `byte[]` | Everything the program wrote to its standard output. |
| `stderr` | `byte[]` | Everything it wrote to its standard error. |
| `code` | `int` | Its exit status; `128 + signal` when a signal ended it. |
| `usage` | `process.ChildUsage?` | What the operating system charged it, where the platform reports that. |

The streams are bytes, as `fs.readFile`'s result is: a program may write
anything, and text comes out through `string.fromByteArray` (§6.6).

```
import 'process';
import 'string';

process.ChildRunResult? r = await process.child.run("git", ["status", "--short"]) catch null;
if (r != null && r.code == 0) {
    process.stdout.write(string.fromByteArray(r.stdout));
}

await process.child.run("definitely-not-installed", []) catch e {
    print(e.message);   // process.child.run "definitely-not-installed": No such file or directory
}
```

**`process.ChildUsage`** is what the child cost, as the operating system
counted it. Every field is one the kernel actually maintains, so a zero is a
measurement rather than a gap:

| Field | Type | Description |
|---|---|---|
| `peakMemory` | `int` | The most resident memory it held at once, in bytes. |
| `userTime` | `Duration` | CPU time it spent running its own code. |
| `systemTime` | `Duration` | CPU time the kernel spent on its behalf. |
| `minorFaults` | `int` | Page faults served without reading from a device. |
| `majorFaults` | `int` | Page faults that needed a read. |
| `blockReads` | `int` | Block input operations. |
| `blockWrites` | `int` | Block output operations. |
| `voluntarySwitches` | `int` | Times it gave up the CPU, waiting for something. |
| `involuntarySwitches` | `int` | Times it was taken off the CPU. |

```
import 'process';

process.ChildRunResult r = await process.child.run("make", ["all"]);
process.ChildUsage? used = r.usage;
if (used != null) {
    print(used.peakMemory / 1048576, "MB peak,", used.userTime + used.systemTime, "ms of CPU");
}
```

The field is optional because not every system will report it. All three
hosts do fill it in, each with what it has. Linux and macOS use POSIX
`getrusage`. Windows uses `GetProcessTimes` and `GetProcessMemoryInfo`, which
report the peak working set, the two CPU times and a page fault count, and
have no counterpart for the other five fields; those read 0 there. An absent
`usage` means "not reported here", which is a different statement from a
field full of zeros. That is why it is an optional rather than a record of
zeros.

Two things to know about the units. `peakMemory` is bytes on every platform,
normalized: the underlying `ru_maxrss` is bytes on macOS and kibibytes
elsewhere, and a caller should not have to know which. The two CPU times are
`Duration`s, so they count milliseconds (§2.1). They are rounded, not
truncated, so a child that used half a millisecond reports one. A child that
ran for less than that reports none, which is the resolution the language's
whole time model works at.

On Linux, `peakMemory` is never less than the memory the parent held when
it started the child. The kernel charges that memory to the child, with
`fork` and with `posix_spawn` alike, and the child's own peak cannot be
separated from it afterwards. To measure a small program, start it from a
small parent.

Average memory is not reported. POSIX defines three "integral" sizes for it
(`ru_ixrss`, `ru_idrss`, `ru_isrss`), and neither Linux nor macOS has
maintained any of them for decades: both always report zero. A field that is
always zero is worse than no field, because nothing distinguishes it from a
real answer.

### 6.13 `test`

Declaring tests, asserting inside them, and reporting how they went. This
module is written in Nio rather than C (`build/stdlib/test.nio`): it needs
nothing the language does not already have, and reads as an ordinary program
using the rest of the library.

| Function | Signature | Description |
|---|---|---|
| `test.start(title, body)` | `String, Function()<void!> →` | Runs `body` now and reports it under `title`. |
| `test.startAsync(title, task)` | `String, Future<void!> →` | Registers an already-started task, reported when `run` collects it. |
| `test.assert(ok, message)` | `bool, String →`, fallible | Fails the test with `message` when `ok` is false. |
| `test.expectInt(got, want)` | `int, int →`, fallible | Fails unless `got == want`. |
| `test.expectFloat(got, want)` | `float, float →`, fallible | Fails unless `got == want` exactly. |
| `test.expectFloatNear(got, want, tolerance)` | `float, float, float →`, fallible | Fails unless `got` is within `tolerance` of `want`. |
| `test.expectString(got, want)` | `String, String →`, fallible | Fails unless the two strings are equal. |
| `test.expectBool(got, want)` | `bool, bool →`, fallible | Fails unless the two bools are equal. |
| `test.run()` | `→` | Waits for every async test, prints the summary, exits 1 if any failed. |

```
import 'test';

int add(int a, int b) { return a + b; }

test.start("adds numbers", void () -> {
    test.expectInt(add(2, 2), 4);
});

test.start("expects the wrong answer", void () -> {
    test.expectInt(add(2, 2), 5);
});

test.run();
// ok   adds numbers
// FAIL expects the wrong answer - expected 5, got 4
// 1 passed, 1 failed
```

An assertion that does not hold raises (§2.9). This is the module's whole
mechanism: `assert` and the expectations return an `Error`, which makes the
test body fallible, and `start` catches it at the closure boundary. Three
things follow from it, and they are the module's semantics:

- A test stops at its first failed assertion. The Error leaves the body the
  way any error leaves any function, and the tests around it are untouched.
- An error the test never expected is reported as a failure too, with the
  message the failing call produced. A body is fallible for whatever reason,
  and a file that would not open (§6.11) or a number that would not parse
  (§6.6) reaches `start` by the same path an assertion does. A test run does
  not stop because one test hit something it did not plan for.
- A body that cannot fail at all is still accepted where the fallible
  parameter is expected (§3.4), so a test that only prints compiles like any
  other.

Results stream out as they are known: `start` prints its line before
returning. So a run that never reaches `run` still shows what it managed to
check. `run` waits for the async tests, prints the tally, and gives the run
its exit status. That status is 1 when anything failed; otherwise `run` sets
none at all, so a successful run simply returns and the program goes on.
`run` may be called more than once; it forgets the tasks it has collected and
keeps counting.

`startAsync` takes the future, not a function that would produce one.
Calling an async function does not run its body (§5.2). It hands back a
`Future` and joins the scheduler's queue. So every test registered before
`run` has started and none has run, and they interleave at their await
points while `run` waits:

```
import 'test';

void async fetch(String name) {
    print(name + " starts");
    await settle();
    test.expectString(name, name);
}
void async settle() {}

test.startAsync("fetch A", fetch("A"));
test.startAsync("fetch B", fetch("B"));
test.run();
// A starts, B starts — both before either finishes — then:
// ok   fetch A
// ok   fetch B
// 2 passed, 0 failed
```

Interleaving is what the language means by concurrency: tasks take turns at
their awaits on one thread, and never run simultaneously (§5.2, §7). An
async test that contains no assertion at all is a `Future<void>` rather than
a `Future<void!>`, which `startAsync` does not accept (§2.7). A test that
tests nothing is the only body that shape rules out.

There is one expectation per type because a function written in Nio takes
concrete types. `array.push` is generic only because the compiler implements
it, and nothing in the language expresses that (§7). Assignability (§2.10)
does most of the work: `expectInt` takes every integer type and every enum,
which compares as its number. `assert` covers what is left, including
records, arrays, `DateTime` and `Duration`:

```
test.assert(myCar.make == "toyota", "make was " + myCar.make);
```

`expectFloat` compares exactly, which is meaningful for a value that was
written down and rarely for one that was computed. `expectFloatNear` is for
the second kind. Failure messages format values the way `print` does
(§6.1), so an enum compared through `expectInt` reports its number, not its
name.

A test program reads `--filter <text>` off its own command line and runs
only the tests whose title holds that text. The match is a substring, so
nothing needs escaping and one filter can name a family. `--filter=<text>` is
the same thing. The filter lives in this module rather than in a command, so
it works for an entry file written by hand as much as for the one `nio test`
generates:

```sh
nio run tests/all.nio --filter lexer
nio test --filter lexer
```

A filtered run counts what it left out, so a filter that matches nothing is
visible rather than silent:

```
ok   lexer: positions
36 passed, 0 failed, 250 filtered out
```

A filter cannot save time on `startAsync`. The caller starts the task before
registering it, so the filter can only stop it being reported. The task is
still collected, because otherwise an error nobody took would be reported by
the drain (§5.2) instead.

Statement coverage is a property of the compiler rather than of this module,
and works for any program. `nio run --coverage file.nio` (or
`nio build --coverage`) instruments the build, and the program writes an
LCOV report when it exits, to `coverage.lcov` or wherever
`NIO_COVERAGE_FILE` says. It also writes a summary line on standard error.

```sh
$ nio run --coverage tests/math.nio
ok   adds numbers
1 passed, 0 failed
coverage: 57.1% of lines (4/7) written to coverage.lcov
```

```
SF:src/math.nio
DA:2,1        <- line 2 ran once
DA:6,0        <- line 6 never ran
LF:4
LH:1
end_of_record
```

The report covers every module the program compiled except the standard
library's own (this one included): it measures the code the program's author
wrote. A line's count is how many times that line was reached. The report is
written from an `atexit` handler, so a run that ends in `process.exit` (which
is how `run` reports a failing suite) or in a runtime error (§5.5) still
produces one.

Instrumentation is opt-in and costs an ordinary build nothing: without the
flag none of it is emitted, and the reporter is not linked.

### 6.14 `regexp`

Matching text against a pattern. The module has three functions and one
type, `RegExp`, which is a pattern already compiled. The two matching
functions take a `RegExp`:

| Function | Signature | Description |
|---|---|---|
| `regexp.create(pattern, flags?)` | `String, String → RegExp!` | Compiles a pattern. Fails if it is not one. |
| `regexp.find(s, re)` | `String, RegExp → int` | The byte offset of the leftmost match in `s`, or −1. |
| `regexp.match(s, re)` | `String, RegExp → bool` | Whether the pattern matches anywhere in `s`. |

```
import 'regexp';

RegExp mySearchRegexp = regexp.create("ab+c", "i");
print(regexp.find("xxABBBCyy", mySearchRegexp));   // 2
print(regexp.match("xxABBBCyy", mySearchRegexp));  // true
print(regexp.match("xxacyy", mySearchRegexp));     // false
print(regexp.find("nothing here", mySearchRegexp)); // -1
```

`find` and `match` are the regular-expression forms of `string.find` and
`string.contains` (§6.6), and answer as those do: a byte offset or −1, and a
bool. They take the subject first and the pattern second, as those two do.

Compiling is separate from matching. A pattern is compiled once and matched
many times, so `create` is where the work, and the only failure, lives.
Passing a pattern as text to every call would hide a compile behind each of
them and give the failure nowhere to go.

`regexp.create` is fallible (§2.9). A pattern is text, and text that does
not spell a pattern is the same kind of failure as text that does not spell a
number: it routinely comes from a command line, a config file or a search
box, so it must be catchable rather than a runtime error.

```
import 'regexp';

RegExp? re = regexp.create("a(b") catch e {
    print(e.message);          // regexp.create "a(b": unmatched (
    print(e.code == regexp.ErrorCode.INVALID);   // true
};
```

Every failure carries the code `regexp.ErrorCode.INVALID`. This is the same
enum the other raising modules name (§2.9), and the same code `string.toInt`
reports for text that is not a number. The case is the same: text that is not
a pattern. The message says which part of the pattern was wrong.

Fallibility is inferred, so a pattern written as a literal costs nothing to
write: the call needs no syntax of its own, and a program that never catches
one stops with the message. A function that compiles a pattern becomes
fallible in turn, like any other caller of a fallible function.

#### Flags

The second argument may be left off; no flags and `""` mean the same thing.

| Flag | Name | Effect |
|---|---|---|
| `i` | case-insensitive | ASCII letters match either case. |
| `m` | multiline | `^` and `$` also match at line boundaries. |
| `s` | dot-all | `.` also matches a newline. |

Any other flag is a compile error; it is not ignored.

#### Pattern syntax

| Construct | Meaning |
|---|---|
| `abc` | the bytes themselves |
| `.` | any one byte, except a newline unless `s` |
| `^` `$` | start and end of the text, or of a line under `m` |
| `\b` `\B` | at, and not at, an ASCII word boundary |
| `x*` `x+` `x?` | zero or more, one or more, optional |
| `x{n}` `x{n,}` `x{n,m}` | exactly, at least, and between |
| `x*?` `x+?` `x??` `x{n,m}?` | the same, preferring the shorter match |
| `a\|b` | either |
| `(…)` `(?:…)` | grouping, which is all a group does here |
| `[abc]` `[^abc]` `[a-z]` | one byte in, or not in, the set |
| `\d` `\D` `\w` `\W` `\s` `\S` | digit, word byte, whitespace, and their complements |
| `\n` `\r` `\t` `\f` `\v` `\0` `\xHH` | one byte, written out |
| `\.` `\*` `\\` … | any non-alphanumeric byte, as itself |

Groups do not capture: there is nothing to read a captured group out of, so
`(…)` groups for repetition and alternation and nothing more. Lazy
quantifiers are accepted and change which match is preferred, which neither
`find` nor `match` can observe. Both answer about the leftmost match, and
where that match starts is the same either way.

An alphanumeric escape the table does not list (`\q`, `\A`, `\p`) is an
error rather than the letter itself. So a pattern using a feature this module
does not have fails where it is written instead of quietly matching the wrong
thing. These features are left out by design: captures and backreferences,
lookaround, inline flags (`(?i)`), and named classes (`[[:alpha:]]`).

#### Bytes, not characters

Patterns and subjects are byte strings, as everywhere else in the language
(§2.1, §6.6). `.` matches one byte, `[a-z]` is a byte range, and case folding
is ASCII. These are the same rules `string.length`, `string.substring` and
`string.toUpperCaseAscii` already follow. Literal text therefore matches
correctly whatever the encoding, since a byte is a byte. `.` and a class
count encoded bytes, so `.` matches one byte of a multi-byte character rather
than the character. Offsets from `find` are byte offsets, which is what
`string.substring` takes.

#### Matching is linear

The engine follows every alternative at once rather than backtracking, so a
match costs the subject's length times the pattern's size and no more.
Patterns that make a backtracking engine run for longer than the age of the
universe answer here immediately. `(a+)+b` against a string of `a`s is the
usual example:

```
import 'regexp';

RegExp evil = regexp.create("(a+)+b");
print(regexp.match("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", evil));  // false, at once
```

This follows the same rule as making `create` fallible: a module whose input
comes from outside the program must not have a failure mode where that input
decides how long the program runs.

Within that bound, what a pattern begins with decides the constant factor. A
pattern beginning with literal text, or with `\b` or `^` and then literal
text, is searched for by scanning for that text. The engine then follows the
pattern only where it could match, which costs less than following it at
every position. A pattern beginning with `.`, or one that can match the empty
string, admits every position and gets no such scan: `the .*cat` is searched
for by scanning for `the `, while `.*the cat` is followed from every position
in the subject.

A pattern that would take unreasonable space is a compile error instead: a
pattern over 32 KB, one nested more than 200 groups deep, a repeat count over
1000, or one whose counted repeats expand past 100000 instructions
(`a{1000}{1000}` is such a pattern).

#### A `RegExp` is opaque

A compiled pattern is a program, not a value with parts. It cannot be
printed, compared with `==`, serialized to JSON, or used as a map key; each
of those is a compile error naming the reason. The part worth printing is the
pattern text, which the program that wrote it still has. A `RegExp` can be
held in a variable, passed, returned, stored in a record, an array or a map,
and captured by a function value, like any other value.

A `RegExp` has no useful zero value: a variable declared without an
initializer holds nothing, and matching with it is a runtime error (§5.5), as
calling a function value that was never assigned one is.

### 6.15 `net`

Sockets, over TCP, UDP and Unix domain. Everything that could wait produces a
future (§2.7) the scheduler completes. So a program serving many connections
does so on one thread, at await points, as it interleaves any other async
work (§5.2).

```
import 'net';
```

There are two built-in types, both opaque. A `Socket` and a `Listener` stand
for a connection the operating system owns, not for a value with parts:

| Type | What it is |
|---|---|
| `Listener` | Something connections arrive on: `net.tcp.listen`, `net.unix.listen`. |
| `Socket` | Something bytes go through: an accepted or connected stream, or a bound datagram socket. |

Neither can be printed, compared with `==`, serialized to JSON, or used as a
map key; each is a compile error naming the reason. Both can be held in a
variable, passed, returned, stored in a record, an array or a map, and
captured by a function value, like any other value. Neither has a useful
zero value: one declared without an initializer holds nothing, and using it
raises `NOT_CONNECTED`.

#### Opening one

| Function | Signature | Description |
|---|---|---|
| `net.tcp.listen(addr, opts?)` | `String, net.Options → Listener!` | Binds and listens for TCP connections. |
| `net.tcp.connect(addr, opts?)` | `String, net.Options → Future<Socket!>` | Connects to a TCP address. |
| `net.udp.bind(addr)` | `String → Socket!` | Binds a datagram socket. |
| `net.unix.listen(path, opts?)` | `String, net.Options → Listener!` | Binds and listens on a Unix domain socket. |
| `net.unix.connect(path, opts?)` | `String, net.Options → Future<Socket!>` | Connects to a Unix domain socket. |

An address is `"host:port"`, or `"[host]:port"` when the host is an IPv6
literal, whose colons would otherwise be indistinguishable from the
separator. The port is always a number: a service name would make the meaning
of a string depend on the machine. An empty host (`":8080"`) means every
interface. A Unix address is a file system path.

Port 0 asks the operating system to choose, and `net.address` reports what
it chose. This is how a program takes a port without naming one.

Name resolution is synchronous: `getaddrinfo` blocks, and there is one
thread. A server never resolves; a client resolves once per connection.

#### Using one

| Function | Signature | Description |
|---|---|---|
| `net.accept(l, opts?)` | `Listener, net.Options → Future<Socket!>` | The next connection to arrive. |
| `net.read(s, n, opts?)` | `Socket, int, net.Options → Future<byte[]!>` | Up to `n` bytes; empty means the peer closed its end. |
| `net.readExactly(s, n, opts?)` | `Socket, int, net.Options → Future<byte[]!>` | Exactly `n` bytes, however many arrivals it takes; a peer that closes first fails it with `END_OF_FILE`. The timeout covers the whole read. |
| `net.write(s, data, opts?)` | `Socket, byte[], net.Options → Future<void!>` | Every byte of `data`, however many writes that takes. |
| `net.receive(s, n, opts?)` | `Socket, int, net.Options → Future<net.Datagram!>` | One datagram, with the address it came from. |
| `net.send(s, addr, data, opts?)` | `Socket, String, byte[], net.Options → Future<void!>` | One datagram to `addr`. |
| `net.close(h)` | `Socket \| Listener →` | Closes it. Total, and safe to call twice. |
| `net.address(h)` | `Socket \| Listener → String!` | The address this end is bound to. |
| `net.peer(s)` | `Socket → String!` | The address at the other end. |

`net.close` and `net.address` take either kind of handle; everything else
takes the one it is about, and passing the wrong one is a compile error.

A `Socket` is one type whichever protocol opened it, so the checker cannot
tell a stream from a datagram socket. `net.read`/`net.write` on a datagram
socket, and `net.receive`/`net.send` on a stream, raise `INVALID` with a
sentence saying which pair to use.

```
import 'net';
import 'async';
import 'string';

void async serve(Listener l) {
    while (true) {
        Socket c = await net.accept(l) catch e { return; };
        byte[] req = await net.read(c, 4096, { timeout: 30000 }) catch e {
            net.close(c);
            continue;
        };
        await net.write(c, string.toByteArray("you said: " + string.fromByteArray(req)))
            catch e { };
        net.close(c);
    }
}

// A top-level catch has no function to return from, so it falls through to
// null (§3.7).
Listener? l = net.tcp.listen("127.0.0.1:8080") catch null;
if (l != null) {
    print("listening on " + (net.address(l) catch "?"));
    async.run(serve(l), void () -> { });
}
```

#### `net.Options`

The trailing argument every function above may be given and none requires.
Each reads the fields that apply to it and ignores the rest; leaving it off
is the same as passing a record with every field unset.

| Field | Type | Applies to |
|---|---|---|
| `timeout` | `Duration?` | Everything that waits: `connect`, `accept`, `read`, `write`, `receive`, `send`. |
| `backlog` | `int?` | `tcp.listen` and `unix.listen`. How many pending connections the operating system holds; 128 by default. |
| `noDelay` | `bool?` | `tcp.connect` and `accept`, where it sets `TCP_NODELAY` on the connection that comes out. |

`timeout` is a `Duration`, so a whole-number literal means milliseconds by
rule 5 of §2.10: `{ timeout: 5000 }` is five seconds. An operation that runs
out of its timeout raises `TIMED_OUT` and is removed. Nothing is left half
done and no future is left to collect. Without a timeout, an operation waits
as long as it takes.

#### `net.Datagram`

What `net.receive` answers: `address` (`String`), the sender, in the form
`net.send` accepts back, and `data` (`byte[]`), the message. A datagram
socket hears from anyone, which is why receiving is not simply a read.

#### Ending a connection

`net.close` closes the descriptor and fails whatever was waiting on it with
`NOT_CONNECTED`. Otherwise that future would stay pending for the rest of the
program, and the scheduler would call a program waiting on it a deadlock
(§5.2). Closing a Unix listener also removes the path it bound, which lets a
server be restarted. `net.unix.listen` does not remove a path that is
already there, since it may belong to a server that is running, and taking it
would leave that server listening on a name nothing can reach.

A socket the program stops referring to is closed by the garbage collector.
This is a backstop only. `net.close` is how to release one at a known moment,
and a server that never closes anything will hold every connection it has
ever accepted until a collection happens to run. The backstop rules out the
unbounded case: a long-running program that leaks descriptors until it can
open no more. A socket with an operation in flight is never collected.

A closed handle raises rather than reaching another connection. The
descriptor lives in the block the value points at, so closing one is visible
through every copy of that value at once. Nothing in the language can hold a
number the operating system has since given to a different socket.

#### One operation per side

A socket may have one read-side operation (`read`, `receive`, `accept`) and
one write-side operation (`write`, `send`, `connect`) outstanding at a time.
A second on the same side raises `INVALID`: two readers of one stream have no
meaning, since the bytes would go to whichever the scheduler woke first.

#### Failures

Every function but `net.close` is fallible. The ones that answer without
waiting (`net.tcp.listen`, `net.udp.bind`, `net.unix.listen`, `net.address`,
`net.peer`) raise at the call. Everything that waits completes its future
with the error instead, so a caller meets it at the await, as with
`process.child.run` (§6.12). The codes are the `net.ErrorCode` members listed
in §2.9.

### 6.16 `http`

```
import 'http';
```

Serving HTTP/1.1 requests and making them, over `net` (§6.15) and, for the
client, over `tls` (§6.18). An `https://` URL dispatches through
`tls.connect`, which verifies the peer by default. Like `test` (§6.13), this
module is written in Nio. It needs nothing from C, so it is parsed, checked
and compiled as a user module is. The C files it pulls in are the ones its
own imports need, which, through `tls`, include the crypto stack.

#### Methods and codes

`http.Method` is `GET`, `POST`, `PUT`, `DELETE`, `PATCH`, `HEAD`, `OPTIONS`.
Printing a member gives the token that goes on the wire (§6.1).

`http.ErrorCode` is this module's own enum, not the shared `ErrorCode` of
§2.9, because a module written in Nio cannot extend that one. Its members
begin at 100 so they cannot collide with the `NIO_ERR_*` values a `net`
failure carries, and a handler may compare a caught error against both:
`MALFORMED_REQUEST` (100), `MALFORMED_RESPONSE`, `TOO_LARGE`,
`UNSUPPORTED_SCHEME`, `TOO_MANY_REDIRECTS`.

#### Serving

```
import 'http';

http.Response hello(http.Request req) {
    return http.text(200, "hello");
}

http.Server s = http.server();
s.route(http.Method.GET, "/", http.Response (http.Request r) -> hello(r));
s.route(http.Method.GET, "/users/:id", http.Response (http.Request r) ->
    http.text(200, "user " + string.from(r.params["id"])));
s.route(http.Method.GET, "*", http.Response (http.Request r) -> http.text(404, "not found"));

await s.listen("0.0.0.0", 8080);
```

`http.server()` answers a value, not a global, so one program may run two
servers: an admin port beside a public one, or two in one test.

| Member | Signature | Description |
|---|---|---|
| `http.server()` | `→ Server` | A server with no routes. |
| `s.route(m, pattern, h)` | `Method, Pattern, ResponseFunction →` | Registers a handler. |
| `s.bind(host, port, opts?)` | `String, int, Options → int!` | Binds, and answers the port actually bound. |
| `s.serve()` | `→ Future<void!>` | Accepts until `close`. |
| `s.listen(host, port, opts?)` | `String, int, Options → Future<void!>` | `bind` then `serve`. |
| `s.port()` | `→ int` | The port bound, or 0. |
| `s.close()` | `→` | Stops accepting. Connections already being served finish. |

Host and port are two arguments rather than `net`'s single `"host:port"`
string, so an IPv6 literal needs no brackets. Port `0` lets the operating
system choose, and `bind` answers which port it chose. This is how a program
or a test takes a port without naming one.

`bind` is separate from `serve` for this reason: calling an async function
does not run its body (§5.2), so a program that started the accept loop with
`async.run` and then asked `port()` would ask before anything had bound.
`bind` is synchronous, so the port is known the moment it returns. `listen`
is the two together, for a program that runs nothing else.

One function registers every route. A registration makes two choices: what
the route matches on, and what shape its handler has. Each is a union
(§2.11), so `route` accepts every combination of the two and there is nothing
else to register with:

```
export union Pattern { String, RegExp }

export union ResponseFunction {
    Function(Request)<Response!>        Sync,
    Function(Request)<Future<Response>> Async
}
```

§2.10 rule 7 picks both members from what the call writes, so no
registration names a tag. Each union is needed for the same reason. No single
type could accept both alternatives: function types are structural and
assignability between them is equality (§2.6), and a `String` is not a
`RegExp`. A second function per choice would multiply, to four of them for
these two.

A `String` pattern is split on `/`. A segment is a literal, `:name` (which
binds into `req.params`), or `*`, which matches that segment and every one
after it. A `RegExp` pattern matches the whole path and binds nothing.
Segments are the default because a regex per route per request is a real
cost and most routes do not need one. The first matching route wins,
whichever form it took, so a catch-all belongs last; one registered first
would shadow everything. A route matches one method, so a catch-all is per
method, and a request matching no route at all gets a plain 404.

`Sync` is what almost every handler uses and costs nothing: no future, no
frame, no trip through the scheduler. `Async` is for a handler that waits on
something, and only those pay for a future. Both go in the one route table
and may be mixed freely; matching either union costs one pointer comparison
(§2.11).

A `Sync` handler may raise, and the server answers 500. §2.9 lets a function
value take a fallible contract whether or not its body can fail, so a raising
and a non-raising handler both fit that member. `Async`'s future is not
fallible, because `Future<T>` and `Future<T!>` are unrelated types with no
coercion either way (§2.7). So an async handler catches its own failures and
answers a status for them; that is its job rather than the server's.

#### Requests and responses

```
type Request {
    Method method;  String path;  String query;
    Map<String, String> headers;    // names lower-cased
    Map<String, String> params;     // bound by ":name" segments
    byte[] body;  String peer;
    Function()<Future<byte[]!>> read;   // the body in pieces
    int? maxBody;                       // what `bytes` and `text` will hold

    byte[] async bytes();
    String async text();
}

type Response {
    int status;  Map<String, String> headers;  byte[] body;
    BodyStream? stream;                 // a body written in pieces
}
```

`path` is percent-decoded; `query` is not, because decoding before splitting
would lose the difference between a separator and an escaped one. Pass it to
`http.parseQuery`.

| Function | Signature | Description |
|---|---|---|
| `http.text(status, body)` | `int, String → Response` | With `text/plain`. |
| `http.json(status, encoded)` | `int, String → Response` | Already-encoded JSON, with `application/json`. Pair it with `json.toText` (§6.3). |
| `http.bytes(status, body)` | `int, byte[] → Response` | Raw, with no content type. |
| `http.redirect(status, location)` | `int, String → Response` | With `Location`. |

The server writes `Content-Length` for a whole body, so framing is never
ambiguous. A streamed body has no length when the header goes out and is
framed chunked instead.

#### Streaming

A body is a stream in every direction, and one shape carries all three
directions: a closure answering the next piece of a body, and an empty array
once there is no more. Because it is one shape, a body moves between
directions without being held whole anywhere. A client response can be the
producer of a server response.

```
export union BodyStream {
    Function()<Future<byte[]>>  Plain,
    Function()<Future<byte[]!>> Raising
}

http.stream(status, produce)    // int, BodyStream → Response
```

`BodyStream` is a union for `ResponseFunction`'s reason: a producer that
cannot fail and one that can are unrelated function types (§2.6), and §2.9's
coercion does not reach inside a future (§2.7). Rule 7 picks the member from
what the call writes, so no call site names a tag.

**Out of a handler.** A `Response` carrying a `stream` is framed chunked, any
`Content-Length` on it is dropped, and the producer is called until it answers
an empty array. A producer that never does is a response that never ends,
which is how server-sent events work. Two things follow from the header going
out before the first piece. A HEAD answers the headers and none of the body,
so the producer is never called at all. And a producer that fails part-way
through has no status left to send, so the body ends without its terminator
and the connection closes. That is the only way to say "not the whole body"
to a peer that has already read a 200.

**Into a client.** `ClientResponse.read` answers one piece per chunk, so an
event written on its own arrives on its own. The connection lives as long as
the body and closes itself at the end of it; `close` gives up on the rest.
`bytes` and `text` are the whole body, held to `ClientOptions.maxBody`.

**Into a handler.** `routeStream` registers a route whose request body the
server does not read: `Request.body` is empty and `Request.read` answers the
pieces as they arrive. On an ordinary route `read` hands over `body` once, so
one handler reads either kind. It is a second registration rather than the
default because pulling a body means awaiting, and only a named `async`
function can await (§5.2, there being no async function literal). Making
every route stream would cost every inline handler its shape, for a body
`maxBody` already bounds. What a streaming handler leaves unread is drained,
up to 256 KB, after it answers. Past that the connection is given up instead,
since the next message begins where this body ends.

#### Making a request

```
import 'http';

http.ClientOptions options = { headers: { "accept": "application/json" }, timeout: 5000 };
http.ClientResponse r = await http.request(http.Method.GET, "http://example.com/", options);
print(r.status);
```

| Function | Signature | Description |
|---|---|---|
| `http.request(m, url, opts?)` | `Method, String, ClientOptions → Future<ClientResponse!>` | One request. The failure arrives at the `await`. |
| `http.options()` | `→ ClientOptions` | An empty one, for building with the setters. |

There are no `get`/`post` shorthands: `http.request(http.Method.GET, url)` is
already short, and if `get` existed, six others should exist too.

```
type ClientOptions {
    Map<String, String>? headers;  String? body;  byte[]? bodyBytes;
    Duration? timeout;  bool? followRedirects;
    tls.Options? tls;              // what an https dial hands to tls.connect
    int? maxBody;                  // the largest response body to read, no limit if null

    void setHeader(String name, String value);   // folds the name
    String? getHeader(String name);
    void removeHeader(String name);
}

type ClientResponse {
    int status;  String reason;
    Map<String, String> headers;
    String[] setCookies;
    byte[] body;
}
```

Every `ClientOptions` field is optional, which makes `{}` and any subset a
legal record literal (§2.4 requires every non-optional field). `body` is a
`String` because Nio has no implicit conversion (`body: "test"` must
typecheck), and `bodyBytes` is for what is not text, winning when both are
set. The three accessors exist because header names are case-insensitive and
a raw map is not.

`setCookies` is separate from `headers` because `Set-Cookie` is the only
header that may legitimately repeat and cannot be re-joined. Its value
contains commas, so folding several into one comma-separated string destroys
them. Every other repeat folds, which is what RFC 9110 says a list header
means.

`followRedirects` is off by default; on, it follows at most five and then
raises `TOO_MANY_REDIRECTS`. `maxBody` is the largest response body the
request reads; left null there is no limit.

An `https://` URL dispatches through `tls.connect` (§6.18), which verifies
the peer by default: the chain must build to a root in the host's trust
store, the leaf must be valid for the URL's own host, and the peer must prove
it holds the leaf's private key. There is no way to ask this module to verify
a different name than the one it is fetching from, because a mismatch between
the two is what verification exists to catch. `ClientOptions.tls` carries
`roots`, `insecureSkipVerify` and `pins` through to `tls.connect` with the
meanings §6.18 gives them. Its `alpn` is ignored, because this module speaks
HTTP/1.1 and offers exactly that. A `Location` that names `https://` moves a
redirect chain onto TLS mid-flight, which is the commonest redirect on the
web. Any scheme other than `http` and `https` raises `UNSUPPORTED_SCHEME`.
The server side has no TLS. `tls` is a client (§6.18), so `http.Server`
speaks plain HTTP and sits behind a terminating proxy when it needs to be
reached over https.

There is no connection pool. A pool is a cache with an eviction policy and a
half-closed-socket problem, and it belongs above this function rather than
hidden inside it.

#### Parsing on its own

Every piece the server and client are built from is exported, because a
malformed chunk header is easier to check as a string than to arrange over a
connection. A program with its own transport can also reuse them.

| Function | Signature | Description |
|---|---|---|
| `http.parseRequestLine(line)` | `String → RequestLine!` | `GET /p?q HTTP/1.1`. |
| `http.parseStatusLine(line)` | `String → StatusLine!` | `HTTP/1.1 200 OK`. |
| `http.parseHeaders(lines)` | `String[] → HeaderBlock!` | The block between the first line and the blank one. |
| `http.framingOf(headers)` | `Map<String, String> → Framing!` | `Content-Length` or `chunked`; both at once is rejected. |
| `http.decodeChunked(s, start, maxBody)` | `String, int, int? → Chunked!` | As many complete chunks as `s` holds. A null `maxBody` reads whatever arrived. |
| `http.parseUrl(url)` | `String → Url!` | Scheme, host, port, path, query. |
| `http.parseQuery(query)` | `String → Map<String, String>` | `a=1&b=2`, with `+` for a space. |
| `http.percentDecode(s)` | `String → String` | `%41` → `A`. |
| `http.percentEncode(s)` | `String → String` | Everything outside the unreserved set. |
| `http.getHeader(h, name)` | `Map<String, String>, String → String?` | Case-insensitively. |
| `http.setHeader(h, name, value)` | `Map<String, String>, String, String →` | Case-insensitively. |
| `http.reasonFor(status)` | `int → String` | The phrase a server writes. |
| `http.CRLF`, `http.CRLF2` | `String` | The line terminator, and the blank line. |

`CRLF` is `"\r\n"` (§1.6). It is exported so that a program writing a raw
message uses the same constant as the module.

Parsing works on `String` rather than `byte[]` throughout, and so should
anything built on it. Every function `string` offers is C over the packed
bytes, so searching a header block is a vectorized `memchr`, where the same
parser over a `byte[]` would compare an element at a time. A Nio string is a
byte string (§6.6), NUL included, so nothing is lost: a binary body survives
`fromByteArray` and comes back out of `toByteArray` unchanged.

#### Limits

A server that will read a header block of any size is denied service by one
connection that never sends the blank line. So the limits are fixed rather
than advisory, for the same reason that makes `regexp` a Pike VM (§6.14).

| Limit | Value |
|---|---|
| Any one line (request, status, header) | 8 KB |
| The whole header block | 64 KB |
| How many headers | 100 |
| Body a server buffers | 10 MB, or `Options.maxBody` |
| Body a client holds | no limit, or `ClientOptions.maxBody` |
| Body a streaming handler left unread, before the connection is dropped | 256 KB |

`Options` is the server's: `timeout`, `backlog` (default 128), and `maxBody`.
A body past the cap is answered 413 and the connection closed.

`maxBody` bounds what is buffered, not what may arrive. A `routeStream` route
reads whatever the peer sends, and the ceiling applies to that handler only
if it calls `bytes` or `text`. Past the ceiling those raise `TOO_LARGE`. This
is an error rather than a truncation, since a body cut short can still parse
as a whole one.

The client's limit is the caller's to set, and there is none by default.
A response body is a stream, so the client imposes no ceiling and the caller
decides how much to keep. `ClientOptions.maxBody` is what `bytes` and `text`
will hold. `read` is the stream and is not held to it, because a caller
reading piece by piece has already made the decision the field is for. When
set, the limit is also compared against the declared length before the body
is read, and a response over it raises `TOO_LARGE`, since a client has nobody
to answer 413 to. The consequence of the default: `text` against a host the
program does not control, with no limit named, holds whatever that host
sends.

#### Failures

`http.request` and the parsing functions are fallible; a handler's own failure
becomes a 500 and never reaches the client as text. A malformed request gets
400 before any route is consulted, and one over a limit gets 413.

`serve` completes with an error only if accepting fails for a reason that is
not transient. Running out of descriptors is retried, since a connection that
finishes gives one back.

### 6.17 `crypto`

Cryptographic primitives: hashing, message authentication, key derivation,
authenticated encryption, Diffie-Hellman key agreement, the two byte-to-text
codecs they are exchanged in, and random bytes from the operating system.
The HTTPS stack (`tls`, §6.18, and `x509`, §6.19) is built on this module.
Every body in it is a transliteration of its specification (FIPS 180-4 for
the hashes, RFC 2104 for HMAC, RFC 5869 for HKDF, RFC 8439 for
ChaCha20-Poly1305, RFC 7748 for X25519, RFC 4648 for the codecs). The one
exception is the part no specification writes out, which is generated code
that has been formally verified (see X25519 below). All of it is locked to
published test vectors by `tests/crypto_test.nio`.

| Function | Signature | Description |
|---|---|---|
| `crypto.sha256(data)` | `byte[] → byte[]` | The SHA-256 digest: 32 bytes. |
| `crypto.sha384(data)` | `byte[] → byte[]` | The SHA-384 digest: 48 bytes. |
| `crypto.hmacSha256(key, data)` | `byte[], byte[] → byte[]` | The HMAC of `data` under `key`: 32 bytes. |
| `crypto.hmacSha384(key, data)` | `byte[], byte[] → byte[]` | The same over SHA-384: 48 bytes. |
| `crypto.hkdfExtractSha256(salt, ikm)` | `byte[], byte[] → byte[]` | HKDF-Extract: a pseudorandom key from input key material. An empty salt is the RFC's "no salt". |
| `crypto.hkdfExtractSha384(salt, ikm)` | `byte[], byte[] → byte[]` | The same over SHA-384. |
| `crypto.hkdfExpandSha256(prk, info, length)` | `byte[], byte[], int → byte[]` | HKDF-Expand: `length` bytes of output key material. `length` past 255 × 32 is a runtime error. |
| `crypto.hkdfExpandSha384(prk, info, length)` | `byte[], byte[], int → byte[]` | The same over SHA-384; the cap is 255 × 48. |
| `crypto.chacha20Poly1305Seal(key, nonce, plaintext, aad)` | `byte[], byte[], byte[], byte[] → byte[]` | Encrypts and authenticates: the ciphertext with its 16-byte tag appended. The key is 32 bytes and the nonce 12. |
| `crypto.chacha20Poly1305Open(key, nonce, ciphertext, aad)` | `byte[], byte[], byte[], byte[] → byte[]!` | Its inverse. Fails if the message was tampered with. |
| `crypto.aesGcmAvailable()` | `→ bool` | Whether this CPU can do AES-GCM in constant time. |
| `crypto.aes128GcmSeal(key, nonce, plaintext, aad)` | `byte[], byte[], byte[], byte[] → byte[]!` | AES-128-GCM: a 16-byte key, a 12-byte nonce, the tag appended. Fails where the hardware is absent. |
| `crypto.aes128GcmOpen(key, nonce, ciphertext, aad)` | `byte[], byte[], byte[], byte[] → byte[]!` | Its inverse. Fails if the message was tampered with, or where the hardware is absent. |
| `crypto.x25519PublicKey(privateKey)` | `byte[] → byte[]` | The 32-byte X25519 public key for a 32-byte private key. |
| `crypto.x25519SharedSecret(privateKey, peerPublicKey)` | `byte[], byte[] → byte[]!` | The 32-byte shared secret. Fails if the peer's key has small order. |
| `crypto.base64Encode(data)` | `byte[] → String` | RFC 4648 §4: the standard alphabet, padded. |
| `crypto.base64Decode(s)` | `String → byte[]!` | Its strict inverse. Fails on anything else. |
| `crypto.hexEncode(data)` | `byte[] → String` | Lower-case hex, two characters per byte. |
| `crypto.hexDecode(s)` | `String → byte[]!` | Its inverse; either case in. Fails on anything else. |
| `crypto.rsaVerifyPkcs1v15(modulus, exponent, hash, digest, signature)` | `byte[], int, String, byte[], byte[] → void!` | Checks an RSA PKCS#1 v1.5 signature. Answers nothing; fails if it does not verify. |
| `crypto.rsaVerifyPss(modulus, exponent, hash, digest, signature)` | `byte[], int, String, byte[], byte[] → void!` | The same for RSASSA-PSS, salt length equal to the hash length. |
| `crypto.ecdsaVerify(curve, publicKey, digest, r, s)` | `String, byte[], byte[], byte[], byte[] → void!` | Checks an ECDSA signature on `"p256"` or `"p384"`. The key is a SEC 1 uncompressed point; `r` and `s` are the signature's integers as big-endian bytes, already out of their DER (`x509.parseEcdsaSignature`). Answers nothing; fails if it does not verify. |
| `crypto.randomBytes(n)` | `int → byte[]` | `n` bytes from the operating system's generator. |

```
import 'crypto';
import 'string';

byte[] digest = crypto.sha256(string.toByteArray("abc"));
print(crypto.hexEncode(digest));
// ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad

byte[] mac = crypto.hmacSha256(crypto.randomBytes(32), digest);
print(mac.length);   // 32
```

The hash is part of the function name, not an argument. A wrong algorithm
name must be a compile error, and the checker has no type for "one of these
two strings". So there is no `crypto.hash("sha256", …)` to misspell at run
time. SHA-256 and SHA-384 are the two that TLS 1.3 needs; a new hash is a
new set of names. For the same reason, the cipher is part of the name
`chacha20Poly1305Seal` and the curve is part of the name `x25519PublicKey`.

#### Authenticated encryption

`crypto.chacha20Poly1305Seal` and `crypto.chacha20Poly1305Open` are the AEAD
of RFC 8439: encryption and authentication in one operation, over a 32-byte
key and a 12-byte nonce. The `aad` argument is for data that travels in the
clear but must not be alterable (a header, a message type, a sequence
number). Seal answers the ciphertext with its 16-byte tag appended, which is
the form that every protocol carrying one uses.

```
import 'crypto';
import 'string';

byte[] key = crypto.randomBytes(32);
byte[] nonce = crypto.randomBytes(12);
byte[] sealed = crypto.chacha20Poly1305Seal(key, nonce, string.toByteArray("hello"), []);

byte[] plain = crypto.chacha20Poly1305Open(key, nonce, sealed, []) catch e {
    print("forged or corrupted");   // e.code == crypto.ErrorCode.AUTHENTICATION
    return;
};
print(string.fromByteArray(plain));   // hello
```

`crypto.aes128GcmSeal` and `crypto.aes128GcmOpen` are the other AEAD that
TLS 1.3 uses (SP 800-38D), in the same shape: a 16-byte key, a 12-byte
nonce, the 16-byte tag appended. They exist only where the CPU can run AES
in constant time. That is why both halves are fallible where ChaCha's seal
is not, and why `crypto.aesGcmAvailable()` exists to be asked first. A
software AES is either a table indexed by key-derived bytes (a cache-timing
oracle) or a bitsliced implementation, which is a large project of its own.
This module provides neither and refuses instead; §6.18 offers the suite
only when the answer is yes. Everything, including the S-box of the key
schedule, goes through the CPU's AES and carry-less-multiply instructions.

A nonce must never repeat under one key. Reusing one destroys the security
of both messages. For Poly1305 it also leaks the authentication key itself;
for GCM it leaks the GHASH key, after which anything can be forged. Either
count (a message counter, which is what TLS does) or draw 12 random bytes
per message and send them alongside. A fixed nonce is not safe.

`Open` is fallible, and that is its purpose (§2.9). A message whose tag
does not verify raises `crypto.ErrorCode.AUTHENTICATION`. This is a code of
its own rather than `INVALID`, because the two mean opposite things.
`INVALID` is input nobody could read. `AUTHENTICATION` is input that was
read and is a forgery, which a protocol usually answers by ending the
connection. Nothing partial comes back: the tag is checked before a single
byte is decrypted, so there is no path on which a caller sees
unauthenticated plaintext. A ciphertext too short to contain a tag fails
the same way and with the same message. Telling an attacker how their
forgery was malformed is what makes a decryption oracle.

There is no software AES-GCM. Software AES is a table lookup indexed by key
material, which leaks through the cache. ChaCha20 is add-rotate-xor with no
tables and no such channel. Most TLS 1.3 servers accept ChaCha20-Poly1305,
so a CPU without the AES instructions can still connect to them.

#### Key agreement

`crypto.x25519PublicKey` and `crypto.x25519SharedSecret` are X25519 Diffie-
Hellman (RFC 7748). A private key is any 32 random bytes (the required
clamping happens inside). Both parties reach the same 32-byte secret from
opposite halves:

```
import 'crypto';

byte[] myPrivate = crypto.randomBytes(32);
byte[] myPublic = crypto.x25519PublicKey(myPrivate);
// ... exchange public keys ...
byte[] shared = crypto.x25519SharedSecret(myPrivate, peerPublic) catch e {
    print("the peer sent an unusable public key");
    return;
};
```

The shared secret is not a key. It is a curve point, not uniform bytes. Run
it through `crypto.hkdfExtractSha256` and `hkdfExpandSha256` to get keys
from it. That is what TLS does, and it is what the HKDF functions are here
for.

`x25519SharedSecret` is fallible because the check is mandatory. A peer can
send a point of small order, which forces the "shared" secret to a constant
the peer chose. Both sides then agree on a value that neither private key
influenced. RFC 7748 §6.1 makes rejecting that optional. RFC 8446 §7.4.2
makes it mandatory for TLS 1.3, and it is mandatory here, reported as
`crypto.ErrorCode.INVALID`. Because the function is fallible, an
implementer cannot forget the check.

The curve arithmetic underneath is not hand-written. It is fiat-crypto's
generated, formally verified field arithmetic, vendored unmodified
(`build/runtime/lib/fiat_curve25519_64.h`, MIT/Apache-2.0/BSD-1-Clause).
The Montgomery ladder over it is transliterated from BoringSSL's, which is
the arrangement whose correspondence to scalar multiplication was proven in
Coq. A carry bug in curve arithmetic can fire on one input in 2^60 and
recover the key when it does. No test vector would find such a bug, so the
only real defense is to not write that arithmetic by hand.

The two decoders are fallible (§2.9), for the same reason `string.toInt`
is. Base64 and hex are text from outside the program (a config file, a
header, a PEM block), and text that does not decode is the parser's
expected case. Every failure carries `crypto.ErrorCode.INVALID`, the same
enum every raising module names (§2.9). The decoders are also strict: no
whitespace, exact padding, zero trailing bits. Two different texts must not
decode to one value, and a PEM reader unwraps its own lines before
decoding.

`crypto.randomBytes` is the module's only source of randomness, and it is
the operating system's (`getentropy`, `getrandom`, or `BCryptGenRandom`).
The module never uses a generator of its own. `randomBytes` is not
fallible: a `catch` cannot act on a machine whose entropy source will not
answer, so that failure is a runtime error (§5.5). A negative `n` is a
runtime error too.

Comparing secrets is not this module's job. `==` on strings and byte arrays
stops at the first differing byte, which gives an attacker who can time it
an oracle. `string.equalsConstantTime` and `string.bytesEqualConstantTime`
(§6.6) exist for this, and anything comparing a MAC, a token or a tag goes
through them.

The codecs branch on the value of every input byte, so they are not
constant-time and must not be given secret key material. They are for
certificates and for the public halves of tokens. Everything else here is
constant-time. The hashes, HMAC and HKDF branch only on input lengths,
which are public. ChaCha20 and Poly1305 have no tables and no
data-dependent branches. X25519's ladder runs a fixed number of iterations
whatever the scalar, with a conditional swap done by arithmetic rather than
by a branch. The rules the C bodies follow (no secret-dependent branches,
no secret-indexed loads) are documented in the header of
`build/runtime/lib/crypto.c`, which also lists which functions handle
secret material at all.

A key, nonce, or curve point of the wrong length stops the program (§5.5)
rather than raising. Those lengths are fixed by the algorithm and a program
generates its own key, so a wrong one is a mistake in the program, like an
index out of range. What comes from outside is the message, and that is the
failure the fallible functions above report.

The two RSA verifiers answer nothing and fail instead. A verifier that
returned a `bool` would have callers who forget to test it, which is a
whole family of real vulnerabilities. A verifier that raises cannot be
ignored, because §2.9 makes the failure the caller's to handle. `hash`
names the digest: `"sha256"` or `"sha384"`, spelled as the module's own
hash functions are. Anything else, including SHA-1 and MD5, which this
stack refuses, is an error rather than a fallback.

The two failures are told apart, and `x509` reports this distinction as
the difference between a broken certificate and an untrusted one. A key or
signature that cannot be used raises `crypto.ErrorCode.INVALID`. That
covers a modulus under 1024 bits or over 8192, an even modulus, an even
exponent or one outside 3…2³¹−1, a digest of the wrong length for the
named hash, and a signature that is not exactly as long as the modulus or
is not below it. A well-formed signature that does not verify raises
`crypto.ErrorCode.AUTHENTICATION`, the same code a forged AEAD message
gets, and says nothing about which check failed.

PKCS#1 v1.5 verification re-encodes the block it expects (padding,
DigestInfo, digest) and compares the whole block at once. Nothing is parsed
out of the recovered bytes, so there is no parser for a forged block to
exploit through leniency. Encode-then-compare is the structure that ended
the Bleichenbacher '06 low-exponent forgery class.
`tests/golden/rsa_wycheproof_cases.txt` holds 884 Wycheproof cases that
check it.

`crypto.ecdsaVerify` has the same contract over the two NIST curves the web
serves, P-256 and P-384. The curve is named rather than inferred from the
key's length, so a mistake is a clear `INVALID` rather than a silent guess.
The same code covers a key that is not an uncompressed point on the named
curve, and an `r` or `s` outside 1…n−1. `AUTHENTICATION` again means only
"does not verify". The field arithmetic underneath is fiat-crypto's
formally verified generated code. The group arithmetic uses the complete
addition formulas of Renes, Costello and Batina. The whole is
checked by `tests/golden/ecdsa_wycheproof_cases.txt`, 988 Wycheproof cases
across both curves. ASN.1 stays out of C: the signature's two integers
arrive already parsed, and `x509.parseEcdsaSignature` (§6.19) is where a
DER `ECDSA-Sig-Value` becomes them.

Only public values are involved in any verifier, so none is constant-time
and none needs to be. There is no signing here, and none is planned.
Signing is the most side-channel-fragile operation in a TLS stack, and this
stack does not contain it.

### 6.18 `tls`

A TLS 1.3 client: the key schedule (RFC 8446 §7.1), the record layer (§5),
and the handshake (§4). It is the protocol half of the HTTPS stack built
over `crypto`, and it is written in Nio like `test` and `http`.

The server is authenticated, and both halves of that are required.
`tls.connect` builds the chain to a root in the host's trust store through
`x509` (§6.19) and holds the leaf to the name it was asked to connect to.
It then checks the CertificateVerify signature, which proves that the peer
holds the private key for the certificate it presented. A chain check
alone would authenticate nobody: certificates are public, so anyone can
replay somebody else's. Verification is on when `Options` says nothing
about it. `Options.roots` narrows what is trusted. `Options.pins` narrows
the acceptable verified chains to those carrying a named key.
`Options.insecureSkipVerify` turns all of it off at once.

The signature schemes verified, in the chain and in the CertificateVerify,
are RSA (PSS and, in certificates only, PKCS#1 v1.5) and ECDSA over P-256
and P-384. Between them, these are what the public web serves. The
ClientHello offers exactly the schemes that can be checked. So a server
whose certificate this client could not verify fails the handshake at the
start rather than being trusted unchecked. A curve outside those two
(P-521, or an explicit-parameters key) is refused the same way.

> **Revocation (CRL, OCSP) is not implemented**: a certificate withdrawn
> before its expiry is still accepted here.

Every secret-handling operation is a call into `crypto` (§6.17); this
module arranges bytes. It compares a secret in exactly one place, the
Finished message, and does it with `string.bytesEqualConstantTime` (§6.6).
The key schedule owns no socket and works on arrays the caller brings.
`RecordProtection` seals and opens records it is given. This separation
exists because QUIC (RFC 9001) uses the key schedule but replaces the
record protection.

The scope is small:

- TLS 1.3 only. There is no 1.2, so none of the CBC, renegotiation or
  RSA-key-exchange attacks apply: that code does not exist.
- Two cipher suites: `TLS_CHACHA20_POLY1305_SHA256` always, and
  `TLS_AES_128_GCM_SHA256` where §6.17 can do AES in constant time, for a
  server that refuses ChaCha.
- One key exchange group (x25519).
- No 0-RTT, now or later.
- No client certificates, which means this client never signs anything.

What it does support works against real servers. The tests run it against
a peer of its own over loopback, and it completes handshakes with OpenSSL
and with public HTTPS servers.

```
import 'tls';
import 'string';

String crlf = string.fromByteArray([13, 10]);
tls.Conn c = await tls.connect("example.com:443", "example.com",
    { alpn: ["http/1.1"] });
await c.write(string.toByteArray(
    "GET / HTTP/1.1" + crlf + "Host: example.com" + crlf + crlf));
byte[] head = await c.read(1024);
await c.close();
```

`tls.connect(address, serverName, options?)` answers a `Conn` on a future,
the way `net.tcp.connect` answers a `Socket`. `serverName` goes in the SNI
extension and is the name the certificate is checked against. `Conn`
mirrors `net`'s shapes, so a caller can use either in the same way.
`read(n)` answers up to `n` bytes, and an empty array at the end of the
stream. `write(data)` takes any amount and splits it across records.
`close()` sends the `close_notify` that tells the peer the stream ended
rather than was cut. `Conn.alpn` is the protocol the server selected, or
`""`.

Two more endings exist because `close()` alone cannot express them.
`closeWrite()` sends the `close_notify` and leaves the transport open. This
is the first half of the clean shutdown TLS specifies: this end has no more
to say, but the peer may still have more. A caller that wants to know when
the peer has also finished can keep reading after it says it is done.
`abort(description)` is the other ending, for a connection that stops
because something was wrong. It sends a fatal alert naming the reason and
then closes. Without it, a peer that sent something unacceptable would see
only a closed socket, which looks like a network failure. It would then
retry and get the same answer without having been told why. The handshake
sends its own alerts; these two methods do the same for the rest of the
connection's life.

A server may ask for a client certificate. This client has none and never
signs, and §4.4.2 of RFC 8446 says what to do in that case: it sends a
`Certificate` message with an empty list and no `CertificateVerify`, and
the handshake continues. Refusing outright would break against every
server configured to request rather than require a certificate. That is
the common configuration: such a server asks every client and accepts the
clients that decline.

`tls.Options` carries `timeout`, `alpn`, `roots`, `insecureSkipVerify` and
`pins`. Advertising `http/1.1` costs a few bytes, and `https` in `http`
(§6.16) depends on it: an `https://` URL given to `http.request`
dispatches through `tls.connect` with this option set. `roots` is an
`x509.Certificate[]` to trust instead of the host's store. This is how a
client talks to a private CA. It narrows trust; it does not remove it.
`insecureSkipVerify` removes it: no chain, no hostname, no
CertificateVerify. It does not check less; it checks nothing. A connection
made under it is private against an eavesdropper and worthless against
anyone who can redirect traffic. It exists because `tls.testAccept` cannot
sign and so cannot be authenticated by anything.

`pins` is a set of SHA-256 hashes over the SubjectPublicKeyInfo of keys the
peer may hold (`x509.spkiPin`). It narrows what a verified chain may be; it
does not replace verification. The chain is built and checked first, and
the connection then continues only if some certificate in it carries a
pinned key. Any position counts, so a pin on an intermediate stays valid
for every leaf issued under it. Pinning defends against a certificate
authority that issues a certificate for this name when it should not. No
chain check can see that threat.

It is a set because a pinned key that rotates while only one pin is in
force causes an outage: pin the key in use and the key it moves to.
Pinning is opt-in for that reason. `pins` together with
`insecureSkipVerify` is refused (`MISCONFIGURED`) rather than resolved, and
so is an empty `pins`. Both would otherwise give a connection that
succeeds with nothing pinned, which is what a pinning mistake looks like
from the outside. A chain that verifies and carries no pinned key raises
`PIN_MISMATCH` and sends `bad_certificate`.

A failed verification carries `x509`'s own error code rather than one
generic certificate error. So a caller can tell an expired certificate
from a wrong hostname from one that nobody vouches for. The alert sent to
the peer is read off that same code (`tls.alertFor`) rather than decided a
second time.

| Function | Signature | Description |
|---|---|---|
| `tls.connect(address, serverName, options?)` | `String, String, tls.Options? → Future<Conn!>` | Opens a TCP connection and runs the client handshake over it, verifying the server's certificate. |
| `tls.handshakeClient(io, serverName, options?)` | `Transport, String, tls.Options? → Future<Conn!>` | The same handshake over a transport the caller brings, socket or not. |
| `tls.socketTransport(s, timeout)` | `Socket, Duration? → Transport` | The byte-stream seam over a `net` socket. |
| `tls.testAccept(io, config)` | `Transport, TestConfig → Future<Conn!>` | A test peer, not a server: it cannot sign, so only a client with `insecureSkipVerify` will talk to it (see below). |
| `tls.hkdfExpandLabel(secret, label, context, length)` | `byte[], String, byte[], int → byte[]!` | RFC 8446's HKDF-Expand-Label. Fails if `label` or `context` exceeds what the encoding carries (249 and 255 bytes). |
| `tls.deriveSecret(secret, label, transcriptHash)` | `byte[], String, byte[] → byte[]!` | Derive-Secret: a 32-byte expansion whose context is a transcript hash. |
| `tls.earlySecret(psk)` | `byte[]? → byte[]` | The schedule's first extraction, of the pre-shared key or of zeros when `null`. |
| `tls.handshakeSecret(early, shared)` | `byte[], byte[] → byte[]` | The second, folding in `crypto.x25519SharedSecret`'s answer. |
| `tls.masterSecret(handshake)` | `byte[] → byte[]` | The third and last. |
| `tls.transcript()` | `→ Transcript` | A fresh, empty transcript. |
| `tls.trafficKeys(trafficSecret, keyLength)` | `byte[], int → TrafficKeys` | One direction's write key (`keyLength` bytes: the AEAD's) and 12-byte IV. |
| `tls.finishedVerifyData(trafficSecret, transcriptHash)` | `byte[], byte[] → byte[]` | A Finished message's verify_data. Compare only with `string.bytesEqualConstantTime`. |
| `tls.plaintextRecord(contentType, payload)` | `int, byte[] → byte[]!` | An unprotected record: 5-byte header plus payload. Fails above 2^14 bytes. |
| `tls.parseRecordHeader(data)` | `byte[] → RecordHeader!` | Reads the first five bytes: content type and length. The legacy version bytes are ignored, as §5.1 requires. |
| `tls.protectionKeys(trafficSecret)` | `byte[] → RecordProtection` | One direction's record protection, keyed for ChaCha20-Poly1305, sequence at zero. |

`Transcript` holds the handshake messages pushed so far, each exactly as it
appears on the wire from its 4-byte message header on, with record framing
never included. `hash()` answers their running SHA-256.
`RecordProtection` is one direction of one connection, and its methods are
where the sequence number lives. `seal(contentType, payload)` protects one
record and advances. `open(record)` verifies, unwraps and advances. Both
are fallible. `open` propagates `crypto.ErrorCode.AUTHENTICATION` (§6.17)
for anything forged (a flipped bit, a replayed or reordered record), and
raises `tls`'s own codes for frames malformed in the clear.

```
import 'tls';
import 'crypto';
import 'string';

byte[] early = tls.earlySecret(null);
byte[] shared = crypto.x25519SharedSecret(myPrivate, peerPublic);
byte[] handshake = tls.handshakeSecret(early, shared);

tls.Transcript t = tls.transcript();
t.push(clientHello);
t.push(serverHello);
byte[] clientHs = tls.deriveSecret(handshake, "c hs traffic", t.hash());

tls.RecordProtection writer = tls.protectionKeys(clientHs);
byte[] record = writer.seal(tls.ContentType.HANDSHAKE, finishedMessage);
```

The key schedule's transitions are exported; its internal shape is not.
RFC 8446 puts a `Derive-Secret(·, "derived", "")` between each extraction
and the next, and a common implementation mistake is to skip it.
`earlySecret`, `handshakeSecret` and `masterSecret` each perform their own
"derived" step inside, so no caller can skip it. What a caller derives from
those secrets (`"c hs traffic"`, `"s ap traffic"`, the Finished keys) goes
through `deriveSecret` with the label the RFC names.

SHA-256 is the only hash in the schedule. Both supported suites
(`TLS_CHACHA20_POLY1305_SHA256` and `TLS_AES_128_GCM_SHA256`) hash with
SHA-256. SHA-384 would enter only with a suite that is not planned.
Certificate signatures hash through `crypto` directly and are not part of
the schedule. `trafficKeys` still takes the key length as a parameter (32
for ChaCha20-Poly1305, 16 for AES-128-GCM), which also lets the tests
replay RFC 8448's AES-GCM trace.

A protected record hides its content type, and its length can be padded.
What goes under the AEAD is the payload, then the true type byte, then any
number of zeros. The header authenticated alongside claims
`APPLICATION_DATA` whatever the truth (§5.2). `seal` writes no padding
(padding is a traffic-analysis countermeasure that is not applied here).
`open` strips it regardless, because reading padded records is the
receiver's obligation (§5.4). A record of only zeros names no type, which
is a `DECODE_ERROR`. A protected `CHANGE_CIPHER_SPEC` contradicts itself
and is an `UNEXPECTED_MESSAGE`.

The sequence number is state, not wire format. Each direction counts
records from zero at key installation, and the count is XORed into the IV
to make each record's nonce (§5.3). So nonce reuse, the failure §6.17 warns
every direct AEAD caller about, cannot happen here. The count never
travels: both ends count for themselves, so a record that is lost,
duplicated or reordered fails to authenticate. `open` advances its count
only when the record verified.

Every incoming byte is read through a bounds-checked cursor. `Reader`
answers a `DECODE_ERROR` where a direct index would run off the end. An
out-of-range index is a runtime error (§5.5), and in a parser reading what
an attacker sent, that is a denial of service anyone can trigger by
truncating a message. Nothing in the module indexes a received array
directly, and the tests feed it every prefix of a real ClientHello and
ServerHello to confirm this.

The client takes one message at a time, in the order the protocol names
them. A ServerHello arriving where a Finished belongs is refused rather
than acted on. A repeated extension is refused rather than resolved to one
of its copies. A cipher suite, group, or ALPN protocol that the client did
not offer is refused, even when the server selects it. This prevents the
state-confusion bugs found in implementations that accepted whatever
arrived.

`tls.testAccept` is a test peer and must never be used as a server. A real
TLS server signs its CertificateVerify with the certificate's private key,
and this stack contains no signing and no TLS server. `testAccept` sends a
CertificateVerify of the correct shape instead, carrying a signature that
verifies nothing. Any peer that checks signatures rejects it, which is the
correct outcome and means it cannot be misused without notice. It exists
because a client tested only against another party's server is tested only
when the network is available. With `testAccept`, the whole path runs
against itself over loopback in milliseconds, which is what makes the
failure paths testable.

`tls.ErrorCode` is the module's own enum, in `http`'s pattern (§6.16). It
starts at 200 so its members collide with neither the built-in codes nor
`http`'s: `ILLEGAL_PARAMETER` (200), `DECODE_ERROR` (201),
`RECORD_OVERFLOW` (202), `UNEXPECTED_MESSAGE` (203), `PEER_ALERT` (204),
`HANDSHAKE_FAILURE` (205), `DECRYPT_ERROR` (206),
`UNSUPPORTED_CERTIFICATE` (207), `UNSUPPORTED_EXTENSION` (208),
`TOO_MANY_MESSAGES` (209), `PIN_MISMATCH` (210) and `MISCONFIGURED` (211).
The last three are the ones that are not alert names. `PIN_MISMATCH` is a
verified chain carrying no pinned key, and becomes `bad_certificate`.
`MISCONFIGURED` is this end's own mistake (options that contradict each
other), so it is raised before a byte goes out and the peer is told
nothing. `TOO_MANY_MESSAGES` is what every limit in the module answers
with: a run of empty records, too many `KeyUpdate`s, too many
`user_canceled` alerts, too many `change_cipher_spec` records. It becomes
`unexpected_message`, which is the nearest alert in §6 to "you may not
send me that, in this quantity, here". RFC 8446 bounds none of those, so
the receiver sets its own limits. The rest of the names are RFC 8446 §6's
alert names, and `tls.alertFor(code)` is the mapping. So the alert sent on
the way out is read off the error caught rather than decided a second
time. Two other modules' codes reach it unwrapped. A record that fails to
authenticate keeps `crypto`'s `AUTHENTICATION`, which becomes
`bad_record_mac`. A certificate failure keeps `x509`'s code (§6.19), which
becomes `certificate_expired`, `unknown_ca`, `bad_certificate` or
`unsupported_certificate` as the case requires. Because they stay
unwrapped, a caller can tell those failures apart.
`tls.ContentType` names the record layer's four types:
`CHANGE_CIPHER_SPEC` (20), `ALERT` (21), `HANDSHAKE` (22),
`APPLICATION_DATA` (23).

### 6.19 `x509`

Certificates, and the decision to trust one: DER parsing, chain building,
signature checking, validity, and hostname matching. It is what makes
`tls` (§6.18) an authenticated channel rather than merely a private one.
Like `test`, `http` and `tls`, it is written in Nio. The only cryptographic
operation it performs, checking a signature, is a call into `crypto`
(§6.17).

```
import 'x509';
import 'fs';
import 'string';
import 'array';

byte[] pem = fs.readFile("/etc/ssl/cert.pem");
x509.Certificate[] roots = [];
forEach(x509.parsePem(string.fromByteArray(pem)), der) {
    array.push(roots, x509.parseCertificate(der));
}

x509.Certificate[] chain = x509.verifyChain(peerSentThese, "example.com", roots, null);
print(chain[0].subject);        // CN=example.com
print(chain.length);            // 3: leaf, intermediate, root
```

| Function | Signature | Description |
|---|---|---|
| `x509.parseCertificate(data)` | `byte[] → Certificate!` | Parses one DER certificate. The only way to make a `Certificate`. |
| `x509.verify(chain, options?)` | `Certificate[], Options? → Certificate[]!` | Builds and checks a chain from a leaf-first list. Answers the chain it built. |
| `x509.verifyChain(ders, dnsName?, roots?, at?)` | `byte[][], String?, Certificate[]?, DateTime? → Certificate[]!` | `parseCertificate` over each, then `verify`, in one call, so there is only one call to get wrong. |
| `x509.matchHostname(cert, host)` | `Certificate, String → void!` | Holds a certificate to a name (RFC 6125). Fails with `HOSTNAME_MISMATCH`. |
| `x509.checkSignature(child, parent)` | `Certificate, Certificate → void!` | Checks that `parent`'s key signed `child`. |
| `x509.rsaPublicKey(cert)` | `Certificate → RsaPublicKey!` | The certificate's RSA modulus and exponent, for a signature that is not a certificate's. |
| `x509.ecdsaPublicKey(cert)` | `Certificate → EcdsaPublicKey!` | The same for an EC key: the curve name and uncompressed point, exactly what `crypto.ecdsaVerify` takes. |
| `x509.parseEcdsaSignature(data)` | `byte[] → EcdsaSignature!` | A DER `ECDSA-Sig-Value` as its two integers `r` and `s`, under this module's DER strictness. |
| `x509.spkiPin(cert)` | `Certificate → byte[]` | SHA-256 over the certificate's SubjectPublicKeyInfo: a pin, and the same 32 bytes `openssl x509 -pubkey -noout \| openssl pkey -pubin -outform DER \| openssl dgst -sha256` writes. |
| `x509.pinned(chain, pins)` | `Certificate[], byte[][] → bool` | Whether any certificate in `chain` holds a key one of `pins` names. An empty `pins` matches nothing. |
| `x509.parsePem(text)` | `String → byte[][]!` | Every `CERTIFICATE` block in a PEM bundle, as DER. |
| `x509.systemRoots()` | `→ Certificate[]!` | The host's trusted roots. Fails rather than answering an empty set. |

`x509.Options` carries `dnsName`, `roots` and `at`; leaving `roots` out
reads the system store, and leaving `at` out means now. Leaving `dnsName`
out skips the hostname check. A tool inspecting a chain may want that; a
client never does.

A `Certificate` carries `subject`, `issuer`, `serialNumber`, `notBefore`,
`notAfter`, `signatureAlgorithm`, `publicKeyAlgorithm`, `rsaModulus`,
`rsaExponent`, `ecCurve`, `ecPoint`, `isCA`, `maxPathLen`, `keyUsage`,
`extKeyUsage`, `dnsNames`,
`ipAddresses`, `emailAddresses`, `permittedSubtrees`, `excludedSubtrees`,
`extensions` and `unhandledCritical`, plus the `raw*` byte ranges the
signature covers. `hasKeyUsage(bit)` reads one bit of the key-usage set and
`isSelfIssued()` compares subject to issuer.

Name constraints are read and enforced (RFC 5280 §4.2.1.10). A certificate
below a constrained issuer must be named within that issuer's subtrees. This
applies to the three general-name forms this module reads out of a
subjectAltName:

- `dNSName`, compared label by label, so `notexample.com` is not inside
  `example.com`;
- `rfc822Name`, in its three spellings, where a bare host is that host and
  not the names below it;
- `iPAddress`, compared under the subtree's mask.

Excluded wins over permitted, a constraint applies only to the form it names,
and a self-issued certificate that is not the leaf is skipped. A subtree in a
form this module cannot compare is not one it may skip. It is named in
`unsupportedConstraints`, and a CA carrying one cannot issue at all, since
nothing below it can be shown to be within its constraint.

The raw byte ranges are slices of what arrived, not re-encodings. A signature
covers bytes. A verifier that re-encoded a structure to check a signature
over it would be checking a different document whenever its encoder and the
issuer's disagreed. Names are compared the same way, by their DER. This is
also why the module needs no Unicode normalization to compare two names, and
it has none.

Nothing indexes a received array directly. Every byte goes through a
bounds-checked cursor that answers `BAD_CERTIFICATE` where an index would be
a runtime error (§5.5). In a certificate parser, such a runtime error is a
denial of service anyone can trigger by truncating a certificate. The tests
feed every prefix of every certificate in their corpus back in, and every
single-byte mutation of one.

The encoding rules enforced are DER's, not BER's:

- definite lengths only, minimally encoded;
- no trailing bytes after any structure;
- integers without a redundant leading zero;
- booleans that are exactly `0x00` or `0xFF`;
- BIT STRINGs whose unused bits are zero;
- OIDs without padded arcs;
- a field equal to its DER default (an explicit version 1, an explicit
  `critical=FALSE`) is refused rather than accepted.

Each of those is a place where two byte strings would otherwise mean one
certificate. Since a signature covers bytes, two spellings of a certificate
is a forgery primitive. The parser was compared against a reference parser
over 17,118 mutated certificates. Where the two differ,
`tests/golden/x509_der_cases.txt` records which way and why.

Everything unrecognized fails closed. A critical extension this module does
not implement makes the certificate unusable rather than ignorable, as RFC
5280 §4.2 requires. This keeps an unimplemented `nameConstraints` from being
silently skipped. A signature algorithm the module cannot check is a refusal,
never a pass.

SHA-1 and MD5 signatures are refused outright. Chosen-prefix collisions
against SHA-1 are practical, and every public trust program dropped it years
ago. RSA (SHA-256/384, PKCS#1 v1.5 and the TLS 1.3 PSS profile) and ECDSA
over P-256 and P-384 are verified, in any pairing a chain uses. For example,
the real Let's Encrypt hierarchy today has an RSA root vouching for an EC
intermediate. An EC key names its curve (RFC 5480's namedCurve arm only,
uncompressed points only). A curve outside those two, P-521 included,
parses and keeps its OID on the record, and is refused as soon as it is
asked to verify anything: a verifier that cannot check a signature must say
so. Ed25519 is refused the same way. Revocation is not implemented: neither
CRLs nor OCSP are consulted, so a certificate withdrawn before its expiry is
still accepted.

Only `subjectAltName` decides a hostname. The common name is not consulted.
It is a display string that often holds a hostname, and every major client
stopped honouring it years ago. A wildcard matches exactly one label, and
only as the entire leftmost one. So `*.example.com` covers `www.example.com`
and covers neither `example.com` nor `a.b.example.com`. A wildcard with
fewer than two labels behind it (`*.com`) is ignored. An address literal is
matched against `iPAddress` entries and never against a `dNSName`.

A chain is built, not accepted. The intermediates a peer sends are
candidates. The chain that comes back is the one this module assembled and
checked; it need not be the one that arrived, and it may use none of it.
Each issuer must:

- be a CA by `basicConstraints`;
- permit certificate signing, if it declares a `keyUsage` at all;
- be within its validity window;
- not have its `pathLenConstraint` exceeded.

The search is bounded in depth and in total work. Without the bound, a pool
of cross-signed certificates is an exponential search that a peer can cause
at no cost to itself.

`x509.ErrorCode` starts at 300, above the built-in codes, `http`'s and
`tls`'s: `BAD_CERTIFICATE` (300), `UNSUPPORTED_ALGORITHM` (301), `EXPIRED`
(302), `NOT_YET_VALID` (303), `UNTRUSTED_ROOT` (304), `HOSTNAME_MISMATCH`
(305), `CONSTRAINT_VIOLATION` (306), `BAD_SIGNATURE` (307). A caller acts on
this split. Bytes that are not a certificate, a certificate nobody vouches
for, and a certificate for a different server are three different things to
tell a user, and one "TLS error" would describe none of them.

### 6.20 Array members

| Member | Signature | Description |
|---|---|---|
| `a.length` | `int` | Number of elements (read-only). |

Arrays have no methods of their own; only records declare methods (§2.4).
Iterate with the `forEach` statement (§4.7):

```
int sum;

forEach([2, 5, 4], e) {
    sum = sum + e;
}
// sum == 11
```

---

## 7. Not in this version

Explicitly out of scope for v0.1, planned for later:

- loop labels: `break` always binds to the innermost loop or `switch`
  around it and `continue` to the innermost loop (§4.8). So leaving a loop
  from inside a switch clause, or leaving two loops at once, takes a flag
  the outer one tests
- async function values (a function value cannot be declared `async`)
- more array functions (`map`, `filter`, `reverse`, …) and more string
  functions (`startsWith`, a character-aware counterpart to the byte-counting
  `substring` of §6.6, …). The array ones must be implemented by the
  compiler, as `push` and `sort` already are, since §7.1 rules out the
  other route
- a live child (§6.12). `process.child.run` gives a child its whole input
  in advance and collects its whole output at the end. `inherit` covers the
  case where the output is not the program's business and belongs on the
  program's own streams. What is missing is a handle whose streams this
  program can write and read while the child runs, a `process.child.spawn` beside `run`. Also missing are
  killing a child and a timeout. Both need a way to cancel a future, which
  the language does not have
- more map functions (`merge`, `clear`, an entries view, …), map literal
  keys beyond string and number literals (bool, enum members, computed
  keys; today those go through indexed assignment, §2.8), and a forEach
  that takes a map directly
- using a declared function's name as a function value (today it must be
  wrapped: `int (int x) -> f(x)`, §3.4)
- a spread at the call site: an existing array cannot be passed to a
  variadic parameter as its arguments (§5.2), only element by element
- subtyping beyond a sealed type's own extenders. A `sealed` type (§2.4) is
  usable as the type of any value extending it, but a plain `extends`
  copies members and leaves the two types unrelated: a `Truck` cannot be
  passed where a `Car` is expected unless `Car` is sealed. Also missing:
  a hierarchy deeper than one level, and dispatching a method on a value
  whose type is the sealed one, which today must be narrowed first
- errors carrying more than a message and a code. `Error` has two fields
  (§2.9), and dispatching on `code` covers telling one failure from another.
  There is no way to declare an error type of one's own or to attach data
  beyond the two fields
- a shorthand for "propagate this one call". Propagation is automatic, so
  what is missing is a way to see at a call site that it can happen. Today
  the compiler knows and the reader does not
- exponent and hex numeric literals
- a package system on top of file imports (versioning, remote packages)
- passing records, optionals, maps or functions across the C boundary:
  §5.8 crosses only the scalar families, `String` and arrays of them,
  which is why §5.7 could fix a layout without growing a second
  `repr(C)`-style rule for values handed to C
- callbacks from C into Nio through an extern declaration: the two
  places C calls back today (the scheduler, `array.sort`) are built in,
  and a general mechanism needs a story for roots and re-entrancy first


### 6.21 `math`

The arithmetic every language ships: libm, the four functions every language
spells the same way, and the two conversions §3.6 refuses to guess at.

| Function | Signature | Description |
|---|---|---|
| `math.sqrt(x)` | `float → float` | The square root. |
| `math.pow(x, y)` | `(float, float) → float` | x raised to the power y. |
| `math.sin(x)`, `math.cos(x)`, `math.tan(x)` | `float → float` | Of an angle in radians. |
| `math.asin(x)`, `math.acos(x)`, `math.atan(x)` | `float → float` | The inverse functions, in radians. |
| `math.atan2(y, x)` | `(float, float) → float` | The angle of the point (x, y), in radians. |
| `math.log(x)` | `float → float` | The natural logarithm. |
| `math.log2(x)`, `math.log10(x)` | `float → float` | The logarithm in base 2 and in base 10. |
| `math.exp(x)` | `float → float` | e raised to the power x. |
| `math.floor(x)`, `math.ceil(x)` | `float → float` | The nearest whole number at or below x, and at or above it. |
| `math.round(x)` | `float → float` | The nearest whole number; a half rounds away from zero. |
| `math.isNaN(x)`, `math.isInfinite(x)` | `float → bool` | Whether x is NaN, or an infinity. |
| `math.toInt(x)` | `float → int` | x truncated toward zero. A runtime error when no `int` holds x. |
| `math.toFloat(n)` | `integer → float` | The nearest float to n. Takes either integer family. |
| `math.abs(x)` | `T → T` | The magnitude of x. |
| `math.min(a, b)`, `math.max(a, b)` | `(T, T) → T` | The smaller, and the larger. |
| `math.clamp(x, lo, hi)` | `(T, T, T) → T` | x held within lo and hi: `min(max(x, lo), hi)`. |
| `math.PI`, `math.E` | `float` | The two constants, 3.141592653589793 and 2.718281828459045. |

No function in this module raises, and none stops the program, except
`toInt`. A float operation with no answer answers what IEEE 754 says:
`math.sqrt(-1.0)` is NaN, `math.log(0.0)` is `-inf`, `math.pow(10.0, 400.0)`
is `inf`. The float operators already follow this rule (`1.0 / 0.0` is
`inf`), so a formula ported from another language computes the same values
here. This is an exception to §2.9's "a bug is
a runtime error". A domain error depends on the data, and a server that
computes a square root of a request value must not stop on a negative one. A
program that cares tests the result with `isNaN` or `isInfinite`. Nothing
else will: NaN is not equal to anything, itself included, and every
operation on a NaN answers NaN. `print` writes the three as `nan`, `inf` and
`-inf` (§6.1).

The float functions take a `float`. An integer literal is an `int` (§1.6),
so `math.sqrt(4)` is a compile error and the call is `math.sqrt(4.0)`. A
`float32` is accepted wherever a `float` is, as under any operator.

`toInt` and `toFloat` are the two conversions `as` refuses. §3.6 refuses
`as` between the integers and the floats because such a conversion has two
questions in it: how to round, and what to do with a value no integer holds.
These two functions are where each is answered. `toFloat(n)` gives the
nearest float to n, exactly for any integer up to 2^53 in magnitude. It
takes either integer family: a `uint` at the top of its range converts as the
large positive value it is. `toInt(x)` truncates toward zero (`toInt(2.9)`
is 2 and `toInt(-2.9)` is -2). It is a runtime error (§5.5) for NaN, an
infinity, or a value outside the range of an `int`. A float with no int in it
is a fact about the program rather than about the outside world, and the
program had `floor`, `ceil` and `round` to name the rounding it wanted first.

`abs`, `min`, `max` and `clamp` take any number type and answer the widened
type their arguments give under `+` (§2.1). `math.min(a, b)` on two `byte`s
is a `uint`, and `math.abs(x)` on an `int8` is an `int`. The arguments must
share a family, as an operator's must: `math.max(n, 1.5)` on an `int` n is a
compile error, as `n + 1.5` is, and `math.toFloat` or `math.toInt` is the way
across. A bare literal takes the family of the value beside it, so
`math.max(u, 0)` on a `uint` is fine. `abs` of the lowest signed value wraps
to itself, as `-x` does. `min` and `max` of a number and a NaN answer the
number, as IEEE 754's minNum and maxNum do.

```
import 'math';

float side = math.sqrt(3.0 * 3.0 + 4.0 * 4.0);
print(side);                                   // 5
print(math.round(2.5), math.floor(-2.5));      // 3 -3
print(math.clamp(15, 0, 10));                  // 10
print(math.toInt(side) + 1);                   // 6
print(math.isNaN(math.sqrt(-1.0)));            // true
```

### 7.1 Not planned: decisions, not deferrals

Everything above is missing and wanted. The items here are missing and
settled. They are recorded so that they are not argued again. A proposal to
add one has to argue against the reason given, not only point out that the
feature is absent.

**Generic user code.** A function written in Nio takes concrete types, and
this will not change. The `array` and `map` functions are generic because
the compiler implements them; nothing in the language expresses what they
do. For the same reason, `test` (§6.13) has one expectation per type rather
than one for all of them.

Type parameters are the largest single source of complexity in a type
checker and in the diagnostics it produces. Inference failures are the
errors users find hardest to read in every language that has them. Type
parameters also force a permanent choice between monomorphization (code
size, compile time) and boxing (a uniform representation, which §5.7 packing
exists to avoid). Nio covers the same needs in other ways. The compiler
implements the few operations that are truly independent of the element
type. `sealed` and `union` (§2.4, §2.11) cover the closed-set case with
exhaustiveness checking, and a record of closures covers the open one. What
is given up is user-written container libraries. That is a real cost, and
an accepted one.

**Certificate revocation.** `x509` (§6.19) does not consult CRLs or OCSP.
See the [x509 guide](https://nio-lang.org/docs/stdlib/x509) for the reasoning.
In short: it is the usual default for TLS libraries; soft-fail checking
does not stop an attacker who is already positioned to present a revoked
certificate; and shorter certificate lifetimes close the same window without
a network round trip.

**A TLS server, and signing of any kind.** `crypto` (§6.17) has verification
primitives and no signing primitives, and `tls` (§6.18) is a client. These
two facts are one decision.

**Threads, and every other form of parallelism inside one program.** Async
tasks interleave at await points on one thread (§5.2). They are concurrent,
but never simultaneous. There are no threads, no worker pool, no shared
memory between concurrent code, no atomics and no channels, and none of
these is planned. (Child processes are the exception, and not one Nio
creates: `process.child.run` (§6.12) starts a separate program, and several
of those do run at the same time. The program's own code never does.)

Parallelism in Nio is process-level. Run several copies of the program.
Where they must share an entry point, share the listening socket rather than
the state: `SO_REUSEPORT` lets several processes accept on one address, and
a supervisor restarts them. Each process has its own heap, its own collector
and its own scheduler, and they exchange nothing except through the
operating system.

This is the permanent answer, not an interim one, because the alternative
costs the language its simplest guarantee: no data race is expressible in
Nio. That is why §5.2 is short and complete, and why the collector can be
precise and non-moving with no synchronization anywhere in it. Threads over
a shared heap would require a synchronizing collector, atomics, and a memory
model. That is a large amount of subtlety in exchange for a property most
programs get from processes at no cost.

What is given up is real. A single Nio process uses one core, so shared
in-process state across cores (a shared cache, parallel work inside one
request) is not available. A program that needs it needs an external store,
or another language. This is a chosen limit, not an unimplemented feature.

---


### 6.22 `random`

A pseudo-random generator: seedable, so a run can be repeated, and seeded
from the operating system when a program never seeds it, so runs differ.

| Function | Signature | Description |
|---|---|---|
| `random.seed(n)` | `int → void` | Replaces the state; what follows is a function of n. |
| `random.int(lo, hi)` | `(int, int) → int` | A value in [lo, hi], each equally likely. |
| `random.float()` | `→ float` | A value in [0, 1). |
| `random.shuffle(a)` | `T[] → void` | Reorders a in place, every order equally likely. |
| `random.pick(a)` | `T[] → T` | One element of a, each equally likely. |

It is not a source of secrets. Do not base a security decision on it. The
whole stream follows from 256 bits of state that a seed sets. Anyone who
learns the state or guesses the seed predicts every value after it. A token,
a key, a nonce, or anything else whose unpredictability matters comes from
`crypto.randomBytes` (§6.17), which never leaves the kernel's generator. The
two exist side by side so that a program has a seedable generator for tests
and simulations and an unpredictable one for secrets. A program then never
needs to seed the first from the clock to make it stand in for the second.

**Seeding.** `random.seed(n)` replaces the state with one derived from n
(through SplitMix64, as the generator's authors recommend), so the values
that follow are a function of n alone: the same seed gives the same sequence
on every run and every machine. A program that never seeds has the state set
from the operating system's generator on the first call, so its runs differ.
There is no seeding from the clock, and no way to read the state back. The
generator is xoshiro256++, and there is one state for the program: Nio has no
threads (§3.5), so nothing races on it.

`random.int(lo, hi)` includes both bounds and gives each value the same
chance. The bias a `%` introduces is rejected once, inside the module (a
multiply-and-reject, after Lemire), rather than by every caller. `lo` above
`hi` is a runtime error (§5.5), since such a range holds nothing; `lo == hi`
answers that value. The whole range of an `int` is permitted and answers
every value. `random.float()` carries 53 random bits, the precision of a
`float`.

`shuffle` and `pick` take any array, like the functions of `array` (§6.5),
and neither allocates. `shuffle` swaps elements in place at the array's own
width (§5.7), a Fisher-Yates walk from the end, and `pick` reads one element.
`pick` on an empty array is a runtime error.

```
import 'random';

random.seed(7);
print(random.int(1, 6));            // 1, on every run
random.seed(7);
print(random.int(1, 6));            // 1

String[] names = ["ann", "bo", "cy"];
random.shuffle(names);
print(random.pick(names));
```

## Appendix A: Grammar

EBNF; `{ }` is repetition, `[ ]` is option, `|` is alternation.

```
program        = { import-decl } { statement } ;

import-decl    = "import" string-literal [ "as" identifier ] terminator ;

statement      = union-decl
               | type-decl
               | enum-decl
               | function-decl
               | extern-decl
               | native-stmt
               | var-decl
               | assign-stmt
               | incdec-stmt
               | if-stmt
               | switch-stmt
               | while-stmt
               | for-stmt
               | foreach-stmt
               | break-stmt
               | continue-stmt
               | return-stmt
               | expr-stmt ;

type-decl      = [ "export" ] [ "sealed" ] "type" identifier
                 [ extends-clause ] type-body ;   (* "sealed": §2.4 *)
union-decl     = [ "export" ] "union" identifier
                 "{" [ union-member { "," union-member } [ "," ] ] "}" ;
union-member   = type [ identifier ] ;   (* §2.11; the tag is required unless
                              the type is a single capitalized name *)
extends-clause = "extends" identifier [ "." identifier ] ;   (* §2.4 *)
type-body      = "{" { member } "}" ;
member         = field-def ( ";" | &"}" ) | method-decl [ ";" ] ;
field-def      = field-type field-name [ string-literal ] ;
                              (* 'json:key' renames; 'json*' is the catch-all, §6.3 *)
                              (* the string is "target:key"; see §2.4 *)
method-decl    = ret-type { "async" | "override" } identifier
                 "(" [ param { "," param } ] ")" block ;   (* §2.4 *)
                              (* each modifier at most once, any order *)
record-body    = "{" [ field-def { ";" field-def } [ ";" ] ] "}" ;
                              (* an anonymous record type: fields only *)
field-type     = ( base-type | record-body ) { "[" [ int-literal ] "]" | "?" } ;

enum-decl      = [ "export" ] "enum" identifier
                 "{" [ enum-member { "," enum-member } [ "," ] ] "}" ;
enum-member    = identifier ":" [ "-" ] int-literal ;

function-decl  = [ "export" ] ret-type [ "async" ] identifier
                 "(" [ param { "," param } ] ")" block ;
param          = [ "..." ] type identifier ;
                              (* "..." only on the last parameter; §5.2 *)

extern-decl    = "extern" ret-type identifier
                 "(" [ param { "," param } ] ")" terminator ;   (* §5.8 *)
native-stmt    = "native" ( "source" string-literal
                          | "flags" [ identifier ] string-literal )
                 terminator ;   (* §5.8; the identifier names a platform *)

var-decl       = [ "export" ] type [ "const" ] identifier [ "=" expression ]
                 terminator ;    (* "const" requires the "=" initializer *)
assign-stmt    = target "=" expression terminator ;
incdec-stmt    = target ( "++" | "--" ) terminator ;
target         = identifier
               | postfix-expr "[" expression "]"
               | postfix-expr "." identifier ;

if-stmt        = "if" "(" expression ")" block
                 [ "else" ( block | if-stmt ) ] ;
switch-stmt    = "switch" "(" expression ")" "{" { switch-clause } "}" ;
switch-clause  = ( "case" case-label | "case" type-label | "default" )
                 ":" { statement } ;
                              (* a clause with no statements runs the next
                                 one's; "default" comes last, §4.9 *)
type-label     = identifier { "." identifier } identifier ;
                              (* 1 to 3 dotted parts: a type, `mod.Type`,
                                 `Union.Tag`, or `mod.Union.Tag` (§2.11) *)
                              (* a type extending the sealed subject's, then
                                 the name it is bound to; §4.9. The binding
                                 is what tells this from a case-label. *)
case-label     = [ "-" ] ( int-literal | float-literal ) | string-literal
               | "true" | "false" | "null"
               | [ identifier "." ] identifier "." identifier ;
                              (* an enum member, module-qualified or not, §2.5 *)
while-stmt     = "while" "(" expression ")" block ;
for-stmt       = "for" "(" [ for-init ] ";" expression ";" [ simple-stmt ] ")"
                 block ;
for-init       = type [ "const" ] identifier [ "=" expression ]
               | simple-stmt ;
simple-stmt    = target "=" expression
               | target ( "++" | "--" )
               | expression ;
foreach-stmt   = "forEach" "(" expression "," identifier
                 [ "," identifier ] ")" block ;
break-stmt     = "break" terminator ;
continue-stmt  = "continue" terminator ;
return-stmt    = "return" [ expression ] terminator ;
expr-stmt      = expression terminator ;
block          = "{" { statement } "}" ;

terminator     = ";" ;        (* optional after a statement ending in "}" *)

type           = base-type { "[" [ int-literal ] "]" | "?" } ;
                              (* "??" is rejected; the size must be positive *)
ret-type       = type | "void" ;   (* the return slot of a function, §5.2 *)
base-type      = "int" | "int8" | "int16" | "int32" | "int64"
               | "uint" | "uint8" | "byte" | "uint16" | "uint32" | "uint64"
               | "float" | "float32" | "float64"
               | "String" | "bool" | "DateTime" | "Duration" | "Json"
               | "RegExp" | "Socket" | "Listener"
               | func-type | future-type
               | map-type | identifier | identifier "." identifier ;
func-type      = "Function" "(" [ ftype { "," ftype } ] ")"
                 "<" ( type | "void" ) [ "!" ] ">" ;   (* "!": fallible, §2.9 *)
ftype          = [ "..." ] type ;   (* "..." only on the last one; §5.2 *)
future-type    = "Future" "<" ( type | "void" ) [ "!" ] ">" ;
map-type       = "Map" "<" type "," type ">" ;

expression     = catch-expr ;
catch-expr     = or-expr { "catch" ( identifier block | or-expr ) } ;  (* §3.7 *)
or-expr        = and-expr { "||" and-expr } ;
and-expr       = eq-expr { "&&" eq-expr } ;
eq-expr        = rel-expr { ( "==" | "!=" ) rel-expr } ;
rel-expr       = add-expr { ( "<" | ">" | "<=" | ">=" ) add-expr } ;
add-expr       = mul-expr { ( "+" | "-" ) mul-expr } ;
mul-expr       = unary-expr { ( "*" | "/" | "%" ) unary-expr } ;
unary-expr     = ( "-" | "!" | "await" ) unary-expr | as-expr ;
as-expr        = postfix-expr [ [ "strict" ] "as" type ] ;
                              (* json.parse or a Json; "strict" is contextual, §3.6 *)
postfix-expr   = primary { "(" [ args ] ")" | "[" expression "]"
                         | "." field-name | "?." field-name } ;
field-name     = identifier | keyword ;            (* §1.4 *)
args           = expression { "," expression } ;
primary        = int-literal | float-literal | string-literal
               | "true" | "false" | "null"
               | identifier
               | "(" expression ")"
               | array-literal
               | record-literal
               | map-literal
               | func-lit ;
func-lit       = ret-type "(" [ param { "," param } ] ")" "->"
                 ( expression | block ) ;   (* param may be variadic, §5.2 *)
array-literal  = "[" [ expression { "," expression } [ "," ] ] "]" ;
record-literal = "{" [ record-field { "," record-field } [ "," ] ] "}" ;
record-field   = field-name ":" expression ;
map-literal    = "{" map-entry { "," map-entry } [ "," ] "}" ;
                              (* "{}" parses as an empty record literal and
                                 reads as a map or record by the expected
                                 type, §2.8 *)
map-entry      = map-key ":" expression ;
map-key        = string-literal | [ "-" ] ( int-literal | float-literal ) ;
```
