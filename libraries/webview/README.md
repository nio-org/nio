# webview

A native window that renders web content with the platform's own engine. It
was the first external library to use §5.8 native interop. `webview.nio` is
the API, `native/webview_native.m` is the platform body, and a program reaches
both with one ordinary import:

```
import './libraries/webview/webview';

void main() {
    webview.Window w = webview.create("hello", 900, 600) catch e {
        print(e.message);
        return;
    };
    webview.setPageBridge(w, true) catch e { return; };
    webview.loadHtml(w, PAGE) catch e { return; };
    webview.onPageMessage(w, "hello", void (String body) -> {
        print("the page says: " + body);    // window.nio.send('hello', '...')
    });
    webview.run(w);
}
main();
```

**What it is.** One window, a UI layer and a content view, and a two-way
bridge. `loadHtml` or `navigate` decides what the window shows, and
`evalJs`/`uiEval` speak into the pages. A page speaks back with
`window.nio.send('channel', body)`, which is routed to the handler the program
registered with `onMessage` (UI) or `onPageMessage` (content). There is one
handler per channel, and `run(w)` is the whole main loop.

The content view has no bridge until `setPageBridge(w, true)` turns it on. The
example above, one window whose HTML is the app, needs it. A program that
navigates to whatever the user typed leaves it off. Otherwise every site it
visits (and every third-party frame inside one) holds a channel into the host.
The UI layer always has the bridge, because it is the program's own page.

A bare `window.nio.postMessage("...")` lands on the `"message"` channel. A
handler that raises is reported, and the loop keeps going. The queue primitives
underneath (`step`/`takeMessage`/`dispatch`) stay exported for programs that
want their own loop.

One export has no window at all. `resourcesPath()` answers the bundle's
`Contents/Resources` when the program runs from a packaged `.app` (made by
`nio package`), and `""` when it runs bare. This is how an app finds the assets
that packaging copied into the bundle, while a development run keeps reading
the working tree. The test is the Info.plist, not the shape of the path: only a
real bundle has an identifier.

The engine is the platform's own (WKWebView through Cocoa on macOS). So the
library adds no rendering code, and a program that uses it stays a few hundred
kilobytes. Rendering, JavaScript, networking and TLS inside the view belong to
the operating system.

**The loop is a poll, not callbacks.** The language cannot yet give C a Nio
function to call back. So `step` pumps the platform's event loop
for a bounded slice and answers whether the window is still open, and the
program's main loop is `while (webview.step(w, 1000))`. The timeout is not
latency: a queued message wakes the pump at once. The native side wakes the
event loop in each platform's own way: a synthetic `NSApplicationDefined`
event on macOS, and `PostMessage(WM_APP)` and `g_main_context_wakeup` for
WebView2 and GTK when those bodies are written. So the number only sets how
often an idle program wakes. When `step` answers false, the user closed the
window. That is an event, not an error.

**The UI is one page with a hole in it** (browser literature calls this the
chrome). `setUi(w, html)` gives the window a second web view. It fills the
window underneath the content view and draws every piece of UI (top bar,
sidebar, status bar) as one HTML document. So the cost is one extra web
process, however complex the UI is.

The UI page measures the hole it leaves and sends the four numbers on an
`insets` channel. The program forwards them to `setContentInsets`, which
places the content view over the hole. Insets survive window resizing
natively, so the message recurs only when the UI's layout changes (a sidebar
collapsing, not a window being dragged).

UI messages arrive on their own queue (`onMessage` handlers, with
`takeUiMessage` underneath), kept apart from the content page's queue. The UI
is the program's trusted page, and a content page is whatever the network
sent, so commands must never be taken from the content queue. `uiEval` is the
other direction: Nio updating the tab strip or the address box. A window that
never calls `setUi` is just a content view that fills the window.

The layering can flip for a moment. `setUiFront(w, true)` raises the UI page
above the content views, so that a popover or a menu drawn by the chrome can
float over the page. The UI view is transparent wherever its page paints
nothing, so the page stays visible underneath. (A UI page that wants this
paints its regions and leaves its hole transparent.) While raised, the UI page
takes every mouse event, which is how a popover works: the first click outside
dismisses it. So the program lowers the layer again (`false`) when the popover
closes.

