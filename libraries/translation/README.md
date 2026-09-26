# libraries/translation

On-device text translation through the platform's own engine: Apple's
Translation framework on macOS (15+), the same models Safari translates with.
It is free, offline once the models for a language pair are downloaded, and
private: the text never leaves the machine. `translation.nio` is the API and
has the usage example. This file is the map of the pieces and of the one build
step.

## The pieces

```
translation.nio             the API: load, detect, supportedLanguages/languageName,
                            translate/prepare/availability jobs, poll/take -- a
                            poll-loop surface like webview's
native/translation_glue.c   what clang compiles (`native source`): a dlopen
                            bridge to the Swift dylib, plus honest stubs
native/NioTranslation.swift the engine binding: sessions, jobs, the invisible
                            SwiftUI host window the framework requires
build.sh                    swiftc → native/libNioTranslation.dylib
```

## Why a dylib and not `native source`

Apple's Translation framework speaks only Swift, not C or Objective-C.
`native source` compiles C and Objective-C with clang, and clang is the only
toolchain the language requires. So `sh build.sh` builds the Swift half once
(the only time swiftc is needed) into a dylib. `translation.load()` opens that
dylib at run time from a path the program supplies:

- Programs that import this library still build with clang alone, everywhere.
  On a machine without swiftc, or without the dylib, the program runs and
  translation answers "unavailable".
- A packaged app ships the dylib in its resources and loads it from
  `webview.resourcesPath()`. A development run loads it from this directory.

The header comment of `translation_glue.c` has the full reasoning.

## Why jobs and a poll loop

The framework is asynchronous and needs the platform's event loop to run. A
`TranslationSession` is received by a SwiftUI view and is never constructed. So
the shim keeps an invisible one-pixel window whose only job is to receive
sessions, and every request (translate, prepare, availability) is a job that
the program polls between iterations of its own loop. A GUI program already has
this shape (`webview.step`/`pump`). The consumer this library was built for,
nion's "Translate Page" (`nion/translate.nio`, in a separate repository), is
the model.

## The language list

`supportedLanguages()` is a job that answers the BCP-47 tags the engine has
(21 on macOS 26 today), and `languageName(tag)` names one in the user's own
language. A picker needs these two and nothing else. The consumer has no table
that can fall out of date when the platform adds a language, and never offers a
tag that this machine cannot translate. The tags are reduced to the form a
caller passes back (`en-US` and `en-GB` both arrive as `en`), so the list is
short and every entry works when passed to `translate`. Ask once per run: the
list is a property of the system, not of a document.

## Model downloads

`availability(source, target)` answers `installed`, `supported` (available to
download) or `unsupported`. `prepare(source, target)` asks the system to fetch
the models, behind the system's own consent prompt. That is the only time the
shim's window becomes presentable, since the prompt is anchored to it. A
translate job on a pair that is not installed fails at once with "not
installed" and does not hang. This is the shim's own guard: the framework itself
would wait forever on a consent sheet that nobody can see.

**Ask `availability` before you translate, and call `prepare` only when the
user asks for it.** `prepareTranslation()` waits on that sheet, for minutes or
forever. A consumer that calls it without a request from the user puts a modal
download in front of someone who did not ask for one.

For the same reason, the shim keeps two session hosts: one for translate jobs
and one for prepare jobs. A session host runs one job at a time, so with a
single host an abandoned download would block every later translation. Kind 1
(prepare) has its own host, and kind 0 (translate) keeps running.
