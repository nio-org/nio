// The Swift half of the translation library: Apple's Translation framework
// (macOS 15+) behind a plain C surface. build.sh compiles it into
// libNioTranslation.dylib, which the glue dlopens rather than links, so a
// build made without swiftc still runs and answers "unavailable".
//
// A TranslationSession cannot be constructed. Only a SwiftUI view receives
// one, through .translationTask when its configuration changes. So this shim
// keeps an invisible one-pixel window holding that view, and every job is a
// row in a table the task's closure drains.
//
// Every entry point is main-thread only. The Nio side keeps that contract
// because its event loop is the main thread, and assumeIsolated checks it.

import AppKit
import Foundation
import NaturalLanguage
import SwiftUI

#if canImport(Translation)
import Translation
#endif

// One asked-for piece of work. `state` is 0 running, 1 done, 2 failed, and
// `result` is one string whatever the job was.
//
// Kinds 0 and 1 need a TranslationSession and go through the host window.
// Higher kinds do not. Give a new kind that needs no session a number above 1.
final class NioTrJob {
    let source: String
    let target: String
    let texts: [String]
    let kind: Int  // 0 translate, 1 prepare, 2 availability, 3 languages
    var state: Int32 = 0
    var result: String = ""
    var queued = true

    init(source: String, target: String, texts: [String], kind: Int) {
        self.source = source
        self.target = target
        self.texts = texts
        self.kind = kind
    }
}

@MainActor
final class NioTrJobs {
    static let shared = NioTrJobs()
    var nextId: Int64 = 0
    var jobs: [Int64: NioTrJob] = [:]

    func add(_ job: NioTrJob) -> Int64 {
        nextId += 1
        jobs[nextId] = job
        return nextId
    }
}

#if canImport(Translation)

// A host owns one invisible window, one SwiftUI view and one session at a
// time. There are two hosts because prepareTranslation() waits for the user
// to answer a system sheet, and that wait has no bound. Kind 1 gets its own
// host, so a translation after an abandoned download still starts.
@available(macOS 15.0, *)
@MainActor
final class NioTrHost: ObservableObject {
    static let shared = NioTrHost(kind: 0)
    static let downloads = NioTrHost(kind: 1)

    let kind: Int
    @Published var config: TranslationSession.Configuration?
    var pairSource = ""
    var pairTarget = ""
    var running = false
    var window: NSWindow?

    init(kind: Int) {
        self.kind = kind
    }

    static func pumpAll() {
        shared.pump()
        downloads.pump()
    }

    // The window must exist for the framework to serve the view, and must
    // stay transparent, behind, and deaf to the mouse so no user meets it.
    func ensureWindow() {
        if window != nil {
            return
        }
        _ = NSApplication.shared
        let win = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 2, height: 2),
            styleMask: [.borderless], backing: .buffered, defer: false)
        win.isReleasedWhenClosed = false
        win.alphaValue = 0
        win.ignoresMouseEvents = true
        win.collectionBehavior = [.transient, .ignoresCycle]
        win.contentView = NSHostingView(rootView: NioTrView(host: self))
        win.orderBack(nil)
        window = win
    }

    // The system's download dialog presents from the session's view. A dialog
    // anchored to a window parked behind everything is one nobody can answer,
    // so front the host window for the length of a prepare. The window itself
    // stays borderless and clear, and only the system prompt shows.
    func presentForConsent() {
        guard let win = window, let screen = NSScreen.main else {
            return
        }
        let f = screen.visibleFrame
        win.setFrameOrigin(NSPoint(x: f.midX, y: f.midY))
        win.alphaValue = 1
        win.orderFrontRegardless()
    }

    func hideAgain() {
        guard let win = window else {
            return
        }
        win.alphaValue = 0
        win.orderBack(nil)
    }

    // Starts the runner when it is idle and a session job waits. The task
    // fires on a configuration change only, so the same pair needs an
    // invalidate and a new pair needs a fresh configuration.
    func pump() {
        if running {
            return
        }
        guard let job = nextSessionJob() else {
            return
        }
        ensureWindow()
        running = true
        if config != nil && job.source == pairSource && job.target == pairTarget {
            config?.invalidate()
        } else {
            pairSource = job.source
            pairTarget = job.target
            let src = job.source.isEmpty ? nil : Locale.Language(identifier: job.source)
            config = TranslationSession.Configuration(
                source: src, target: Locale.Language(identifier: job.target))
        }
    }

    func nextSessionJob() -> NioTrJob? {
        // Oldest first, so answers keep the order of asks.
        for id in NioTrJobs.shared.jobs.keys.sorted() {
            let job = NioTrJobs.shared.jobs[id]!
            if job.queued && job.kind == kind {
                return job
            }
        }
        return nil
    }

    // Drains every queued job of the session's pair.
    //
    // Each translation checks availability first. A session asked to
    // translate a pair whose models are absent does not throw. It waits on a
    // consent prompt that the invisible window cannot show. The check turns
    // that hang into a failed job that names prepare.
    func run(session: TranslationSession) async {
        while let job = nextMatching() {
            job.queued = false
            do {
                if job.kind == 1 {
                    presentForConsent()
                    defer { hideAgain() }
                    try await session.prepareTranslation()
                    job.result = "prepared"
                } else {
                    var src = job.source
                    if src.isEmpty {
                        let recognizer = NLLanguageRecognizer()
                        recognizer.processString(job.texts.joined(separator: "\n"))
                        src = recognizer.dominantLanguage?.rawValue ?? ""
                    }
                    if !src.isEmpty {
                        let status = await LanguageAvailability().status(
                            from: Locale.Language(identifier: src),
                            to: Locale.Language(identifier: job.target))
                        if status == .unsupported {
                            job.result = "cannot translate " + src + " to " + job.target
                            job.state = 2
                            continue
                        }
                        if status == .supported {
                            job.result = "the " + src + " to " + job.target
                                + " models are not installed; prepare first"
                            job.state = 2
                            continue
                        }
                    }
                    var requests: [TranslationSession.Request] = []
                    for (i, t) in job.texts.enumerated() {
                        requests.append(
                            TranslationSession.Request(sourceText: t, clientIdentifier: String(i)))
                    }
                    let responses = try await session.translations(from: requests)
                    var out = job.texts
                    for r in responses {
                        if let id = r.clientIdentifier, let i = Int(id), i >= 0, i < out.count {
                            out[i] = r.targetText
                        }
                    }
                    job.result = NioTr.jsonArray(out)
                }
                job.state = 1
            } catch is CancellationError {
                job.queued = true
                break
            } catch {
                job.result = error.localizedDescription
                job.state = 2
            }
        }
        running = false
        NioTrHost.pumpAll()
    }

    func nextMatching() -> NioTrJob? {
        for id in NioTrJobs.shared.jobs.keys.sorted() {
            let job = NioTrJobs.shared.jobs[id]!
            if job.queued && job.kind == kind && job.source == pairSource && job.target == pairTarget {
                return job
            }
        }
        return nil
    }
}