The two layers also share the mouse between them, because AppKit delivers
hover traffic by rectangle and not by what is visibly on top. Whichever web
view is covered at the mouse's position stops handling mouse-moved and cursor
updates. So when you hover a link in the page, you see one steady pointer, and
the covered layer's arrow does not flicker against it, in both layer orders.

**Platforms.** macOS is implemented (Cocoa + WebKit; the `native flags darwin`
line carries the frameworks). Windows (WebView2) and Linux (WebKitGTK) are
stubs behind the same `nw_*` surface. `webview.create` raises
`ErrorCode.UNSUPPORTED` there today. A real body fits into the preprocessor
branches of `webview_native.m` with no change to the Nio side.

**The window can use the UI as its titlebar.** `setInlineTitlebar(w)` makes
the title bar transparent, with full-size content (the macOS unified look). The
traffic lights float over the UI page's own top bar, vertically centered as
Safari's are. The library ships no styling for this. `titlebarMetrics(w)`
reports where the platform drew its buttons (right edge, vertical center, in
the page's CSS pixels). What the program's bar does with that (an inset, a bar
height) is the program's decision, based on measurement and not a guess.

The bar must also provide dragging, because a web view takes the mouse. The
page sends a channel message on mousedown in its draggable area, and the
handler calls `startDrag(w)`. `zoom(w)` is the titlebar double-click, and it is
the only one such a window takes. AppKit's own titlebar double-click zoom is
refused there (`windowShouldZoom:`). The transparent strip sits over whatever
the page drew in it, a text input as much as an empty bar, and a double-click
to select all must not toggle the window. The page reports the gesture from its
bar's own background, as it does for the drag. On platforms without the look,
the calls raise and the window keeps its ordinary title bar.

**The program can open the system's emoji palette.** `emojiPicker(w)` brings
the platform's character palette to the front (the Ctrl-Cmd-Space panel on
macOS). It types into whichever text field has focus, so the program focuses
its input first and then asks. The library only opens the panel; after that,
the OS owns it. It raises where the platform has no such panel.

**The application outlives any window.** These calls are for programs whose
windows come and go:

- `pump(ms)` runs the shared event loop, tied to no window. It is the loop's
  blocking step; after it, poll each open window with a zero budget.
- `addDockMenuItem(id, title)` puts an entry in the Dock icon's right-click
  menu. Adding an id again changes its title in place.
- `addDockMenuSeparator(id)` adds a separator line under an id of its own.
  Adding it again does nothing.
- `removeDockMenuItem(id)` removes either kind again, which a window list in
  that menu needs when a window closes. The system's own window-list/Options
  section always shows below the program's block, behind its own separator.
- `takeAppEvent()` drains the app-level queue that these report on.

The app-level queue carries:

- `"dock:<id>"` for a Dock menu pick.
- `"menu:<id>"` for a main-menu item picked while no window is open. With a
  key window, the pick arrives on that window's `__menu` channel instead,
  because the responder chain stops there. So this event is exactly the
  zero-window case, which keeps a New Window item selectable when the last
  window is gone.
- `"reopen"` for a click on the Dock icon. The click is reported and not
  handled. The library suppresses the platform's default, which would raise
  every window the app has, and the program answers. `focusWindow(w)` does the
  whole bring-up gesture (deminiaturize, order front, make key, activate) on
  the one window the program chooses. `lastActiveWindow()` says which window
  the user was in most recently (as a `Window.handle`, 0 when none is open).
  A program with no window opens one.

What any of this means is the program's policy; the library only carries the
request. The wake rule is the loop's usual one: a queued message or app event
ends a blocked pump at once.