@available(macOS 15.0, *)
struct NioTrView: View {
    @ObservedObject var host: NioTrHost

    var body: some View {
        Color.clear
            .translationTask(host.config) { session in
                await host.run(session: session)
            }
    }
}

#endif

enum NioTr {
    static func jsonArray(_ texts: [String]) -> String {
        let data = (try? JSONEncoder().encode(texts)) ?? Data("[]".utf8)
        return String(data: data, encoding: .utf8) ?? "[]"
    }

    static func parseArray(_ json: String) -> [String]? {
        return try? JSONDecoder().decode([String].self, from: Data(json.utf8))
    }

    static func cString(_ s: String) -> UnsafeMutablePointer<CChar> {
        return strdup(s) ?? strdup("")!
    }

    static func take(_ p: UnsafePointer<CChar>?) -> String {
        guard let p = p else {
            return ""
        }
        return String(cString: p)
    }
}

// The C surface the glue dlsyms. Call on the main thread only. Strings are
// UTF-8. Every char* answer is malloc'd and must go back to niotr_free.

@_cdecl("niotr_supported")
public func niotr_supported() -> Int32 {
    if #available(macOS 15.0, *) {
        #if canImport(Translation)
        return 1
        #else
        return 0
        #endif
    }
    return 0
}

// Answers the user's own language as a BCP-47 tag. minimalIdentifier drops a
// region that says nothing ("en-US" -> "en") and keeps a script that does
// ("zh-Hans").
@_cdecl("niotr_syslang")
public func niotr_syslang() -> UnsafeMutablePointer<CChar> {
    return NioTr.cString(Locale.current.language.minimalIdentifier)
}

// Answers the dominant language of the text as a BCP-47 tag, or "" when the
// recognizer cannot say. It is synchronous: NaturalLanguage needs no session
// and no download.
@_cdecl("niotr_detect")
public func niotr_detect(_ text: UnsafePointer<CChar>?) -> UnsafeMutablePointer<CChar> {
    let s = NioTr.take(text)
    let recognizer = NLLanguageRecognizer()
    recognizer.processString(s)
    guard let lang = recognizer.dominantLanguage else {
        return NioTr.cString("")
    }
    return NioTr.cString(lang.rawValue)
}

// Starts a translation job. `textsJson` is a JSON array of strings, and a
// `source` of "" means auto-detect. Answers the job id, -1 on an unsupported
// OS, or -2 on bad arguments.
@_cdecl("niotr_start")
public func niotr_start(
    _ source: UnsafePointer<CChar>?, _ target: UnsafePointer<CChar>?,
    _ textsJson: UnsafePointer<CChar>?
) -> Int64 {
    guard #available(macOS 15.0, *) else {
        return -1
    }
    #if canImport(Translation)
    let tgt = NioTr.take(target)
    guard let texts = NioTr.parseArray(NioTr.take(textsJson)), !tgt.isEmpty else {
        return -2
    }
    return MainActor.assumeIsolated {
        let job = NioTrJob(source: NioTr.take(source), target: tgt, texts: texts, kind: 0)
        let id = NioTrJobs.shared.add(job)
        NioTrHost.pumpAll()
        return id
    }
    #else
    return -1
    #endif
}