**A URL from another application arrives on the same queue**, as
`"open:<url>"`. This covers a link the user clicks outside the program when the
platform sends that scheme here (the default browser's case), and a document
opened with the program. There is one event per URL, so a group stays a group.
As for `"reopen"`, the library reports the URL and does nothing else. The
program decides which window takes a URL, whether it must open a window for it,
and whether it accepts the URL at all. Becoming the handler for a scheme or a
document type is a packaging matter, not an API one (the bundle's
`CFBundleURLTypes` / `CFBundleDocumentTypes`). The library carries what the
platform delivers. A URL that arrives while the program starts waits on the
queue, so a start from a click loses nothing.

**The content view can identify as the platform's browser.**
`setUserAgent(w, ua)` replaces the User-Agent that the content view sends. The
engine's default names the host app, and sign-in pages check for that to refuse
"embedded" browsers. The caller chooses the string (the library ships no
default), and an empty string restores the engine's own. The UI view is not
changed.

`setUserAgentRule(w, host, ua)` adds a per-host exception for sites that enable
a feature by the browser's name and not by what the engine can do. A
main-frame navigation to that host or a subdomain of it sends the rule's
string, and the tab goes back to the window's string when it leaves. The check
runs in the navigation policy. The string must be set before the request goes
out, so a navigation that crosses the boundary is cancelled and issued again
once, including link clicks and redirects. The longest matching rule host wins.
An empty `ua` removes the rule.

**A window can hold tabs.** `newTab(w)` creates one more content view
(hidden), `showTab(w, id)` decides which one is on screen, and
`closeTab(w, id)` destroys a hidden one. Closing a tab releases the engine
resources its page held, which is the purpose of unloading a background tab.
Every per-content call (navigate, evalJs, history, the insets) acts on the tab
currently shown, so a program without tabs never sees the feature. The window
opens with tab 1. Insets and the user agent follow tabs automatically. Closing
the shown tab is refused, so the window always has a content view. Which tabs
stay live is the program's policy, not the library's. nion keeps the active tab
plus a few recent ones and unloads the rest to its session file.

`focusTab(w)` gives the keyboard to the shown tab's page (it becomes the
window's first responder, so Space pauses the video the user just switched to).
It is a separate call and not a side effect of `showTab`, because whether a
switch implies focus is policy. A browser focuses the page it switched to, but
on a new blank tab the next step is to type into the program's own chrome.

**The history's edges are available.** `canBack(w)`/`canForward(w)` answer
whether the shown view's session history has anywhere to go, which is what a
browser uses to grey out its arrows. Read them after the moments when the
answer can change (a `__nav`, a tab switch). The engine owns the history, so the
answer at those events is exact. `back`/`forward` at an edge do nothing, as a
toolbar expects.

**Unloading can be soft, and the library reports sound.** `tabState(w, id)`
answers an opaque snapshot of everything the engine holds for a tab: its
back/forward list, scroll positions, and form data (WKWebView's
`interactionState`). `tabRestore(w, id, state)` gives a snapshot to a tab that
has not navigated yet, in place of its first navigation. So after an unload,
returning to the tab brings the page back where it was, and does not reload its
URL from scratch. This is best-effort: `""` (nothing to give) and a snapshot the
engine no longer recognizes both mean "navigate instead".

`tabAudible(w, id)` answers whether the tab is making sound now. Safari draws
its tab speaker icon from the same fact, and a rule such as "audible tabs don't
sleep" needs it. The rule itself is the program's.

`tabCaptureState(w, id)` answers whether the tab holds the camera or the
microphone: `"none"`, `"camera"`, `"microphone"` or `"both"`. Muted devices
count, since the engine keeps the device session either way. A recording
indicator draws from this, and a rule such as "tabs in a call don't sleep"
needs it. It is always best-effort, because WebKit has the fact but no public
API for it; an engine that cannot say raises UNSUPPORTED. It is different from
`tabMediaState(w, id)`, which answers the public question:
`"none"`/`"playing"`/`"paused"`/`"suspended"`. There, muted autoplay counts as
`"playing"`. A homepage video carousel "plays" silently, so a sleep policy based
on `tabMediaState` would exempt half the web. State and restore need macOS 12.

**The keyboard belongs to one layer at a time.** `focusTab(w)` gives it to the
shown page, and `focusUi(w)` gives it to the UI layer. A program that opens a
text box in its own chrome must call `focusUi`. A DOM `focus()` moves the caret
inside the UI page only: while a content view holds the window's first
responder, the keys still go to the site and the box does not respond.
`uiFocused(w)` answers which side has the keyboard, so a program can check
instead of assuming.

`setBackspaceNavigation(w, false)` turns off the engine's "Backspace goes back"
shortcut, which WebKit applies outside text fields. Every major browser removed
it, because it discards what the user typed. The library keeps the engine's
default and lets the program decide.

**A page can be searched with the engine's own find.**
`find(w, tab, text, flags, maxCount)` runs the search that the platform's
browser uses for its find bar. It highlights the matches and scrolls to the one
it lands on. `clearFind(w, tab)` removes the highlights. The flags are
`FIND_BACKWARDS`, `FIND_CASE_SENSITIVE` and `FIND_WRAP`, combined with `+`.
Searching for the same text again steps to the next match, so one call serves
both typing and a next/previous button.

The answer comes back on `__find`, not from the call, because the engine counts
in the web process. The body is `"<tab>,<count>,<index>"`. The index is
zero-based, and `-1` when the search found nothing. A count of `-1` means the
page holds more matches than `maxCount` asked to count. The match was still
found and shown; only the total is refused and not guessed. No match reads as
count `0`, so there is no separate "not found" case.

The engine does the search, not an injected script, and this matters. It
matches text split across elements, reaches into subframes (including
cross-origin ones that no script can touch), skips what CSS has hidden, and
folds case the way the platform does. A count from anything else would disagree
with the highlights on screen. On WebKit this is SPI, since the public
`findString:` answers only "found or not" with no count. Every call is guarded,
so a backend that cannot search raises at once and does not fail silently later.

**Navigations report themselves.** Reserved channels arrive on the trusted (UI)
queue without any page sending them; the native side pushes them:

- `__nav` ("tab,url"): a tab committed a main-frame document. This covers a
  link click as much as a `navigate` call, and a reload reports its unchanged
  url too.
- `__navinpage` ("tab,url"): the tab's URL changed without a document
  committing. This is a `history.pushState`/`replaceState` route or a
  `#fragment` jump, observed from the engine's URL property the way `__title`
  is. The two kinds use two channels because a browser handles them
  differently. An in-page change keeps the document, so the title, icon and
  content on screen are still current, while a commit replaces them. Chrome's
  extension API makes the same distinction with `onHistoryStateUpdated`
  against `onCommitted`. Only same-origin changes are reported here, because an
  in-page navigation can never cross an origin. A cross-origin change waits for
  its commit, so a slow load cannot put an address that was never fetched in an
  address bar. A same-origin document load announces its url here early, from
  its provisional value, and then commits on `__nav`. Keep the last url and
  compare, as the repeated url of a reload on `__nav` already requires.
- `__http` ("tab,status"): the HTTP status of the document the tab is about to
  render. It is pushed just before that document's `__nav`, for the main frame
  only, and never for a response that became a download. A commit with no
  `__http` before it had no HTTP response at all: a `file://` page, or a back or
  forward from the page cache. What a status means is the host's decision. An
  empty 502 body renders as a blank page, and only the host knows whether to
  leave it or draw something of its own.
- `__title` ("tab,title"): observed through KVO, so a page that changes its own
  title is caught too.
- `__load` ("tab,1" / "tab,0"): the tab started and stopped loading. Finish,
  failure and `stopLoading(w)` all change the one observed flag. So the pair is
  what a UI needs to turn a reload button into ✕, and `stopLoading` is the call
  the ✕ makes.
- `__favicon` ("tab,url"): where the loaded page says its icon is (its icon
  link, else `/favicon.ico`).
- `__newtab`/`__newtabbg` ("tab,url"): the engine made a tab because a page or
  a gesture asked for one.

Page script cannot reach the UI queue, so a handler for these channels reads
the engine, never an attacker. The favicon URL is still the page's own claim,
so a consumer checks its scheme before it loads it. The address bar and a
session file stay correct about where each tab is.

**A page can ask for a window, and the answer is a tab.** `window.open`, a
`target="_blank"` link, and an extension page that opens a login flow all
arrive at the same place, WebKit's UI delegate. A host that does not implement
it drops every one of them silently, which is why "that link does nothing"
happens. The library makes the tab itself (it cannot defer: a view must be
returned at once) and reports it on `__newtab`. The engine is already loading
the URL into it, so a handler adopts the tab and shows it, but must not
navigate it.

A gesture can ask too. A middle-click (any auxiliary button) or a Cmd+click on
a link would otherwise be an ordinary in-place navigation in the engine, which
is why a browser can seem to open a tab only sometimes. The library takes it
over the same way. The engine's own "Open Link/Image/Frame in New Window"
context items arrive at the same delegate (the chosen item is wrapped, so the
library knows what it is). The takeover does not apply to a view that has never
navigated. The first load of an engine-made view repeats the gesture that asked
for it (the synthesized action still carries the click's button and
modifiers), and taking that over again would make a second view. One gesture is
one tab.

Gesture-opened tabs arrive on `__newtabbg` instead. Who asked is a fact the
program cannot recover later, and browsers treat the two cases differently. A
page-opened window goes on screen (an auth popup must be visible). A
gesture-opened tab goes behind the page being read. Shift with the click is the
usual request for the foreground, and lands on `__newtab`. Either way, one
handler pair decides what a new tab does.

The other end of that life is `window.close()`. The engine honours it only in
the windows that script opened (the tabs that `__newtab` reported), and reports
it on `__closed` ("tab,") without acting, since only the program knows what its
session shows next. Auth popups are why both halves exist. The sign-in flow
opens a window, posts its result to the opener, and closes itself. A host that
ignores the close leaves a dead tab behind.

The same delegate makes `alert`/`confirm`/`prompt` work (a page whose `alert()`
never returns is a hung page). They appear as sheets on the window, as does the
file panel behind `<input type="file">` (multiple selection and
`webkitdirectory` are honoured; without the delegate method the click does
nothing). The user's choice in that panel is the whole decision, so no request
reaches the program.

**Files download themselves.** The engine decides when a load is a download:
an anchor's `download` attribute, a response the view cannot render, a
`Content-Disposition: attachment` header, or its own context menu's "Download
Image". That last one is delivered through a WebKit delegate SPI (macOS 12+),
since the public API only hands over downloads decided by policy; without it
those picks do nothing. The library handles the download:

- The file lands in the download directory: the platform's Downloads folder,
  unless `setDownloadDir(w, dir)` sets another. `""` restores the default, and
  `downloadDir(w)` answers the directory in effect.
- The file gets the server's suggested name, made unique in the `name (2).ext`
  style, since WKDownload refuses an existing destination.
- The window keeps every download it has seen (the session's download
  history), and `downloads(w)` answers it whole. The byte counts of an active
  transfer are read from its `NSProgress` at the moment of the call.
- `__download` events on the trusted queue (`"<id>,<json>"`) mark the moments
  worth waking for: started, name decided, finished, failed, cancelled.
  Progress between them is the caller's poll, so the queue never fills with
  progress events.
- `cancelDownload(w, id)` stops one. What already reached the disk stays, and
  is marked cancelled.

Two companions with no window serve the UI a browser draws over this.
`openPath(p)` opens a path as a double-click in the file browser would (for a
click on a finished row). `revealPath(p)` shows a file selected in its folder,
or opens a directory as a folder window (for the folder icons).

A download survives its tab: the native side holds the view it came from until
the transfer ends, so a browser's memory policy can unload the tab without
stopping the transfer. This needs macOS 11.3+. On an older engine nothing
becomes a download, and `downloads` answers the empty list.

**Tabs can carry profiles.** `setProfile(w, name)` decides which profile's
storage (cookies, logins, site data) the tabs created after it use: a persistent
store per name, or the platform default for `""`. A view keeps the store it was
created with, so a profile switcher creates new tabs and closes the old ones.

`deleteProfile(w, name)` asks the engine to remove a named profile's storage.
Call it only after that profile's views are closed and the window has moved to
another store, since the removal is asynchronous inside the engine, and the
engine may refuse a store that is still in use. The library reports that the
request was placed, not that the data is gone. Both calls need macOS 14+.

`setIncognito(w)` moves the window onto the engine's non-persistent store
instead. Cookies, local and session storage, and cache live in RAM, start
empty, and end with the window (closing it releases the store; nothing reaches
disk). Otherwise the rule is the same as for `setProfile`: only tabs created
after the call use it, so replace the built-in view as a named profile's setup
does. The extension world is rebuilt as non-persistent with it, so
`chrome.storage` is as temporary as the site data. It does not need macOS 14.
There is no way back on the same window.

**The content view's context menu is extensible.**
`addContextMenuItem(w, id, title)` adds an item after the engine's own entries
in the content view's right-click menu. A pick reports on the `__ctx` channel
("tab,id", another reserved channel on the trusted queue, beside
`__nav`/`__title`/`__favicon`), and what the item does is up to the program's
handler. Items accumulate in the order they are added and apply to every tab.

The engine's own items can be changed in two ways, both program policy passed
as data:

- `retitleContextMenuItem(w, id, title)` renames an engine item by the
  identifier WebKit puts on it (the `"WKMenuItemIdentifierOpenLinkInNewWindow"`
  family). The usual case: a host that adopts engine-made views as tabs makes
  that item say "Open Link in New Tab", which is what a pick does there.
- `hideContextMenuItem(w, id)` removes one. nion removes "Download Linked
  File", since a link that the server answers as a file downloads through the
  response policy on a plain click anyway.

An identifier the engine never uses never matches, in both calls.

A third call takes the pick instead of the label.
`captureContextMenuItem(w, id)` leaves the item where the engine put it, shown
when the engine shows it, and reports a pick on `__ctx` with the identifier as
the id. The engine's own action never runs. This exists because some of those
actions leave the program. "Search with Google"
(`"WKMenuItemIdentifierSearchWeb"`) hands the selection to the system's
web-search service, which opens the default browser. So a host that is itself a
browser would see its own right-click open another application, and neither a
rename nor a removal can prevent that. When the item is captured, the pick
belongs to the program. nion reads the selection back with `evalJsResult`,
opens its own preferred search engine in a tab, and renames the item to match.

Two companions serve the menu's usual users. `printPage(w)` opens the
platform's print panel for the shown page (as a sheet; the ordinary event pump
drives it; macOS 11+). `setDevTools(w, on)` enables the engine's developer
tools on every content view, current and future, which also adds "Inspect
Element" to the menu. That opens WebKit's inspector, the same one Safari shows.
An attached inspector docks inside the content view's own area. Each view sits
in a wrapper sized by the insets, and WebKit reframes against that wrapper, so
the inspector never spreads over the UI layer's sidebar. The UI view gets none
of this, because its menu is not something the program's users see.

**A window can refuse to shrink.** `setMinSize(w, width, height)` sets the
smallest content size the user may resize the window to, in the same CSS pixels
`create` takes. A window that is already smaller grows to the minimum at once.
What the minimum should be (enough for a sidebar, a toolbar's worth of buttons)
depends on the program's layout, so the library only enforces the numbers it is
given.

**The app can have a real menu and a chosen appearance.** `setAppMenu(w)`
installs the application menu:

- Settings… (Cmd+,)
- Reload (Cmd+R)
- a View menu with Zoom In / Zoom Out / Actual Size (Cmd+Plus, Cmd+Minus,
  Cmd+0; Cmd+Equals also means Zoom In)
- Quit
- a standard Edit menu, which makes Cmd+C/V/X/A work in text fields, web views
  included

The items that report use the `__menu` channel (`"0,settings"`, `"0,reload"`,
`"0,zoomin"`, `"0,zoomout"`, `"0,zoomreset"`). They have no target and resolve
through the responder chain, so with several windows the event lands on the
focused window's queue. The program decides what a settings surface is, what
reload does to a session, and which tab a zoom applies to.
`tabZoom(w, id, percent)` is the call that answers the zoom items: one tab's
page zoom in percent (25..500; WKWebView's `pageZoom`, macOS 11+, layout zoom so
text reflows). It is per tab, so that per-tab, per-site or per-window zoom is
the program's choice.

`addAppMenuItem(w, menu, id, title, key)` adds one item to the named top-level
menu. The menu is created at the end of the bar when it does not exist; `File`
already exists, empty, in its usual place after the app menu. A pick reports as
`"0,<id>"` on the same `__menu` channel, or as `"menu:<id>"` on the app queue
when no window is open (the zero-window case above). `key` is the Cmd shortcut
(`""` for none; a capital letter means Cmd+Shift, so `"T"` is ⇧⌘T). Adding an id
again changes its title and does not duplicate it (as in the Dock menu). What
the item means is the program's decision, as with Settings.
`addAppMenuSeparator(w, menu, id)` adds a separator line the same way. Its id
means that running a window's menu setup again does nothing, and does not stack
up lines. Call both after `setAppMenu`, which installs the menu bar they extend.

`setAppearance(w, mode)` forces a window to light or dark, or makes it follow
the system. Every web view in the window inherits it, so a forced appearance is
also what the pages' `prefers-color-scheme` answers.

**The program answers a page's permission requests** (macOS 12+). When a page
calls `getUserMedia`, WebKit waits until the embedder decides. The library
turns that decision into its usual poll-and-queue shape. A `__perm` event
arrives on the trusted queue, with body `"<tab>,<json>"` and
`{ req, origin, kind }`:

- `origin` is as the engine states it (a page cannot fake it).
- `kind` is one of `"camera"`, `"microphone"`, `"camera+microphone"`,
  `"motion"`, `"screen"`.

The program answers with `permReply(w, req, answer)`, where the answer is
`"grant"`, `"deny"`, or `"prompt"`. `"prompt"` passes the decision to the
engine's own built-in popup (its buttons and memory are the engine's; the
program never learns the choice). Every request must be answered in the end:
the page's call waits until an answer arrives, and a window that closes declines
what is left. The engine asks again on every page load, so any per-site memory
is the program's. A remembered decision is just a `permReply` sent without
showing anything.