// Starts a job that asks the system to make the pair usable. It shows the
// download prompt when the models are absent, and answers "prepared".
@_cdecl("niotr_prepare")
public func niotr_prepare(
    _ source: UnsafePointer<CChar>?, _ target: UnsafePointer<CChar>?
) -> Int64 {
    guard #available(macOS 15.0, *) else {
        return -1
    }
    #if canImport(Translation)
    let tgt = NioTr.take(target)
    if tgt.isEmpty {
        return -2
    }
    return MainActor.assumeIsolated {
        let job = NioTrJob(source: NioTr.take(source), target: tgt, texts: [], kind: 1)
        let id = NioTrJobs.shared.add(job)
        NioTrHost.pumpAll()
        return id
    }
    #else
    return -1
    #endif
}

// Starts a job that answers "installed" (ready now), "supported" (prepare
// first), or "unsupported". It needs no session and never uses the window.
@_cdecl("niotr_avail")
public func niotr_avail(
    _ source: UnsafePointer<CChar>?, _ target: UnsafePointer<CChar>?
) -> Int64 {
    guard #available(macOS 15.0, *) else {
        return -1
    }
    #if canImport(Translation)
    let src = NioTr.take(source)
    let tgt = NioTr.take(target)
    if tgt.isEmpty {
        return -2
    }
    return MainActor.assumeIsolated {
        let job = NioTrJob(source: src, target: tgt, texts: [], kind: 2)
        let id = NioTrJobs.shared.add(job)
        Task { @MainActor in
            let availability = LanguageAvailability()
            let from = src.isEmpty ? nil : Locale.Language(identifier: src)
            let to = Locale.Language(identifier: tgt)
            let status: LanguageAvailability.Status
            if let from = from {
                status = await availability.status(from: from, to: to)
            } else {
                // With no source, ask whether any pair into the target works.
                status = await availability.status(
                    from: Locale.Language(identifier: "en"), to: to)
            }
            switch status {
            case .installed:
                job.result = "installed"
            case .supported:
                job.result = "supported"
            case .unsupported:
                job.result = "unsupported"
            @unknown default:
                job.result = "unsupported"
            }
            job.state = 1
        }
        return id
    }
    #else
    return -1
    #endif
}

// Starts a job that answers a JSON array of the BCP-47 tags the framework can
// translate. minimalIdentifier drops the region, so "en-US" and "en-GB" both
// become "en", and the list holds no duplicates. Each tag works as a source
// or a target. The job needs no session.
@_cdecl("niotr_langs")
public func niotr_langs() -> Int64 {
    guard #available(macOS 15.0, *) else {
        return -1
    }
    #if canImport(Translation)
    return MainActor.assumeIsolated {
        let job = NioTrJob(source: "", target: "", texts: [], kind: 3)
        let id = NioTrJobs.shared.add(job)
        Task { @MainActor in
            var tags: [String] = []
            var seen = Set<String>()
            for language in await LanguageAvailability().supportedLanguages {
                let tag = language.minimalIdentifier
                if !tag.isEmpty && seen.insert(tag).inserted {
                    tags.append(tag)
                }
            }
            job.result = NioTr.jsonArray(tags.sorted())
            job.state = 1
        }
        return id
    }
    #else
    return -1
    #endif
}

// Answers the name of a tag in the user's own language, or "" when the system
// has no name for it. It uses Foundation only, so it answers on macOS 14 too.
@_cdecl("niotr_langname")
public func niotr_langname(_ tag: UnsafePointer<CChar>?) -> UnsafeMutablePointer<CChar> {
    let t = NioTr.take(tag)
    if t.isEmpty {
        return NioTr.cString("")
    }
    return NioTr.cString(Locale.current.localizedString(forIdentifier: t) ?? "")
}

// Answers 0 running, 1 done, 2 failed, or -3 for no such job.
@_cdecl("niotr_poll")
public func niotr_poll(_ id: Int64) -> Int32 {
    return MainActor.assumeIsolated {
        guard let job = NioTrJobs.shared.jobs[id] else {
            return -3
        }
        return job.state
    }
}

// Answers a settled job's result and forgets the job. The caller must poll
// first, because taking a running job answers "".
@_cdecl("niotr_take")
public func niotr_take(_ id: Int64) -> UnsafeMutablePointer<CChar> {
    let result: String = MainActor.assumeIsolated {
        guard let job = NioTrJobs.shared.jobs[id], job.state != 0 else {
            return ""
        }
        NioTrJobs.shared.jobs[id] = nil
        return job.result
    }
    return NioTr.cString(result)
}

@_cdecl("niotr_free")
public func niotr_free(_ p: UnsafeMutablePointer<CChar>?) {
    free(p)
}