The app, not the library, must meet two conditions:

- The Info.plist must carry `NSCameraUsageDescription` /
  `NSMicrophoneUsageDescription`. Without them, WebKit hides
  `navigator.mediaDevices` from every page. A bare binary run outside a bundle
  is exempt, so development runs work, and packaged apps fail if the keys are
  forgotten.
- macOS itself asks for app-level consent once, in its own dialog, on the
  first real capture.

**Screen sharing uses the same protocol, with kind `"screen"`** (macOS 13+).
The delegate hook is WebKit SPI, like the find count and the backspace switch,
so an older engine denies the call itself. A `getDisplayMedia` request reports
on `__perm` like a capture request, but a `"grant"` shows nothing by itself. It
lets the site open the system's picker, where the user chooses the screen or
window to share, or cancels. The picker is a second consent, so a program may
choose never to remember an allow for this kind. `"prompt"` also leads to the
picker, since the picker is the engine's own UI here. On macOS 13 and 14 the app
must also have the OS's Screen Recording consent (System Settings → Privacy &
Security; for a development run, the terminal or IDE that started the program is
the responsible app). On macOS 15 the picker carries the consent itself.

**Geolocation is the one permission the library must emulate.** WebKit has no
public hook for `navigator.geolocation` on macOS. `setGeolocation(w, true)` is
the opt-in; call it once, right after `create`, before anything loads. It works
as follows:

- A page-world `WKUserScript` replaces the API with a shim (main frame only).
- Each request reaches native code on a dedicated script-message handler that
  carries the frame's true origin (`WKFrameInfo.securityOrigin`; a page cannot
  claim another page's origin).
- The request arrives on `__perm` with kind `"geolocation"`, like a capture
  request.
- On `"grant"`, one shared `CLLocationManager` supplies positions. One-shot
  requests are answered and forgotten. `watchPosition` receives positions until
  `clearWatch`. The requested accuracy is the highest that any open request
  wants.

`"prompt"` means deny for this kind, because there is no engine popup behind it.
The shim handles what the specification defines against the page's own clock
and cache: `timeout`, `maximumAge`, and the refusal outside a secure context.
None of these reach native code. The same two app-level conditions apply
(`NSLocationWhenInUseUsageDescription`, and the OS's own consent dialog on the
first real position).

**A window can host web extensions** (macOS 15.4+). `extLoad(w, path)` loads a
Safari Web Extension into Safari's own WebExtensions engine (`WKWebExtension`).
The path can be an unpacked directory, a `.zip` of one, a `.appex`, or a
container `.app` (the appex inside is found by its extension point). The library
hosts the engine by mapping tabs, windows, popups and permissions onto what the
window already has. Content scripts, background pages and MV3 service workers,
`storage`, `declarativeNetRequest` and popups all work. Extensions attach to the
content views only, never to the UI layer. Their storage follows the window's
profile, so `setProfile` unloads them, and the program loads again what the new
profile wants.

Five more reserved channels arrive on the trusted queue, with body
`"<ext>,<json>"`:

- `__extloaded`: the extension is running.
- `__extaction`: redraw its toolbar button. `extIcon` answers the image as a
  data: URI.
- `__extperm`: a permission request at run time. Answer
  `extReply(w, req, "grant")` or `"deny"`.
- `__extpopup`: its popup was presented, in a native popover anchored at the
  rectangle that `extActionClick(w, id, x, y, wd, ht)` measured from the UI page
  (the same measure-and-forward method as the insets). A second click while
  that popup is still open dismisses it, the toggle every browser's pinned
  icon has.
- `__extnewtab`: the extension wants a tab or a window (`tabs.create`,
  `window.open`, `windows.create`, which is one window here). Make one, reply
  with its id (or `""` to refuse), and navigate it to the requested URL. When
  that URL is under the extension's own `Extension.url`, make the tab with
  `extNewTab`, since only a view built from the extension's own configuration
  can load it. NordPass's vault page arrives this way.

The decisions use request ids, because every WKWebExtension delegate decision
takes a completion handler. The native side keeps the completion, and the
program answers through `extReply`: the poll-and-queue form of a callback.

The permission policy for this version, to be revisited when a real permission
UI exists: `extLoad` grants everything the manifest requests, and
`extSetPermission` narrows it afterwards. `extensions(w)` lists what is loaded.
`extUnload` stops one (its storage stays; unloading is not uninstalling).

Two companions with no window serve installation:

- `installedExtensions()` scans `/Applications` and `~/Applications` for apps
  that carry a Safari web extension (the App Store route: the user installs the
  container app, and the browser finds it). It answers `{ path, name }` rows,
  and `extLoad` takes the path as it is.
- `pickPath()` opens the platform's file panel for a folder, `.zip`, `.appex`
  or `.app` (`""` when cancelled).

Native messaging answers with a clear error, so extensions that need a
companion desktop app fail visibly and do not hang. Standalone extensions are
the target.

**Extension pages and web pages are on opposite sides of one line, and the
library carries navigations across it.** An extension's pages load only in a
view built from its own configuration. WebKit refuses, silently, to replace
such a page with a web page. So a login flow that gives its own app page a web
URL hangs forever with nothing in any log. (NordPass does this with
`globalThis.open(url, '_self')` for the OAuth redirect it just got from its
server.) So the library takes over a crossing in either direction: it cancels
it where it was requested, starts it in a tab built the right way, and reports
it on `__newtab` like any engine-made tab. `Extension.url` is the base that an
extension's own pages are under, so it is what tells one of its URLs from a web
URL.

**An extension's identity is stable per (profile, extension path)**, in all
three places WebKit keeps one:

- The scheme is Safari's own (`safari-web-extension:`), not the
  `webkit-extension:` that WebKit uses for an embedder. An extension's server
  sees that scheme in the redirect URI a login gives it, and rejects the second
  one ("Invalid redirect uri", and the flow stops there).
- The host is a deterministic hash of the profile name and the extension path,
  so the origin's own site data (the localStorage a password manager keeps its
  session in) survives a restart.
- The context's `uniqueIdentifier`, which `browser.runtime.id` answers and by
  which WebKit keys `chrome.storage`, is set to that same host. Its default is
  new on each launch, even under an overridden base URL, and would give back an
  empty `chrome.storage.local` at every restart while the origin's localStorage
  survived.

With a reused origin, WebKit treats the extension as already installed and
never runs its MV3 service worker again. The library avoids this by loading the
background content explicitly at every load. `__extloaded` fires when the
background is actually running.

**What it does not do yet.** A navigation is decided only where a view could not
carry it (the extension-page crossing and the download decision above). There is
no policy hook a program can use to block or redirect an ordinary load; that
would be a natural extension of the same native surface. Downloads do not
resume: a failed transfer starts again from the beginning. Extensions cannot
activate or close tabs (the program alone decides which tab is on screen), tabs
the program has unloaded are invisible to them, and content scripts do not
inject into `file:` pages. That last limit is WebKit's own, and Safari has it
too.

`nion/`, a separate repository, holds the working examples. `hello.nio` is the
bridge working in both directions in a single view. `browser.nio` is the UI
layer in use: top bar, collapsible sidebar, bookmarks, and the insets protocol
connected end to end. Both take `--smoke` to open, pump, and close.
