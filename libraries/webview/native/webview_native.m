// The native half of libraries/webview (§5.8): one window that holds the
// platform's web engine. The Cocoa/WebKit body compiles under __APPLE__.
// Elsewhere nw_create answers NW_ERR_UNSUPPORTED, so the Nio wrapper raises
// instead of failing to link.
//
// Contract with webview.nio:
//   - a window is a positive int64 handle; other nw_create results are NW_ERR_* codes
//   - nw_step pumps the event loop for at most timeout_ms; it answers 0 after
//     the window closes, 1 while it is alive
//   - a page calls window.nio.postMessage("..."); the strings queue in C
//     memory and nw_take_message hands them over one at a time
//
// GC contract (runtime.h): Str* parameters are read before anything
// allocates, and nw_take_message builds its result last, so no function here
// needs a GCFrame. The message queue holds malloc'd copies, not Str*.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

// nw_create error results; webview.nio maps each to an Error code.
#define NW_ERR_INVALID (-1)
#define NW_ERR_BAD_INPUT (-2)
#define NW_ERR_FULL (-3)
#define NW_ERR_UNSUPPORTED (-100)

// Flags for nw_find, exported by webview.nio as FIND_*. Programs write
// these numbers, so they must not change when a backend changes.
#define NW_FIND_BACKWARDS (1 << 0)
#define NW_FIND_CASE_SENSITIVE (1 << 1)
#define NW_FIND_WRAP (1 << 2)

typedef struct NwMsg {
    struct NwMsg *next;
    int64_t len;
    char *data;
} NwMsg;

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <CoreLocation/CoreLocation.h>
#import <objc/runtime.h>

#define NW_MAX_WINDOWS 64

// Web extensions (WKWebExtension, macOS 15.4+). Without the SDK the nw_ext_*
// calls answer NW_ERR_UNSUPPORTED.
#if defined(__MAC_15_4) && MAC_OS_X_VERSION_MAX_ALLOWED >= 150400
#define NW_HAS_WEBEXT 1
#endif

// Tab snapshots and media state (interactionState and
// requestMediaPlaybackState:, macOS 12+). Without the SDK the calls answer
// NW_ERR_UNSUPPORTED.
#if defined(__MAC_12_0) && MAC_OS_X_VERSION_MAX_ALLOWED >= 120000
#define NW_HAS_TABSTATE 1
#endif

// Site permission asks (macOS 12+). Without the SDK the delegate methods are
// not compiled and WebKit falls back to its own built-in prompt.
#if defined(__MAC_12_0) && MAC_OS_X_VERSION_MAX_ALLOWED >= 120000
#define NW_HAS_MEDIAPERM 1
#endif

// Downloads (WKDownload, macOS 11.3+). Without the SDK the calls answer
// NW_ERR_UNSUPPORTED and nw_downloads answers "[]".
#if defined(__MAC_11_3) && MAC_OS_X_VERSION_MAX_ALLOWED >= 110300
#define NW_HAS_DOWNLOAD 1
#endif

#define NW_MAX_EXTS 32

@class NioWebHelper;
@class NioContentView;
@class NioExtTab;
@class NioExtWindow;
@class NioExtDelegate;

#ifdef NW_HAS_WEBEXT
// One loaded extension. Slot i answers handle i+1.
typedef struct {
    int used;
    WKWebExtension *ext;           // retained
    WKWebExtensionContext *ctx;    // retained
} NwExt;
#endif

#ifdef NW_HAS_DOWNLOAD
// One download, active or settled. A settled entry stays listed (the table is
// also the session's download history) and gives its slot up only when the
// table is full. Live byte counts come from the WKDownload's NSProgress.
#define NW_DL_ACTIVE 0
#define NW_DL_DONE 1
#define NW_DL_FAILED 2
#define NW_DL_CANCELLED 3
typedef struct {
    int used;
    int64_t id;            // handed out by the window's dlSeq, never reused
    int state;             // NW_DL_*
    WKDownload *dl;        // retained until the download settles
    // Retained until the download settles: the program may close the tab
    // mid-transfer, and the download must not go down with it.
    WKWebView *origin;
    NSString *file;        // the leaf name on disk; "" until the destination is decided
    NSString *path;        // the full destination path; "" until then
    NSString *url;         // the requested URL
    NSString *error;       // prose, failed downloads only
    int64_t total;         // expected bytes; -1 until the server says
    int64_t received;      // the final count once settled (live reads go through dl)
} NwDl;
#endif

#define NW_MAX_DOWNLOADS 64

// A window layers two web views in one container: `ui` is the program's chrome
// page (NULL until nw_set_ui_html), `webview` the content view on top of it,
// positioned over the hole the UI page leaves. Separate views, separate
// origins: the content page can never touch the chrome.
#define NW_MAX_TABS 128

typedef struct {
    int used;
    int closed;
    NSWindow *window;
    NSView *container;
    WKWebView *webview;            // the active content view (== tabs[active])
    WKWebView *ui;                 // the UI layer, or NULL
    NioWebHelper *helper;          // content bridge + window delegate
    NioWebHelper *uiHelper;
    NwMsg *head;                   // messages from the content page
    NwMsg *tail;
    NwMsg *uhead;                  // messages from the UI page
    NwMsg *utail;
    // Content views; slot i answers handle i+1. Exactly one is visible at a
    // time and w->webview aliases it. Each view sits in a wrap (wraps[i]), and
    // the wrap is what the insets frame and show/hide toggle: WebKit docks the
    // attached Web Inspector into the inspected view's superview and reframes
    // the view to fill it, so the superview must be the hole.
    WKWebView *tabs[NW_MAX_TABS];
    NSView *wraps[NW_MAX_TABS];
    int active;                    // index into tabs
    // Last nw_set_content_insets, replayed onto a tab as it is shown. A hidden
    // tab may have been created after the insets landed.
    int64_t ins[4];                // top, left, bottom, right
    int has_insets;
    // The custom User-Agent, replayed onto tabs created later.
    NSString *ua;                  // retained; nil = engine default
    // Per-host exceptions to it (nw_set_ua_rule): lowercase host -> the
    // string to send there. Consulted at each main-frame navigation, so a
    // tab entering a rule's host takes its string and restores the window's
    // on the way out.
    NSMutableDictionary *uaRules;  // retained
    // The data store new views are created with; nil = the default. An
    // existing view keeps the store it was born with.
    WKWebsiteDataStore *store;     // retained
    // 1 when the store is the non-persistent (incognito) one. The extension
    // controller must then be non-persistent too, or chrome.storage would
    // outlive the session's site data.
    int ephemeral;
    int devtools;                  // nw_set_devtools; replayed onto later tabs
    int noBackspaceNav;            // nw_set_backspace_nav; replayed onto later tabs
    // Whether content views carry the window.nio bridge (the UI layer always
    // does). Off until nw_set_page_bridge; see nw_add_bridge.
    int pageBridge;
    int geoOn;                     // nw_set_geolocation
    // Program context-menu items (nw_context_menu_item): retained
    // { id, title } dictionaries, appended to every content view's menu.
    NSMutableArray *ctxItems;      // retained
    // WebKit item identifier -> replacement title (nw_ctx_retitle).
    NSMutableDictionary *ctxRetitles;    // retained
    // Engine items removed outright (nw_ctx_hide), by identifier.
    NSMutableSet *ctxHides;              // retained
    // Engine items whose pick the program takes over (nw_ctx_capture).
    // willOpenMenu retargets each at the helper, so the engine's own action
    // never runs and the pick reports on "__ctx".
    NSMutableSet *ctxCaptures;           // retained
    // Time of the last pick of an engine "Open ... in New Window" item, 0
    // otherwise. That pick reaches createWebViewWithConfiguration
    // indistinguishable from a window.open, so a view asked for within the
    // beat counts as the user's ask for a tab and reports on "__newtabbg".
    double bgOpenAt;
    // Last nw_set_profile name, for the extension controller's identity.
    NSString *profileName;         // retained; nil = the default profile
    // Download directory (nw_set_download_dir); nil = the platform's own
    // Downloads folder. Not validated: an unwritable directory fails the
    // download at destination time.
    NSString *downloadDir;         // retained
    // 1 while the UI layer is raised above the content views
    // (nw_set_ui_front). Content views read it to drop tracking-driven mouse
    // events, because AppKit delivers those by rect and not by occlusion.
    int uiFront;
    // Inline-titlebar windows refuse AppKit's titlebar double-click zoom
    // (windowShouldZoom:), because the transparent strip sits over the page's
    // top bar. zooming marks nw_zoom's programmatic ask, which still passes.
    int inlineTitlebar;
    int zooming;
    // Delegate decisions parked for the program: request id -> a copied block
    // a reply call invokes (nw_ext_reply, nw_perm_reply). Flushed with ""
    // (declined) at window close and extension teardown, so no WebKit
    // completion is ever dropped.
    NSMutableDictionary *replies;      // retained; NSNumber -> void (^)(NSString *)
    int64_t reqSeq;
#ifdef NW_HAS_DOWNLOAD
    NwDl dls[NW_MAX_DOWNLOADS];
    int64_t dlSeq;                 // download ids; per window, never reused
#endif
#ifdef NW_HAS_WEBEXT
    // One controller, created with the window so every content view carries it
    // from birth. A view injects content scripts only for the controller its
    // configuration named. nw_set_profile rebuilds it, because extension
    // storage is keyed by the profile, like site data.
    WKWebExtensionController *extc;    // retained
    NioExtDelegate *extDelegate;       // retained
    NioExtWindow *extWindow;           // retained; this window, as extensions see it
    NioExtTab *extTabs[NW_MAX_TABS];   // retained; slot-parallel to tabs[]
    NwExt exts[NW_MAX_EXTS];
    // Where the UI page said the clicked action button is (CSS pixels,
    // top-left origin), anchoring the popup popover.
    int64_t extRect[4];                // x, y, w, h
    NSPopover *extPopover;             // retained while a popup is up
    WKWebExtensionAction *extPopoverAction;  // retained; the action that presented it
    int64_t extPopoverExt;             // ...and which extension presented it
#endif
} NwWin;

static NwWin nw_wins[NW_MAX_WINDOWS];

// The window the user was last in: set by windowDidBecomeMain, cleared by that
// window's close. nw_last_active answers it.
static NwWin *nw_last_main;

static NwWin *nw_get(int64_t h) {
    if (h < 1 || h > NW_MAX_WINDOWS) return NULL;
    NwWin *w = &nw_wins[h - 1];
    return w->used ? w : NULL;
}

static int64_t nw_handle_of(NwWin *w) {
    return (int64_t)(w - nw_wins) + 1;
}

// Wake a blocked nw_step. A page message arrives through a run-loop source and
// not an NSEvent, so nextEventMatchingMask would otherwise sleep out its whole
// deadline with the message already queued.
static void nw_wake(void) {
    NSEvent *ev = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                     location:NSZeroPoint
                                modifierFlags:0
                                    timestamp:0
                                 windowNumber:0
                                      context:nil
                                      subtype:0
                                        data1:0
                                        data2:0];
    [NSApp postEvent:ev atStart:NO];
}

static void nw_push_msg(NwMsg **head, NwMsg **tail, const char *bytes, size_t len) {
    NwMsg *m = malloc(sizeof(NwMsg));
    if (!m) return;
    m->next = NULL;
    m->len = (int64_t)len;
    m->data = malloc(len ? len : 1);
    if (!m->data) { free(m); return; }
    memcpy(m->data, bytes, len);
    if (*tail) (*tail)->next = m; else *head = m;
    *tail = m;
}

static Str *nw_pop_msg(NwMsg **head, NwMsg **tail) {
    NwMsg *m = *head;
    if (!m) return rt_str_alloc(0);
    *head = m->next;
    if (!*head) *tail = NULL;
    Str *out = rt_str_alloc(m->len);
    memcpy(out->data, m->data, (size_t)m->len);
    free(m->data);
    free(m);
    return out;
}

// One helper per web view: the script-message handler JS posts through. isUi
// says which queue its messages join. The content view's helper is also the
// window delegate and the navigation delegate.
#ifdef NW_HAS_WEBEXT
static NioExtTab *nw_ext_tab(NwWin *w, int slot);
#endif

@interface NioWebHelper : NSObject <WKScriptMessageHandler, NSWindowDelegate, WKNavigationDelegate, WKUIDelegate
#ifdef NW_HAS_DOWNLOAD
    , WKDownloadDelegate
#endif
>
@property(assign, nonatomic) NwWin *win;
@property(assign, nonatomic) int isUi;
@end

// ---- find in page ----
//
// WebKit's find function is SPI: the public findString: answers only a BOOL,
// and a find bar that shows "3 of 12" needs the count. Each call below tests
// respondsToSelector: first and answers NW_ERR_INVALID without the function.
enum {
    NW_FIND_OPT_CASE_INSENSITIVE = 1 << 0,
    NW_FIND_OPT_BACKWARDS        = 1 << 3,
    NW_FIND_OPT_WRAP             = 1 << 4,
    NW_FIND_OPT_SHOW_OVERLAY     = 1 << 5,
    NW_FIND_OPT_SHOW_INDICATOR   = 1 << 6,
    NW_FIND_OPT_DETERMINE_INDEX  = 1 << 9,
};

@interface WKWebView (NioFind)
- (void)_findString:(NSString *)string options:(NSUInteger)options maxCount:(NSUInteger)maxCount;
- (void)_hideFindUI;
- (void)_setFindDelegate:(id)delegate;
@end

// Backspace goes back in history. WebKit applies it only outside a text
// field, and the switch is SPI like the find function above.
@interface WKPreferences (NioBackspace)
- (void)_setBackspaceKeyNavigationEnabled:(BOOL)enabled;
@end

static void nw_apply_backspace_nav(WKWebView *wv, int on) {
    if (!wv) return;
    WKPreferences *p = wv.configuration.preferences;
    if (![p respondsToSelector:@selector(_setBackspaceKeyNavigationEnabled:)]) return;
    [p _setBackspaceKeyNavigationEnabled:(on ? YES : NO)];
}

// The tab handle a view answers to, or 0.
static int64_t nw_tab_of(NwWin *w, WKWebView *wv) {
    for (int i = 0; i < NW_MAX_TABS; i++) {
        if (w->tabs[i] == wv) return (int64_t)i + 1;
    }
    return 0;
}

// A library-made event, framed like a page's channel message
// ("<channel>\x01<body>") but pushed onto the UI queue, which page script
// cannot reach. A handler for a "__" channel therefore reads the engine and
// never an attacker.
static void nw_push_event(NwWin *w, const char *channel, int64_t tab, NSString *text) {
    const char *utf8 = text ? text.UTF8String : "";
    if (!utf8) utf8 = "";
    char head[64];
    int n = snprintf(head, sizeof head, "%s\x01%lld,", channel, (long long)tab);
    size_t tlen = strlen(utf8);
    char *buf = malloc((size_t)n + tlen);
    if (!buf) return;
    memcpy(buf, head, (size_t)n);
    memcpy(buf + n, utf8, tlen);
    nw_push_msg(&w->uhead, &w->utail, buf, (size_t)n + tlen);
    free(buf);
    nw_wake();
}

// Park a delegate decision for the program. The block runs when the program
// answers through nw_ext_reply or nw_perm_reply, or at flush time with "".
static int64_t nw_park(NwWin *w, void (^handler)(NSString *)) {
    if (!w->replies) w->replies = [[NSMutableDictionary alloc] init];
    int64_t req = ++w->reqSeq;
    [w->replies setObject:[[handler copy] autorelease] forKey:@(req)];
    return req;
}

// Answer every parked decision with "" (declined) and forget them. Runs at
// window close and at extension teardown, so no WebKit completion is dropped.
static void nw_flush_replies(NwWin *w) {
    if (!w->replies) return;
    for (NSNumber *k in [w->replies allKeys]) {
        void (^cb)(NSString *) = [w->replies objectForKey:k];
        cb(@"");
    }
    [w->replies removeAllObjects];
}

// The string a program keys permissions by: scheme://host, with the port only
// when the origin carries one.
static NSString *nw_origin_string(WKSecurityOrigin *o) {
    if (!o) return @"";
    if (o.port) {
        return [NSString stringWithFormat:@"%@://%@:%ld", o.protocol, o.host, (long)o.port];
    }
    return [NSString stringWithFormat:@"%@://%@", o.protocol, o.host];
}

static NSRect nw_content_frame(NwWin *w);
static void nw_apply_devtools(WKWebView *wv, int on);
static NSString *nw_json(id obj);
static NSString *nw_ua_for(NwWin *w, NSURL *url);

// ---- geolocation ----
//
// WebKit exposes no public hook for navigator.geolocation on macOS, so
// nw_set_geolocation injects a page-world shim (a WKUserScript, main frame
// only). Its asks arrive on the "nioGeo" script-message handler and carry the
// frame's true origin (WKFrameInfo.securityOrigin), which a page cannot spoof.
// They ride the same "__perm" protocol as capture, with kind "geolocation";
// "prompt" is a deny here, because no engine popup sits behind this kind. A
// granted ask reads one shared CLLocationManager, and each fix evaluates the
// shim's deliver or fail entry point in the asking view. The app-level TCC
// consent sits underneath, as it does with capture.

// One outstanding ask: a one-shot (getCurrentPosition) or a watch
// (watchPosition), from one view's page, under the shim's own id. The view is
// retained until the entry clears. A view that stopped being a tab is dropped
// at the next delivery and never misdelivered.
#define NW_MAX_GEO 128
typedef struct {
    int used;
    NwWin *win;
    WKWebView *wv;        // retained
    int64_t jsId;
    int watch;
    int granted;
    int high;
} NwGeo;
static NwGeo nw_geos[NW_MAX_GEO];
static CLLocationManager *nw_loc;
static id nw_loc_delegate;

static int nw_geo_active(void) {
    for (int i = 0; i < NW_MAX_GEO; i++) {
        if (nw_geos[i].used && nw_geos[i].granted) return 1;
    }
    return 0;
}

static void nw_geo_clear(int i) {
    if (!nw_geos[i].used) return;
    [nw_geos[i].wv release];
    memset(&nw_geos[i], 0, sizeof nw_geos[i]);
    if (nw_loc && !nw_geo_active()) [nw_loc stopUpdatingLocation];
}

// Answer one entry's error and forget it. The message is always a library
// constant, so it needs no JS quoting.
static void nw_geo_fail(int i, int code, NSString *msg) {
    NwGeo *g = &nw_geos[i];
    [g->wv evaluateJavaScript:[NSString stringWithFormat:
        @"window.__nioGeo && window.__nioGeo.fail(%lld, %d, '%@')",
        (long long)g->jsId, code, msg] completionHandler:nil];
    nw_geo_clear(i);
}

// A fix for everyone granted: one-shots answered and forgotten, watches kept
// for the next. An invalid CLLocation component (a negative accuracy, course
// or speed) becomes the null the web spec wants.
static void nw_geo_deliver(CLLocation *loc) {
    NSString *alt = @"null";
    NSString *altAcc = @"null";
    if (loc.verticalAccuracy >= 0) {
        alt = [NSString stringWithFormat:@"%.3f", loc.altitude];
        altAcc = [NSString stringWithFormat:@"%.3f", loc.verticalAccuracy];
    }
    NSString *hdg = loc.course >= 0 ? [NSString stringWithFormat:@"%.2f", loc.course] : @"null";
    NSString *spd = loc.speed >= 0 ? [NSString stringWithFormat:@"%.2f", loc.speed] : @"null";
    long long ts = (long long)([loc.timestamp timeIntervalSince1970] * 1000.0);
    for (int i = 0; i < NW_MAX_GEO; i++) {
        NwGeo *g = &nw_geos[i];
        if (!g->used || !g->granted) continue;
        if (!nw_tab_of(g->win, g->wv)) { nw_geo_clear(i); continue; }
        [g->wv evaluateJavaScript:[NSString stringWithFormat:
            @"window.__nioGeo && window.__nioGeo.deliver(%lld, %.8f, %.8f, %.2f, %@, %@, %@, %@, %lld)",
            (long long)g->jsId, loc.coordinate.latitude, loc.coordinate.longitude,
            loc.horizontalAccuracy, alt, altAcc, hdg, spd, ts] completionHandler:nil];
        if (!g->watch) nw_geo_clear(i);
    }
}

@interface NioGeoDelegate : NSObject <CLLocationManagerDelegate>
@end
@implementation NioGeoDelegate
- (void)locationManager:(CLLocationManager *)m didUpdateLocations:(NSArray<CLLocation *> *)locs {
    (void)m;
    CLLocation *loc = locs.lastObject;
    if (loc) nw_geo_deliver(loc);
}
- (void)locationManager:(CLLocationManager *)m didFailWithError:(NSError *)error {
    (void)m;
    // kCLErrorLocationUnknown is not an end: CoreLocation keeps trying.
    if ([error.domain isEqualToString:kCLErrorDomain] && error.code == kCLErrorLocationUnknown) return;
    int denied = [error.domain isEqualToString:kCLErrorDomain] && error.code == kCLErrorDenied;
    for (int i = 0; i < NW_MAX_GEO; i++) {
        if (nw_geos[i].used && nw_geos[i].granted) {
            nw_geo_fail(i, denied ? 1 : 2,
                denied ? @"Location access denied" : @"Position unavailable");
        }
    }
}
@end

// Start or retune the shared manager for whoever is granted. The desired
// accuracy is the strongest any outstanding ask wanted. A cached fix under
// 30 seconds old answers at once, so a page does not wait for the radio.
static void nw_geo_start(void) {
    if (!nw_loc) {
        nw_loc_delegate = [[NioGeoDelegate alloc] init];
        nw_loc = [[CLLocationManager alloc] init];
        nw_loc.delegate = nw_loc_delegate;
        [nw_loc requestWhenInUseAuthorization];
    }
    int high = 0;
    for (int i = 0; i < NW_MAX_GEO; i++) {
        if (nw_geos[i].used && nw_geos[i].granted && nw_geos[i].high) high = 1;
    }
    nw_loc.desiredAccuracy = high ? kCLLocationAccuracyBest : kCLLocationAccuracyHundredMeters;
    [nw_loc startUpdatingLocation];
    CLLocation *cached = nw_loc.location;
    if (cached && [cached.timestamp timeIntervalSinceNow] > -30.0) nw_geo_deliver(cached);
}

// One shim ask off the "nioGeo" handler. get and watch park a "__perm"
// decision, as a capture ask does. clear forgets a watch, or a one-shot the
// shim timed out.
static void nw_geo_message(NwWin *w, WKScriptMessage *msg) {
    if (!w->geoOn) return;
    if (![msg.body isKindOfClass:[NSDictionary class]]) return;
    NSDictionary *d = (NSDictionary *)msg.body;
    NSString *op = [d objectForKey:@"op"];
    NSNumber *jid = [d objectForKey:@"id"];
    if (![op isKindOfClass:[NSString class]] || ![jid isKindOfClass:[NSNumber class]]) return;
    WKWebView *wv = msg.webView;
    int64_t tab = wv ? nw_tab_of(w, wv) : 0;
    if (!tab) return;
    int64_t jsId = [jid longLongValue];
    if ([op isEqualToString:@"clear"]) {
        for (int i = 0; i < NW_MAX_GEO; i++) {
            if (nw_geos[i].used && nw_geos[i].wv == wv && nw_geos[i].jsId == jsId) nw_geo_clear(i);
        }
        return;
    }
    int watch = [op isEqualToString:@"watch"];
    if (!watch && ![op isEqualToString:@"get"]) return;
    int slot = -1;
    for (int i = 0; i < NW_MAX_GEO; i++) {
        if (!nw_geos[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        [wv evaluateJavaScript:[NSString stringWithFormat:
            @"window.__nioGeo && window.__nioGeo.fail(%lld, 2, 'Too many outstanding asks')",
            (long long)jsId] completionHandler:nil];
        return;
    }
    NwGeo *g = &nw_geos[slot];
    g->used = 1;
    g->win = w;
    g->wv = [wv retain];
    g->jsId = jsId;
    g->watch = watch;
    NSNumber *hi = [d objectForKey:@"high"];
    g->high = ([hi isKindOfClass:[NSNumber class]] && [hi intValue]) ? 1 : 0;
    g->granted = 0;
    int64_t req = nw_park(w, ^(NSString *answer) {
        // The entry may have cleared while the program decided, so identity is
        // the slot, the view and the id together.
        NwGeo *gg = &nw_geos[slot];
        if (!gg->used || gg->wv != wv || gg->jsId != jsId) return;
        if ([answer isEqualToString:@"grant"]) {
            gg->granted = 1;
            nw_geo_start();
        } else {
            nw_geo_fail(slot, 1, @"Permission denied");
        }
    });
    nw_push_event(w, "__perm", tab,
        nw_json(@{ @"req": @(req), @"origin": nw_origin_string(msg.frameInfo.securityOrigin),
                   @"kind": @"geolocation" }));
}

// The page-world shim: the whole navigator.geolocation surface over the
// "nioGeo" handler. Timeout, maximumAge and the secure-context refusal are
// answered here and never reach native. Injecting it twice is harmless.
static NSString * const nwGeoShim =
    @"(function () {"
    @" if (window.__nioGeo) return;"
    @" var pend = {}; var nextId = 1; var last = null;"
    @" function post(m) { try { window.webkit.messageHandlers.nioGeo.postMessage(m); } catch (e) {} }"
    @" function mkErr(code, msg) { return { code: code, message: msg,"
    @"  PERMISSION_DENIED: 1, POSITION_UNAVAILABLE: 2, TIMEOUT: 3 }; }"
    @" function drop(id) { var p = pend[id]; if (p && p.timer) clearTimeout(p.timer); delete pend[id]; }"
    @" window.__nioGeo = {"
    @"  deliver: function (id, lat, lon, acc, alt, altAcc, hdg, spd, ts) {"
    @"   var p = pend[id]; if (!p) return;"
    @"   var pos = { coords: { latitude: lat, longitude: lon, accuracy: acc, altitude: alt,"
    @"    altitudeAccuracy: altAcc, heading: hdg, speed: spd }, timestamp: ts };"
    @"   last = pos;"
    @"   if (!p.watch) drop(id);"
    @"   try { p.success(pos); } catch (e) {}"
    @"  },"
    @"  fail: function (id, code, msg) {"
    @"   var p = pend[id]; if (!p) return;"
    @"   if (!p.watch) drop(id);"
    @"   if (p.error) { try { p.error(mkErr(code, msg)); } catch (e) {} }"
    @"  }"
    @" };"
    @" function start(watch, success, error, opts) {"
    @"  opts = opts || {};"
    @"  var id = nextId++;"
    @"  if (!window.isSecureContext) {"
    @"   setTimeout(function () { if (error) { try {"
    @"    error(mkErr(1, 'Geolocation needs a secure context')); } catch (e) {} } }, 0);"
    @"   return watch ? id : undefined;"
    @"  }"
    @"  if (!watch && last && typeof opts.maximumAge === 'number' && opts.maximumAge > 0"
    @"    && (Date.now() - last.timestamp) <= opts.maximumAge) {"
    @"   var c = last;"
    @"   setTimeout(function () { try { success(c); } catch (e) {} }, 0);"
    @"   return undefined;"
    @"  }"
    @"  var p = { success: success, error: error, watch: watch, timer: null };"
    @"  pend[id] = p;"
    @"  if (!watch && typeof opts.timeout === 'number' && isFinite(opts.timeout)) {"
    @"   p.timer = setTimeout(function () { drop(id); post({ op: 'clear', id: id });"
    @"    if (error) { try { error(mkErr(3, 'Timeout')); } catch (e) {} } }, Math.max(0, opts.timeout));"
    @"  }"
    @"  post({ op: watch ? 'watch' : 'get', id: id, high: opts.enableHighAccuracy ? 1 : 0 });"
    @"  return watch ? id : undefined;"
    @" }"
    @" var geo = {"
    @"  getCurrentPosition: function (s, e, o) { if (typeof s === 'function') start(false, s, e, o); },"
    @"  watchPosition: function (s, e, o) { if (typeof s !== 'function') return 0; return start(true, s, e, o); },"
    @"  clearWatch: function (id) { if (pend[id]) { drop(id); post({ op: 'clear', id: id }); } }"
    @" };"
    @" try { Object.defineProperty(navigator, 'geolocation', { value: geo, configurable: false }); } catch (e) {}"
    @"})();";

// Give one configuration the shim and its handler. Content views only, and
// never twice: a configuration derived from an opener's already carries both.
static void nw_add_geo(NwWin *w, WKWebViewConfiguration *cfg, NioWebHelper *helper) {
    if (helper.isUi || !w->geoOn) return;
    for (WKUserScript *s in cfg.userContentController.userScripts) {
        if ([s.source containsString:@"__nioGeo"]) return;
    }
    [cfg.userContentController addScriptMessageHandler:helper name:@"nioGeo"];
    WKUserScript *shim = [[[WKUserScript alloc] initWithSource:nwGeoShim
        injectionTime:WKUserScriptInjectionTimeAtDocumentStart
        forMainFrameOnly:YES] autorelease];
    [cfg.userContentController addUserScript:shim];
}

// ---- downloads (WKDownload, macOS 11.3+) ----
//
// A download is born in the navigation delegate, or in the engine's own
// context menu (_webView:contextMenuDidCreateDownload:), and lives in the
// window's NwDl table. The program reads the table whole as JSON
// (nw_downloads). "__download" events on the trusted queue (body
// "<id>,<json>") mark started, destination decided, finished, failed and
// cancelled. Byte counts come from the download's NSProgress at each poll, so
// the queue carries no progress spam.

static NSString *nw_download_dir_of(NwWin *w) {
    if (w->downloadDir && w->downloadDir.length) return w->downloadDir;
    NSArray *dirs = NSSearchPathForDirectoriesInDomains(NSDownloadsDirectory, NSUserDomainMask, YES);
    if (dirs.count) return dirs[0];
    return NSTemporaryDirectory();
}

#ifdef NW_HAS_DOWNLOAD

static void nw_dl_release(NwDl *d) API_AVAILABLE(macos(11.3)) {
    [d->dl release];
    [d->origin release];
    [d->file release];
    [d->path release];
    [d->url release];
    [d->error release];
    memset(d, 0, sizeof *d);
}

// A free slot, or the oldest settled entry's when the table is full: history
// gives way before an active transfer does. NULL when every slot is active.
static NwDl *nw_dl_slot(NwWin *w) API_AVAILABLE(macos(11.3)) {
    for (int i = 0; i < NW_MAX_DOWNLOADS; i++) {
        if (!w->dls[i].used) return &w->dls[i];
    }
    NwDl *old = NULL;
    for (int i = 0; i < NW_MAX_DOWNLOADS; i++) {
        NwDl *d = &w->dls[i];
        if (d->state != NW_DL_ACTIVE && (!old || d->id < old->id)) old = d;
    }
    if (old) nw_dl_release(old);
    return old;
}

static NwDl *nw_dl_of(NwWin *w, WKDownload *dl) API_AVAILABLE(macos(11.3)) {
    for (int i = 0; i < NW_MAX_DOWNLOADS; i++) {
        if (w->dls[i].used && w->dls[i].dl == dl) return &w->dls[i];
    }
    return NULL;
}

static NwDl *nw_dl_by_id(NwWin *w, int64_t id) API_AVAILABLE(macos(11.3)) {
    for (int i = 0; i < NW_MAX_DOWNLOADS; i++) {
        if (w->dls[i].used && w->dls[i].id == id) return &w->dls[i];
    }
    return NULL;
}

static NSDictionary *nw_dl_dict(NwDl *d) API_AVAILABLE(macos(11.3)) {
    static NSString *const states[] = { @"active", @"done", @"failed", @"cancelled" };
    int64_t received = d->received;
    int64_t total = d->total;
    if (d->state == NW_DL_ACTIVE && d->dl) {
        received = d->dl.progress.completedUnitCount;
        int64_t t = d->dl.progress.totalUnitCount;
        if (t > 0) total = t;
    }
    return @{ @"id": @(d->id),
              @"state": states[d->state],
              @"file": d->file ? d->file : @"",
              @"path": d->path ? d->path : @"",
              @"url": d->url ? d->url : @"",
              @"received": @(received),
              @"total": @(total),
              @"error": d->error ? d->error : @"" };
}

static void nw_dl_event(NwWin *w, NwDl *d) API_AVAILABLE(macos(11.3)) {
    nw_push_event(w, "__download", d->id, nw_json(nw_dl_dict(d)));
}

// A download reached its end. The state guard drops the didFailWithError that
// a cancel also provokes. The final byte count is read before the WKDownload
// and the originating view are released.
static void nw_dl_settle(NwWin *w, NwDl *d, int state, NSString *err) API_AVAILABLE(macos(11.3)) {
    if (d->state != NW_DL_ACTIVE) return;
    d->state = state;
    if (d->dl) {
        d->received = d->dl.progress.completedUnitCount;
        int64_t t = d->dl.progress.totalUnitCount;
        if (t > 0) d->total = t;
    }
    if (err && err.length) {
        [d->error release];
        d->error = [err copy];
    }
    [d->dl release];
    d->dl = nil;
    [d->origin release];
    d->origin = nil;
    nw_dl_event(w, d);
}

#endif  /* NW_HAS_DOWNLOAD */

static WKWebView *nw_new_content_view(NwWin *w, WKWebViewConfiguration *cfg, NSRect frame);
static WKWebView *nw_make_view(NwWin *w, NioWebHelper *helper, NSRect frame);
#ifdef NW_HAS_WEBEXT
static int64_t nw_ext_of_host(NwWin *w, NSString *host);
int64_t nw_ext_tab_new(int64_t h, int64_t ext);
#endif

// The page-side half of the bridge, added to a configuration before any page
// script runs. postMessage(s) sends a bare string. send(channel, body) prefixes
// the channel and a U+0001 separator, which webview.nio's deliver takes apart.
// This file and webview.nio are the only holders of that framing.
//
// The UI layer always has the bridge. A content view gets it only where the
// program asked (nw_set_page_bridge), because the bridge would otherwise give
// every site, and every third-party iframe in one, a channel into the host.
static void nw_add_bridge(NwWin *w, WKWebViewConfiguration *cfg, NioWebHelper *helper) {
    if (!helper.isUi && !w->pageBridge) return;
    // A configuration derived from an opener's already carries the bridge, and
    // a second handler under the same name raises an exception.
    for (WKUserScript *s in cfg.userContentController.userScripts) {
        if ([s.source containsString:@"window.nio"]) return;
    }
    [cfg.userContentController addScriptMessageHandler:helper name:@"nio"];
    WKUserScript *shim = [[[WKUserScript alloc]
        initWithSource:@"window.nio = {"
                        " postMessage: function(s) {"
                        "  window.webkit.messageHandlers.nio.postMessage(String(s)); },"
                        " send: function(ch, body) {"
                        "  window.webkit.messageHandlers.nio.postMessage("
                        "   String(ch) + '\\u0001' + String(body === undefined ? '' : body)); }"
                        " };"
         injectionTime:WKUserScriptInjectionTimeAtDocumentStart
      forMainFrameOnly:NO] autorelease];
    [cfg.userContentController addUserScript:shim];
}

// Put a freshly built content view into a tab slot: hidden, wrapped, observed,
// and introduced to the extension controller. Answers the slot, or -1 when the
// window is full.
//
// Ownership: adoption takes over the caller's +1. The slot stores the view
// unretained and nw_tab_close spends that reference. On -1 nothing was stored
// and the +1 stays the caller's to release.
static int nw_adopt_view(NwWin *w, WKWebView *wv) {
    int slot = -1;
    for (int i = 0; i < NW_MAX_TABS; i++) {
        if (!w->tabs[i]) { slot = i; break; }
    }
    if (slot < 0) return -1;
    NSView *wrap = [[NSView alloc] initWithFrame:nw_content_frame(w)];
    [wrap setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    [wv setFrame:[wrap bounds]];
    [wv setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    wv.navigationDelegate = w->helper;
    wv.UIDelegate = w->helper;
    // SPI: a view without the delegate sends no "__find" event, and nw_find
    // refuses in the same way.
    if ([wv respondsToSelector:@selector(_setFindDelegate:)]) {
        [wv _setFindDelegate:w->helper];
    }
    [wv addObserver:w->helper forKeyPath:@"title" options:NSKeyValueObservingOptionNew context:NULL];
    [wv addObserver:w->helper forKeyPath:@"loading" options:NSKeyValueObservingOptionNew context:NULL];
    // The observer's same-origin rule compares both ends of the change, so it
    // needs the URL the view is leaving.
    [wv addObserver:w->helper forKeyPath:@"URL"
            options:(NSKeyValueObservingOptionNew | NSKeyValueObservingOptionOld)
            context:NULL];
    if (w->devtools) nw_apply_devtools(wv, 1);
    if (w->ua) wv.customUserAgent = w->ua;
    [wrap addSubview:wv];
    [wrap setHidden:YES];
    [w->container addSubview:wrap positioned:NSWindowAbove relativeTo:w->ui];
    w->tabs[slot] = wv;
    w->wraps[slot] = wrap;
#ifdef NW_HAS_WEBEXT
    if (@available(macOS 15.4, *)) {
        if (w->extc) [w->extc didOpenTab:nw_ext_tab(w, slot)];
    }
#endif
    return slot;
}

@implementation NioWebHelper
- (void)userContentController:(WKUserContentController *)ucc
      didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)ucc;
    if ([message.name isEqualToString:@"nioGeo"]) {
        nw_geo_message(self.win, message);
        return;
    }
    if (![message.body isKindOfClass:[NSString class]]) return;
    const char *utf8 = [(NSString *)message.body UTF8String];
    if (!utf8) return;
    NwWin *w = self.win;
    if (self.isUi) {
        nw_push_msg(&w->uhead, &w->utail, utf8, strlen(utf8));
    } else {
        nw_push_msg(&w->head, &w->tail, utf8, strlen(utf8));
    }
    nw_wake();
}
// One report per search on "__find", body "<tab>,<count>,<index>". A count of
// -1 means the page holds more matches than the call asked to count, which
// WebKit reports as UINT_MAX. A search that finds nothing counts 0.
- (void)_webView:(WKWebView *)wv didFindMatches:(NSUInteger)matches
       forString:(NSString *)string withMatchIndex:(NSInteger)index {
    (void)string;
    NwWin *w = self.win;
    if (!w) return;
    long long count = (matches == NSUIntegerMax || matches > (NSUInteger)INT_MAX)
        ? -1 : (long long)matches;
    nw_push_event(w, "__find", nw_tab_of(w, wv),
        [NSString stringWithFormat:@"%lld,%lld", count, (long long)index]);
}
- (void)_webView:(WKWebView *)wv didFailToFindString:(NSString *)string {
    (void)string;
    NwWin *w = self.win;
    if (!w) return;
    nw_push_event(w, "__find", nw_tab_of(w, wv), @"0,-1");
}
- (void)windowWillClose:(NSNotification *)note {
    (void)note;
    self.win->closed = 1;
    nw_flush_replies(self.win);
    [self.win->replies release];
    self.win->replies = nil;
    [self.win->uaRules release];
    self.win->uaRules = nil;
    // Drop the granted geolocation asks; the flush above failed the undecided.
    for (int i = 0; i < NW_MAX_GEO; i++) {
        if (nw_geos[i].used && nw_geos[i].win == self.win) nw_geo_clear(i);
    }
    // For an incognito store (nw_set_incognito) this reference is what keeps
    // the session's cookies, storage and cache in RAM.
    [self.win->store release];
    self.win->store = nil;
    if (nw_last_main == self.win) nw_last_main = NULL;
}
// With a full-size content view, AppKit's titlebar strip still answers a
// double-click with zoom, over whatever the page drew there. An inline-titlebar
// window therefore refuses the gesture and leaves it to the page, which asks
// nw_zoom. The zooming flag lets that programmatic ask through.
- (BOOL)windowShouldZoom:(NSWindow *)window toFrame:(NSRect)newFrame {
    (void)window;
    (void)newFrame;
    NwWin *w = self.win;
    if (!w || !w->inlineTitlebar || w->zooming) return YES;
    NSEvent *ev = [NSApp currentEvent];
    if (ev && (ev.type == NSEventTypeLeftMouseDown || ev.type == NSEventTypeLeftMouseUp)
        && ev.clickCount >= 2) {
        return NO;
    }
    return YES;
}
// Main, not key: a popover or panel can take key status from the window the
// user is working in.
- (void)windowDidBecomeMain:(NSNotification *)note {
    (void)note;
    nw_last_main = self.win;
}
- (void)nioOpenSettings:(id)sender {
    (void)sender;
    nw_push_event(self.win, "__menu", 0, @"settings");
}
- (void)nioReload:(id)sender {
    (void)sender;
    nw_push_event(self.win, "__menu", 0, @"reload");
}
// The menu items below carry no target, so they reach the helper through the
// responder chain and report on the key window's queue.
- (void)nioZoomIn:(id)sender {
    (void)sender;
    nw_push_event(self.win, "__menu", 0, @"zoomin");
}
- (void)nioZoomOut:(id)sender {
    (void)sender;
    nw_push_event(self.win, "__menu", 0, @"zoomout");
}
- (void)nioZoomReset:(id)sender {
    (void)sender;
    nw_push_event(self.win, "__menu", 0, @"zoomreset");
}
- (void)nioAppMenuItem:(NSMenuItem *)item {
    if (![item.representedObject isKindOfClass:[NSString class]]) return;
    nw_push_event(self.win, "__menu", 0, (NSString *)item.representedObject);
}
// The context menu only opens on the shown content view, so the tab handle is
// the active one's.
- (void)nioContextItem:(NSMenuItem *)item {
    NwWin *w = self.win;
    if (![item.representedObject isKindOfClass:[NSString class]]) return;
    nw_push_event(w, "__ctx", (int64_t)w->active + 1, (NSString *)item.representedObject);
}
// An engine item the program captured (nw_ctx_capture) was picked. It reports
// on "__ctx" under the WebKit identifier, and the engine's action never runs.
- (void)nioCapturedItem:(NSMenuItem *)item {
    NwWin *w = self.win;
    if (!item.identifier.length) return;
    nw_push_event(w, "__ctx", (int64_t)w->active + 1, item.identifier);
}
// An engine "Open ... in New Window" item was picked. Record the moment, then
// run WebKit's own action off the originals stashed on the item. Only the pick
// tells the resulting view apart from a window.open's view.
static char nw_ctx_orig_target;
static char nw_ctx_orig_action;
- (void)nioEngineOpenTab:(NSMenuItem *)item {
    NwWin *w = self.win;
    if (w) w->bgOpenAt = [NSDate timeIntervalSinceReferenceDate];
    id target = objc_getAssociatedObject(item, &nw_ctx_orig_target);
    NSString *sel = objc_getAssociatedObject(item, &nw_ctx_orig_action);
    if ([sel isKindOfClass:[NSString class]] && sel.length) {
        [NSApp sendAction:NSSelectorFromString(sel) to:target from:item];
    }
}
- (void)webView:(WKWebView *)wv didCommitNavigation:(WKNavigation *)nav {
    (void)nav;
    NwWin *w = self.win;
    int64_t tab = nw_tab_of(w, wv);
    if (tab) nw_push_event(w, "__nav", tab, wv.URL.absoluteString);
#ifdef NW_HAS_WEBEXT
    // tabs.onUpdated: an extension background hears only what the host reports.
    if (@available(macOS 15.4, *)) {
        if (tab && w->extc) {
            [w->extc didChangeTabProperties:(WKWebExtensionTabChangedPropertiesURL
                                            | WKWebExtensionTabChangedPropertiesLoading)
                                     forTab:nw_ext_tab(w, (int)tab - 1)];
        }
    }
#endif
}
// WKWebView has no favicon API, so the page's own DOM is the source: the
// declared icon link, else /favicon.ico. The completion block retains the view,
// so it drops a late answer from a view that is no longer a tab instead of
// attributing it to whatever reused the slot.
- (void)webView:(WKWebView *)wv didFinishNavigation:(WKNavigation *)nav {
    (void)nav;
    NwWin *w = self.win;
    if (!nw_tab_of(w, wv)) return;
#ifdef NW_HAS_WEBEXT
    // The "complete" status tabs.onUpdated listeners key on.
    if (@available(macOS 15.4, *)) {
        if (w->extc) {
            [w->extc didChangeTabProperties:WKWebExtensionTabChangedPropertiesLoading
                                     forTab:nw_ext_tab(w, (int)nw_tab_of(w, wv) - 1)];
        }
    }
#endif
    NSString *js = @"(function(){var l=document.querySelector('link[rel~=\"icon\"]');"
                    "try{return l&&l.href?l.href:new URL('/favicon.ico',location.href).href}"
                    "catch(e){return ''}})()";
    [wv evaluateJavaScript:js completionHandler:^(id result, NSError *err) {
        (void)err;
        if (![result isKindOfClass:[NSString class]]) return;
        int64_t tab = nw_tab_of(w, wv);
        if (tab) nw_push_event(w, "__favicon", tab, (NSString *)result);
    }];
}
// WebKit refuses a main-frame navigation from a safari-web-extension: page to
// http(s) and reports nothing, so an extension flow that opens a web URL in its
// own page stalls. This delegate takes the navigation over: cancelled in the
// extension page, started in a new content tab, reported on "__newtab".
- (void)webView:(WKWebView *)wv decidePolicyForNavigationAction:(WKNavigationAction *)action
    decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    NwWin *w = self.win;
#ifdef NW_HAS_DOWNLOAD
    // An anchor with a download attribute: the page asked for a file and not
    // a navigation. This is decided first, because the frame it names does
    // not matter to a download.
    if (@available(macOS 11.3, *)) {
        if (action.shouldPerformDownload) {
            decisionHandler(WKNavigationActionPolicyDownload);
            return;
        }
    }
#endif
    NSURL *url = action.request.URL;
    if (!url || !action.targetFrame.isMainFrame || !nw_tab_of(w, wv)) {
        decisionHandler(WKNavigationActionPolicyAllow);
        return;
    }
    // A middle-click or Cmd+click on a link. The engine spells it as an ordinary
    // in-place navigation, so the gesture is taken over into a fresh view.
    // buttonNumber is 0 for no mouse and 1 for the primary button, so anything
    // else is an auxiliary button. Link activations only: a form submit or a
    // JS-driven load with Cmd held is not an ask for a tab. The tab opens behind
    // the page ("__newtabbg"), or in front ("__newtab") when Shift rides along.
    //
    // Only a view that has already navigated is taken over. The engine itself
    // synthesizes such an action for a view it just made for the gesture, and
    // that action still carries the click's buttonNumber and modifiers, so
    // without the test a second view opens and the first stays blank.
    BOOL auxGesture = action.buttonNumber != 0 && action.buttonNumber != 1;
    BOOL cmdGesture = (action.modifierFlags & NSEventModifierFlagCommand) != 0;
    if ((auxGesture || cmdGesture) && action.navigationType == WKNavigationTypeLinkActivated
        && wv.URL != nil) {
        decisionHandler(WKNavigationActionPolicyCancel);
        WKWebView *nv = nw_make_view(w, w->helper, nw_content_frame(w));
        int gslot = nw_adopt_view(w, nv);
        if (gslot < 0) { [nv release]; return; }
        [nv loadRequest:action.request];
        BOOL fg = (action.modifierFlags & NSEventModifierFlagShift) != 0;
        nw_push_event(w, fg ? "__newtab" : "__newtabbg", (int64_t)gslot + 1, url.absoluteString);
        return;
    }
    BOOL here = [wv.URL.scheme isEqualToString:@"safari-web-extension"];
    BOOL there = [url.scheme isEqualToString:@"safari-web-extension"];
    BOOL web = [url.scheme isEqualToString:@"http"] || [url.scheme isEqualToString:@"https"];
    // A view renders one side of that line only: an extension's own
    // configuration is the only thing that loads its pages. Either crossing is
    // cancelled here and started in a tab built the right way.
    int slot = -1;
    if (here && web) {
        decisionHandler(WKNavigationActionPolicyCancel);
        WKWebView *nv = nw_make_view(w, w->helper, nw_content_frame(w));
        slot = nw_adopt_view(w, nv);
        if (slot < 0) { [nv release]; return; }
        [nv loadRequest:action.request];
    } else if (!here && there) {
        decisionHandler(WKNavigationActionPolicyCancel);
#ifdef NW_HAS_WEBEXT
        int64_t ext = nw_ext_of_host(w, url.host);
        if (!ext) return;    // no loaded extension owns it: nothing renders it
        int64_t tab = nw_ext_tab_new(nw_handle_of(w), ext);
        if (tab < 1) return;
        slot = (int)tab - 1;
        [w->tabs[slot] loadRequest:action.request];
#else
        return;
#endif
    } else {
        // A User-Agent rule for this host that the view is not sending yet:
        // the string must land before the load, since the engine reads
        // customUserAgent when it makes a request. Cancel, set, re-issue; the
        // re-entered policy pass sees the strings agree and allows. Web
        // schemes only, so an about: or extension page never churns the
        // string a site page took.
        NSString *want = nw_ua_for(w, url);
        NSString *have = wv.customUserAgent;
        if (web && ![(want ? want : @"") isEqualToString:(have ? have : @"")]) {
            decisionHandler(WKNavigationActionPolicyCancel);
            wv.customUserAgent = want;
            [wv loadRequest:action.request];
            return;
        }
        decisionHandler(WKNavigationActionPolicyAllow);
        return;
    }
    nw_push_event(w, "__newtab", (int64_t)slot + 1, url.absoluteString);
}
// A body the view cannot render (canShowMIMEType), or one the server marked
// Content-Disposition: attachment, becomes a download instead of a navigation.
// The engine renders an attachment through, so the header is checked here.
// Below macOS 11.3 everything falls through to Allow.
- (void)webView:(WKWebView *)wv decidePolicyForNavigationResponse:(WKNavigationResponse *)navigationResponse
    decisionHandler:(void (^)(WKNavigationResponsePolicy))decisionHandler {
#ifdef NW_HAS_DOWNLOAD
    if (@available(macOS 11.3, *)) {
        BOOL attachment = NO;
        if ([navigationResponse.response isKindOfClass:[NSHTTPURLResponse class]]) {
            NSString *cd = [(NSHTTPURLResponse *)navigationResponse.response
                valueForHTTPHeaderField:@"Content-Disposition"];
            attachment = cd && [[cd lowercaseString] hasPrefix:@"attachment"];
        }
        if (attachment || !navigationResponse.canShowMIMEType) {
            decisionHandler(WKNavigationResponsePolicyDownload);
            return;
        }
    }
#endif
    // The status of the document the tab is about to render, reported before
    // the commit so a consumer pairs it with the "__nav" that follows. Main
    // frame only, as "__nav" is: a subframe's status is not the tab's. A
    // download answers above and reports nothing here: it renders no page.
    if (navigationResponse.forMainFrame
        && [navigationResponse.response isKindOfClass:[NSHTTPURLResponse class]]) {
        NwWin *w = self.win;
        int64_t tab = nw_tab_of(w, wv);
        if (tab) {
            long code = (long)((NSHTTPURLResponse *)navigationResponse.response).statusCode;
            nw_push_event(w, "__http", tab, [NSString stringWithFormat:@"%ld", code]);
        }
    }
    decisionHandler(WKNavigationResponsePolicyAllow);
}
#ifdef NW_HAS_DOWNLOAD
// Both Download policies land here with a fresh WKDownload. The originating
// view is retained for the transfer's life; see NwDl.
- (void)nioAdoptDownload:(WKDownload *)download from:(WKWebView *)wv API_AVAILABLE(macos(11.3)) {
    NwWin *w = self.win;
    NwDl *d = nw_dl_slot(w);
    if (!d) {    // every slot is an active transfer; refuse rather than lose one
        [download cancel:nil];
        return;
    }
    d->used = 1;
    d->id = ++w->dlSeq;
    d->state = NW_DL_ACTIVE;
    d->dl = [download retain];
    d->origin = [wv retain];
    NSURL *u = download.originalRequest.URL;
    d->url = [(u ? u.absoluteString : @"") copy];
    d->file = [@"" copy];
    d->path = [@"" copy];
    d->error = [@"" copy];
    d->total = -1;
    d->received = 0;
    download.delegate = self;
    nw_dl_event(w, d);
}
- (void)webView:(WKWebView *)wv navigationAction:(WKNavigationAction *)action
    didBecomeDownload:(WKDownload *)download API_AVAILABLE(macos(11.3)) {
    (void)action;
    [self nioAdoptDownload:download from:wv];
}
- (void)webView:(WKWebView *)wv navigationResponse:(WKNavigationResponse *)response
    didBecomeDownload:(WKDownload *)download API_AVAILABLE(macos(11.3)) {
    (void)response;
    [self nioAdoptDownload:download from:wv];
}
// A download the engine's own context menu started ("Download Image" and
// family). Only downloads born from a policy decision reach didBecomeDownload,
// and a context-menu download with no delegate is cancelled silently by WebKit.
// The selector is navigation-delegate SPI (macOS 12+); an older engine leaves
// those menu items inert.
- (void)_webView:(WKWebView *)wv contextMenuDidCreateDownload:(WKDownload *)download
    API_AVAILABLE(macos(11.3)) {
    [self nioAdoptDownload:download from:wv];
}
// Where the bytes land: the download directory, under the server's suggested
// name, dedup'd as "name (2).ext". WKDownload refuses a destination that
// already exists. Answering nil cancels the download.
- (void)download:(WKDownload *)download decideDestinationUsingResponse:(NSURLResponse *)response
    suggestedFilename:(NSString *)suggestedFilename
    completionHandler:(void (^)(NSURL *))completionHandler API_AVAILABLE(macos(11.3)) {
    NwWin *w = self.win;
    NwDl *d = nw_dl_of(w, download);
    if (!d) {
        completionHandler(nil);
        return;
    }
    NSString *dir = nw_download_dir_of(w);
    NSFileManager *fm = [NSFileManager defaultManager];
    [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL];
    NSString *name = suggestedFilename.length ? suggestedFilename : @"download";
    NSString *stem = [name stringByDeletingPathExtension];
    NSString *ext = [name pathExtension];
    NSString *cand = name;
    int n = 2;
    while ([fm fileExistsAtPath:[dir stringByAppendingPathComponent:cand]]) {
        if (ext.length) {
            cand = [NSString stringWithFormat:@"%@ (%d).%@", stem, n, ext];
        } else {
            cand = [NSString stringWithFormat:@"%@ (%d)", stem, n];
        }
        n++;
        if (n > 10000) {    // treat a directory this full as refused
            completionHandler(nil);
            return;
        }
    }
    [d->file release];
    d->file = [cand copy];
    NSString *full = [dir stringByAppendingPathComponent:cand];
    [d->path release];
    d->path = [full copy];
    if (response.expectedContentLength >= 0) d->total = response.expectedContentLength;
    nw_dl_event(w, d);
    completionHandler([NSURL fileURLWithPath:full]);
}
- (void)downloadDidFinish:(WKDownload *)download API_AVAILABLE(macos(11.3)) {
    NwWin *w = self.win;
    NwDl *d = nw_dl_of(w, download);
    if (d) nw_dl_settle(w, d, NW_DL_DONE, nil);
}
- (void)download:(WKDownload *)download didFailWithError:(NSError *)error
    resumeData:(NSData *)resumeData API_AVAILABLE(macos(11.3)) {
    (void)resumeData;
    NwWin *w = self.win;
    NwDl *d = nw_dl_of(w, download);
    if (d) nw_dl_settle(w, d, NW_DL_FAILED, error ? error.localizedDescription : @"failed");
}
#endif  /* NW_HAS_DOWNLOAD */
// A page asked for a new window: window.open, a target="_blank" link, an
// extension page's globalThis.open. WKWebView routes all of those here and does
// nothing when the delegate declines.
//
// The view must be answered synchronously, so the tab is made here and reported
// afterwards on "__newtab" ("tab,url"). WebKit loads the request into the view
// it was handed, so the program need not navigate it. The configuration must be
// used as given: it carries the opener's relationship to the new view.
- (WKWebView *)webView:(WKWebView *)wv
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
    forNavigationAction:(WKNavigationAction *)navigationAction
    windowFeatures:(WKWindowFeatures *)windowFeatures {
    (void)windowFeatures;
    NwWin *w = self.win;
    // A window.open from the UI layer is refused, not turned into a tab.
    if (self.isUi || !nw_tab_of(w, wv)) return nil;
    // A view asked for within a beat of an engine "Open ... in New Window" pick
    // is that pick's view, so it opens behind the page. The note expires, so a
    // pick whose view the engine never made cannot relabel a later window.open.
    int bg = w->bgOpenAt != 0
        && [NSDate timeIntervalSinceReferenceDate] - w->bgOpenAt < 2.0;
    w->bgOpenAt = 0;
    nw_add_bridge(w, configuration, w->helper);
    nw_add_geo(w, configuration, w->helper);
    WKWebView *nv = nw_new_content_view(w, configuration, [w->container bounds]);
    int slot = nw_adopt_view(w, nv);
    if (slot < 0) { [nv release]; return nil; }    // the window is full: the ask is declined
    NSURL *u = navigationAction.request.URL;
    nw_push_event(w, bg ? "__newtabbg" : "__newtab", (int64_t)slot + 1,
        u ? u.absoluteString : @"");
    // Borrowed: the slot's retain outlives the return, and WebKit retains what
    // the delegate answers. Do not autorelease this view. The slot would then
    // own nothing and nw_tab_close's release would free it under the pool.
    return nv;
}
// window.close() in a view the method above created. WebKit honors the call
// only in a window script opened, so this fires only for those. Reported on
// "__closed" and not acted on: the view stays alive and on screen until the
// program spends the slot with nw_tab_close.
- (void)webViewDidClose:(WKWebView *)wv {
    NwWin *w = self.win;
    int64_t tab = self.isUi ? 0 : nw_tab_of(w, wv);
    if (tab) nw_push_event(w, "__closed", tab, @"");
}
#ifdef NW_HAS_MEDIAPERM
// A page asked to capture (getUserMedia). WebKit waits for the decision
// handler. The handler is parked and the ask goes out on "__perm" with the
// request id, the origin from WKSecurityOrigin (which a page cannot spoof) and
// the kind. The program answers "grant", "deny" or "prompt" through
// nw_perm_reply; "prompt" hands the decision to the engine's own popup. A view
// that is no longer a tab, and the UI layer, are denied outright.
- (void)webView:(WKWebView *)wv
    requestMediaCapturePermissionForOrigin:(WKSecurityOrigin *)origin
    initiatedByFrame:(WKFrameInfo *)frame
    type:(WKMediaCaptureType)type
    decisionHandler:(void (^)(WKPermissionDecision))decisionHandler API_AVAILABLE(macos(12.0)) {
    (void)frame;
    NwWin *w = self.win;
    int64_t tab = self.isUi ? 0 : nw_tab_of(w, wv);
    if (!w || !tab) { decisionHandler(WKPermissionDecisionDeny); return; }
    NSString *kind = @"camera+microphone";
    if (type == WKMediaCaptureTypeCamera) kind = @"camera";
    if (type == WKMediaCaptureTypeMicrophone) kind = @"microphone";
    int64_t req = nw_park(w, ^(NSString *answer) {
        if ([answer isEqualToString:@"grant"]) {
            decisionHandler(WKPermissionDecisionGrant);
        } else if ([answer isEqualToString:@"prompt"]) {
            decisionHandler(WKPermissionDecisionPrompt);
        } else {
            decisionHandler(WKPermissionDecisionDeny);
        }
    });
    nw_push_event(w, "__perm", tab,
        nw_json(@{ @"req": @(req), @"origin": nw_origin_string(origin), @"kind": kind }));
}
// The device orientation and motion ask rides the same protocol, kind "motion".
- (void)webView:(WKWebView *)wv
    requestDeviceOrientationAndMotionPermissionForOrigin:(WKSecurityOrigin *)origin
    initiatedByFrame:(WKFrameInfo *)frame
    decisionHandler:(void (^)(WKPermissionDecision))decisionHandler API_AVAILABLE(macos(12.0)) {
    (void)frame;
    NwWin *w = self.win;
    int64_t tab = self.isUi ? 0 : nw_tab_of(w, wv);
    if (!w || !tab) { decisionHandler(WKPermissionDecisionDeny); return; }
    int64_t req = nw_park(w, ^(NSString *answer) {
        if ([answer isEqualToString:@"grant"]) {
            decisionHandler(WKPermissionDecisionGrant);
        } else if ([answer isEqualToString:@"prompt"]) {
            decisionHandler(WKPermissionDecisionPrompt);
        } else {
            decisionHandler(WKPermissionDecisionDeny);
        }
    });
    nw_push_event(w, "__perm", tab,
        nw_json(@{ @"req": @(req), @"origin": nw_origin_string(origin), @"kind": @"motion" }));
}
// A page asked to share the screen (getDisplayMedia). The public WKUIDelegate
// has no display-capture hook, so the method is WebKit SPI (macOS 13+), the
// same gap the find and backspace SPI fill; an engine without it denies every
// getDisplayMedia call by itself. The ask rides "__perm" with kind "screen".
// A grant answers ScreenPrompt: WebKit then runs the system's own picker, so
// the user still chooses what to share and a grant alone shows nothing.
// "prompt" answers the picker too, because the picker is the engine's own UI
// for this kind. withSystemAudio is ignored: WebKit on macOS captures no
// system audio.
#define NW_DISPLAY_DENY 0
#define NW_DISPLAY_SCREEN_PROMPT 1
- (void)_webView:(WKWebView *)wv
    requestDisplayCapturePermissionForSecurityOrigin:(WKSecurityOrigin *)origin
    initiatedByFrame:(WKFrameInfo *)frame
    withSystemAudio:(BOOL)withSystemAudio
    decisionHandler:(void (^)(NSInteger))decisionHandler {
    (void)frame; (void)withSystemAudio;
    NwWin *w = self.win;
    int64_t tab = self.isUi ? 0 : nw_tab_of(w, wv);
    if (!w || !tab) { decisionHandler(NW_DISPLAY_DENY); return; }
    int64_t req = nw_park(w, ^(NSString *answer) {
        if ([answer isEqualToString:@"grant"] || [answer isEqualToString:@"prompt"]) {
            decisionHandler(NW_DISPLAY_SCREEN_PROMPT);
        } else {
            decisionHandler(NW_DISPLAY_DENY);
        }
    });
    nw_push_event(w, "__perm", tab,
        nw_json(@{ @"req": @(req), @"origin": nw_origin_string(origin), @"kind": @"screen" }));
}
#endif
// The three JavaScript panels. Without these methods WebKit runs no panel and
// the page's script stops at the call, so an alert() hangs the page.
- (void)webView:(WKWebView *)wv runJavaScriptAlertPanelWithMessage:(NSString *)message
    initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(void))completionHandler {
    (void)wv; (void)frame;
    NwWin *w = self.win;
    if (!w->window) { completionHandler(); return; }
    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    a.messageText = message ? message : @"";
    [a addButtonWithTitle:@"OK"];
    [a beginSheetModalForWindow:w->window completionHandler:^(NSModalResponse r) {
        (void)r;
        completionHandler();
    }];
}
- (void)webView:(WKWebView *)wv runJavaScriptConfirmPanelWithMessage:(NSString *)message
    initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(BOOL))completionHandler {
    (void)wv; (void)frame;
    NwWin *w = self.win;
    if (!w->window) { completionHandler(NO); return; }
    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    a.messageText = message ? message : @"";
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"];
    [a beginSheetModalForWindow:w->window completionHandler:^(NSModalResponse r) {
        completionHandler(r == NSAlertFirstButtonReturn);
    }];
}
- (void)webView:(WKWebView *)wv runJavaScriptTextInputPanelWithPrompt:(NSString *)prompt
    defaultText:(NSString *)defaultText initiatedByFrame:(WKFrameInfo *)frame
    completionHandler:(void (^)(NSString * _Nullable))completionHandler {
    (void)wv; (void)frame;
    NwWin *w = self.win;
    if (!w->window) { completionHandler(nil); return; }
    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    a.messageText = prompt ? prompt : @"";
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"];
    NSTextField *field = [[[NSTextField alloc]
        initWithFrame:NSMakeRect(0, 0, 260, 24)] autorelease];
    field.stringValue = defaultText ? defaultText : @"";
    a.accessoryView = field;
    [a beginSheetModalForWindow:w->window completionHandler:^(NSModalResponse r) {
        completionHandler(r == NSAlertFirstButtonReturn ? field.stringValue : nil);
    }];
}
// The file panel behind <input type="file">. Without this method WebKit shows
// no panel and the click does nothing. The panel itself is the decision: the
// page receives only what the user picked, so no ask goes to the program.
// allowsDirectories is the input's webkitdirectory attribute; a directory
// input picks directories only, which is what Safari's panel does.
- (void)webView:(WKWebView *)wv runOpenPanelWithParameters:(WKOpenPanelParameters *)parameters
    initiatedByFrame:(WKFrameInfo *)frame
    completionHandler:(void (^)(NSArray<NSURL *> * _Nullable))completionHandler {
    (void)wv; (void)frame;
    NwWin *w = self.win;
    if (!w->window) { completionHandler(nil); return; }
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseFiles = !parameters.allowsDirectories;
    p.canChooseDirectories = parameters.allowsDirectories;
    p.allowsMultipleSelection = parameters.allowsMultipleSelection;
    [p beginSheetModalForWindow:w->window completionHandler:^(NSModalResponse r) {
        completionHandler(r == NSModalResponseOK ? p.URLs : nil);
    }];
}
// The User-Agent a rule assigns to this URL's host, else the window's own
// (which may be nil, the engine default). A rule covers its host and every
// subdomain; the longest matching rule host wins, so an "app.slack.com" rule
// beats a "slack.com" one.
static NSString *nw_ua_for(NwWin *w, NSURL *url) {
    NSString *host = url.host.lowercaseString;
    NSString *best = nil;
    NSUInteger bestLen = 0;
    if (host.length && w->uaRules) {
        for (NSString *rh in w->uaRules) {
            if (rh.length < bestLen) continue;
            if ([host isEqualToString:rh]
                || [host hasSuffix:[@"." stringByAppendingString:rh]]) {
                best = [w->uaRules objectForKey:rh];
                bestLen = rh.length;
            }
        }
    }
    return best ? best : w->ua;
}

// Two URLs share an origin when scheme, host and port all agree, absent parts
// included. A URL that spells the scheme's default port out counts as a
// different origin.
static BOOL nw_same_origin(NSURL *a, NSURL *b) {
    if (!a || !b) return NO;
    NSString *as = a.scheme.lowercaseString, *bs = b.scheme.lowercaseString;
    if (!as.length || !bs.length || ![as isEqualToString:bs]) return NO;
    NSString *ah = a.host.lowercaseString ?: @"", *bh = b.host.lowercaseString ?: @"";
    if (![ah isEqualToString:bh]) return NO;
    NSNumber *ap = a.port ?: @0, *bp = b.port ?: @0;
    return [ap isEqualToNumber:bp];
}
// Three KVO observations, each replacing a delegate hook that cannot answer.
//
// The title: WKWebView fills .title after didFinishNavigation, and a page may
// retitle itself at any time.
//
// The loading flag: the provisional navigation, the finish, a failure and
// stopLoading all flip the same property. Reported on "__load" as "view,1" or
// "view,0".
//
// The URL, for navigations that commit no document: history.pushState,
// replaceState and #fragment jumps never reach didCommitNavigation. They ride
// their own channel, "__navinpage", because an in-page change keeps the
// document, its title and its icon, where a commit replaces all three.
//
// Only a change inside the origin already shown reports at once. Anything
// crossing an origin waits for didCommitNavigation, so a slow cross-site load
// cannot put a never-fetched address in the caller's bar. An in-page navigation
// cannot cross an origin. A same-origin document navigation therefore reports
// its provisional URL here and its commit on "__nav", and a consumer that keeps
// the last URL and compares sees each once.
- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary *)change
                       context:(void *)context {
    (void)context;
    NwWin *w = self.win;
    WKWebView *wv = (WKWebView *)object;
    int64_t tab = nw_tab_of(w, wv);
    if (tab && [keyPath isEqualToString:@"loading"]) {
        nw_push_event(w, "__load", tab, wv.loading ? @"1" : @"0");
        return;
    }
    if (tab && [keyPath isEqualToString:@"URL"]) {
        id nv = change[NSKeyValueChangeNewKey], ov = change[NSKeyValueChangeOldKey];
        NSURL *nu = [nv isKindOfClass:[NSURL class]] ? (NSURL *)nv : nil;
        NSURL *ou = [ov isKindOfClass:[NSURL class]] ? (NSURL *)ov : nil;
        // A missing new URL, a missing old URL (the first navigation, which its
        // commit reports) or no change is nothing an address bar should hear.
        if (!nu.absoluteString.length || !ou) return;
        if ([nu.absoluteString isEqualToString:ou.absoluteString]) return;
        if (!nw_same_origin(ou, nu)) return;
        nw_push_event(w, "__navinpage", tab, nu.absoluteString);
#ifdef NW_HAS_WEBEXT
        // tabs.onUpdated for the in-page arm too: a callback URL can arrive as
        // a pushState route.
        if (@available(macOS 15.4, *)) {
            if (w->extc) {
                [w->extc didChangeTabProperties:WKWebExtensionTabChangedPropertiesURL
                                         forTab:nw_ext_tab(w, (int)tab - 1)];
            }
        }
#endif
        return;
    }
    if (tab && wv.title.length) nw_push_event(w, "__title", tab, wv.title);
#ifdef NW_HAS_WEBEXT
    if (@available(macOS 15.4, *)) {
        if (tab && w->extc) {
            [w->extc didChangeTabProperties:WKWebExtensionTabChangedPropertiesTitle
                                     forTab:nw_ext_tab(w, (int)tab - 1)];
        }
    }
#endif
}
@end

// developerExtrasEnabled adds "Inspect Element" to the context menu. The key is
// preference-plist spelling and not API, so KVC inside a guard is the only
// reach. inspectable (macOS 13.3+) also lets Safari's Develop menu attach.
static void nw_apply_devtools(WKWebView *wv, int on) {
    @try {
        [wv.configuration.preferences setValue:(on ? @YES : @NO)
                                        forKey:@"developerExtrasEnabled"];
    } @catch (NSException *e) {
        (void)e;
    }
    if (@available(macOS 13.3, *)) {
        wv.inspectable = on ? YES : NO;
    }
}

// Both layered web views track the mouse, and NSTrackingArea delivers by rect
// rather than by occlusion. The covered view keeps receiving mouse-moved and
// cursor-update traffic for a region the other one owns, and the two web
// processes then fight over the cursor. The covered view must stand down.
//
// Responder overrides alone are not enough. Since macOS 14 WebKit registers its
// hover tracking areas with an internal WKMouseTrackingObserver as the owner,
// and AppKit calls that owner directly.
//
// addTrackingArea: is the only interception point that works for any owner
// WebKit names. This base class substitutes an identical area owned by the
// view, remembers the real owner, and forwards each event to it only when the
// subclass's gate allows. Where WebKit owns its areas by the view, owner ==
// self and nothing is substituted.
//
// Only an area that asks for mouse-moved or cursor-update events is
// substituted. Those two cause the conflict. An area that asks for enter and
// exit only stays as it is: AppKit tool tips are such an area, owned by
// NSToolTipManager, and the manager finds each tip by the area object it
// registered itself. A substitute is unknown to it, so a forwarded
// mouseEntered: shows no tip. A covered view thus keeps its enter and exit
// events. It gets no moves, so it cannot follow the pointer with them.
@interface NioHoverGateView : WKWebView {
    NSMapTable *nioOrigToRepl;   // WebKit's area -> the substituted one
    NSMapTable *nioReplToOrig;   // and back, for event -> owner lookup
}
@property(assign, nonatomic) NwWin *nwin;
// YES while this view is the covered one: tracking events are dropped.
- (BOOL)nioSuppressHover:(NSEvent *)event;
@end

@implementation NioHoverGateView
- (BOOL)nioSuppressHover:(NSEvent *)event {
    (void)event;
    return NO;
}
- (void)addTrackingArea:(NSTrackingArea *)area {
    id owner = area.owner;
    NSTrackingAreaOptions gated = NSTrackingMouseMoved | NSTrackingCursorUpdate;
    if (!owner || owner == self || !(area.options & gated)) {
        [super addTrackingArea:area];
        return;
    }
    if (!nioOrigToRepl) {
        nioOrigToRepl = [[NSMapTable strongToStrongObjectsMapTable] retain];
        nioReplToOrig = [[NSMapTable strongToStrongObjectsMapTable] retain];
    }
    NSTrackingArea *repl = [[[NSTrackingArea alloc]
        initWithRect:area.rect options:area.options owner:self
            userInfo:area.userInfo] autorelease];
    [nioOrigToRepl setObject:repl forKey:area];
    [nioReplToOrig setObject:area forKey:repl];
    [super addTrackingArea:repl];
}
- (void)removeTrackingArea:(NSTrackingArea *)area {
    NSTrackingArea *repl = [nioOrigToRepl objectForKey:area];
    if (repl) {
        [nioOrigToRepl removeObjectForKey:area];
        [nioReplToOrig removeObjectForKey:repl];
        [super removeTrackingArea:repl];
        return;
    }
    [super removeTrackingArea:area];
}
// Enter, exit and cursor-update events carry the tracking area they came from.
// Mouse-moved events do not, because NSEvent.trackingArea is defined only for
// the other kinds, so those forward to every owner whose area asked for moves.
- (id)nioOwnerOf:(NSEvent *)event {
    NSTrackingArea *orig = [nioReplToOrig objectForKey:event.trackingArea];
    return orig ? orig.owner : nil;
}
- (void)mouseMoved:(NSEvent *)event {
    if ([self nioSuppressHover:event]) return;
    BOOL forwarded = NO;
    for (NSTrackingArea *orig in nioOrigToRepl) {
        if (!(orig.options & NSTrackingMouseMoved)) continue;
        id owner = orig.owner;
        if (owner && owner != self && [owner respondsToSelector:@selector(mouseMoved:)]) {
            [owner mouseMoved:event];
            forwarded = YES;
        }
    }
    if (!forwarded) [super mouseMoved:event];
}
- (void)mouseEntered:(NSEvent *)event {
    if ([self nioSuppressHover:event]) return;
    id owner = [self nioOwnerOf:event];
    if (owner && owner != self && [owner respondsToSelector:@selector(mouseEntered:)]) {
        [owner mouseEntered:event];
        return;
    }
    [super mouseEntered:event];
}
- (void)mouseExited:(NSEvent *)event {
    if ([self nioSuppressHover:event]) return;
    id owner = [self nioOwnerOf:event];
    if (owner && owner != self && [owner respondsToSelector:@selector(mouseExited:)]) {
        [owner mouseExited:event];
        return;
    }
    [super mouseExited:event];
}
- (void)cursorUpdate:(NSEvent *)event {
    // NSView's default resets the cursor to the arrow, so a suppressed view must
    // not call super: the other view's cursor stands.
    if ([self nioSuppressHover:event]) return;
    id owner = [self nioOwnerOf:event];
    if (owner && owner != self && [owner respondsToSelector:@selector(cursorUpdate:)]) {
        [owner cursorUpdate:event];
        return;
    }
    [super cursorUpdate:event];
}
- (void)dealloc {
    [nioOrigToRepl release];
    [nioReplToOrig release];
    [super dealloc];
}
@end

// The content views' class: the hover gate plus the program's context-menu
// items. willOpenMenu is where AppKit hands out the engine's finished menu.
@interface NioContentView : NioHoverGateView
@end

@implementation NioContentView
// While the UI layer is raised (nw_set_ui_front) it owns the mouse everywhere,
// so the page must not go on hovering links under it. Click routing needs no
// such guard, because hit-testing already finds the view on top.
- (BOOL)nioSuppressHover:(NSEvent *)event {
    (void)event;
    NwWin *w = self.nwin;
    return w && w->uiFront;
}
- (void)willOpenMenu:(NSMenu *)menu withEvent:(NSEvent *)event {
    [super willOpenMenu:menu withEvent:event];
    NwWin *w = self.nwin;
    if (!w) return;
    // Renames (nw_ctx_retitle) and removals (nw_ctx_hide), by the identifier
    // WebKit stamps on each item. Removal walks a copy, because taking an item
    // out mutates the array.
    if (w->ctxRetitles && w->ctxRetitles.count) {
        for (NSMenuItem *it in menu.itemArray) {
            NSString *t = it.identifier ? w->ctxRetitles[it.identifier] : nil;
            if (t) it.title = t;
        }
    }
    if (w->ctxHides && w->ctxHides.count) {
        for (NSMenuItem *it in [[menu.itemArray copy] autorelease]) {
            if (it.identifier && [w->ctxHides containsObject:it.identifier]) {
                [menu removeItem:it];
            }
        }
    }
    // The items the program took over (nw_ctx_capture). The target and action
    // are replaced outright, so the engine's own answer never happens. This
    // runs before the wrapping loop below, whose "already wrapped" test then
    // skips these items.
    if (w->ctxCaptures && w->ctxCaptures.count) {
        for (NSMenuItem *it in menu.itemArray) {
            if (!it.identifier || ![w->ctxCaptures containsObject:it.identifier]) continue;
            it.target = w->helper;
            it.action = @selector(nioCapturedItem:);
        }
    }
    // The engine's own "Open ... in New Window" items are wrapped so that a pick
    // is known to be one. The original action still runs; nioEngineOpenTab only
    // records the moment, which is what tells the resulting view apart from a
    // window.open's.
    for (NSMenuItem *it in menu.itemArray) {
        if (!it.identifier) continue;
        if (![it.identifier isEqualToString:@"WKMenuItemIdentifierOpenLinkInNewWindow"]
            && ![it.identifier isEqualToString:@"WKMenuItemIdentifierOpenImageInNewWindow"]
            && ![it.identifier isEqualToString:@"WKMenuItemIdentifierOpenFrameInNewWindow"]) {
            continue;
        }
        if (it.target == w->helper) continue;    // already wrapped
        objc_setAssociatedObject(it, &nw_ctx_orig_target, it.target,
            OBJC_ASSOCIATION_RETAIN);
        objc_setAssociatedObject(it, &nw_ctx_orig_action,
            it.action ? NSStringFromSelector(it.action) : @"",
            OBJC_ASSOCIATION_COPY);
        it.target = w->helper;
        it.action = @selector(nioEngineOpenTab:);
    }
    if (!w->ctxItems || !w->ctxItems.count) return;
    if (menu.numberOfItems) [menu addItem:[NSMenuItem separatorItem]];
    for (NSDictionary *d in w->ctxItems) {
        NSMenuItem *it = [[[NSMenuItem alloc] initWithTitle:d[@"title"]
                                                     action:@selector(nioContextItem:)
                                              keyEquivalent:@""] autorelease];
        it.target = w->helper;
        it.representedObject = d[@"id"];
        [menu addItem:it];
    }
}
@end

static WKWebView *nw_new_content_view(NwWin *w, WKWebViewConfiguration *cfg, NSRect frame) {
    NioContentView *cv = [[NioContentView alloc] initWithFrame:frame configuration:cfg];
    cv.nwin = w;
    return cv;
}

// The UI view's class: NioContentView's gate, mirrored. The UI view spans the
// whole window, so while it is below the content views an event over the shown
// view's frame is not its to see. Raised, it takes everything and
// NioContentView's gate stands down instead.
@interface NioUiView : NioHoverGateView
@end

@implementation NioUiView
- (BOOL)nioSuppressHover:(NSEvent *)event {
    NwWin *w = self.nwin;
    if (!w || w->uiFront) return NO;
    if (w->active < 0 || w->active >= NW_MAX_TABS) return NO;
    NSView *wrap = w->wraps[w->active];
    if (!wrap || [wrap isHidden]) return NO;
    NSPoint p = [wrap convertPoint:event.locationInWindow fromView:nil];
    return NSPointInRect(p, [wrap bounds]);
}
@end

// ---- web extensions (macOS 15.4+) ----
//
// The library hosts Safari's own WebExtensions engine by implementing the
// delegate protocols that map tabs, windows, popups and permissions onto what
// NwWin already holds. A decision the program must make is parked in the
// window's replies table (nw_park) and pushed as a "__ext*" event on the
// trusted queue; the program answers through nw_ext_reply. Event bodies are
// JSON built with NSJSONSerialization, so no title or URL can break the framing.

// The profile-name hash behind nw_profile_uuid and the extension controller's
// identity: same name, same bytes, so a profile's extension storage follows its
// site data. The version and variant bits are set, so the bytes are a
// well-formed UUID.
static NSUUID *nw_name_uuid(const unsigned char *data, int64_t len) {
    uint64_t a = 14695981039346656037ULL;
    uint64_t b = 1099511628211ULL * 31;
    for (int64_t i = 0; i < len; i++) {
        unsigned char c = data[i];
        a = (a ^ c) * 1099511628211ULL;
        b = (b ^ (c + 0x9e3779b97f4a7c15ULL)) * 1099511628211ULL;
    }
    unsigned char u[16];
    for (int i = 0; i < 8; i++) u[i] = (unsigned char)(a >> (8 * i));
    for (int i = 0; i < 8; i++) u[8 + i] = (unsigned char)(b >> (8 * i));
    u[6] = (u[6] & 0x0F) | 0x40;
    u[8] = (u[8] & 0x3F) | 0x80;
    return [[[NSUUID alloc] initWithUUIDBytes:u] autorelease];
}

static NSString *nw_json(id obj) {
    NSData *d = [NSJSONSerialization dataWithJSONObject:obj options:0 error:NULL];
    if (!d) return @"";
    return [[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] autorelease];
}

// The Safari web extension inside an .app: an .appex under Contents/PlugIns
// whose extension point is com.apple.Safari.web-extension. NSBundle reads the
// plist in any format, binary included, which Nio-side fs code cannot do. Plain
// Foundation, so this works where WKWebExtension is too old to load anything.
static NSBundle *nw_safari_appex(NSString *appPath) {
    NSString *plugins = [appPath stringByAppendingPathComponent:@"Contents/PlugIns"];
    NSArray *items = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:plugins error:NULL];
    for (NSString *it in items) {
        if (![it.pathExtension isEqualToString:@"appex"]) continue;
        NSBundle *b = [NSBundle bundleWithPath:[plugins stringByAppendingPathComponent:it]];
        NSDictionary *ext = [b objectForInfoDictionaryKey:@"NSExtension"];
        if ([ext isKindOfClass:[NSDictionary class]] &&
            [ext[@"NSExtensionPointIdentifier"] isEqualToString:@"com.apple.Safari.web-extension"]) {
            return b;
        }
    }
    return nil;
}

#ifdef NW_HAS_WEBEXT

// The extension whose pages live under this host, or 0. The host comes from the
// context's base URL, which is what runtime.getURL() writes.
static int64_t nw_ext_of_host(NwWin *w, NSString *host) {
    if (@available(macOS 15.4, *)) {
        if (!host.length) return 0;
        for (int i = 0; i < NW_MAX_EXTS; i++) {
            if (!w->exts[i].used) continue;
            if ([w->exts[i].ctx.baseURL.host caseInsensitiveCompare:host] == NSOrderedSame) {
                return (int64_t)i + 1;
            }
        }
    }
    return 0;
}

// The extension handle a context answers to, or 0.
static int64_t nw_ext_id_of(NwWin *w, WKWebExtensionContext *ctx) {
    for (int i = 0; i < NW_MAX_EXTS; i++) {
        if (w->exts[i].used && w->exts[i].ctx == ctx) return (int64_t)i + 1;
    }
    return 0;
}

// Dismiss the presented popup. closePopup on the action unloads the popup web
// view. Until that happens WebKit holds the popup open and drops the next
// performActionForTab, which wedges the button.
static void nw_ext_popup_dismiss(NwWin *w) API_AVAILABLE(macos(15.4)) {
    if (w->extPopover) {
        [w->extPopover performClose:nil];
        [w->extPopover release];
        w->extPopover = nil;
    }
    if (w->extPopoverAction) {
        [w->extPopoverAction closePopup];
        [w->extPopoverAction release];
        w->extPopoverAction = nil;
    }
}

// A window's tab, as an extension sees it: one stable object per slot, because
// the tabs API compares by identity. Each question reads through to tabs[slot]
// and caches nothing. Activating and closing a tab stay unimplemented, so
// WebKit tells the extension they are unsupported and the program keeps sole
// ownership of which tab is on screen.
API_AVAILABLE(macos(15.4))
@interface NioExtTab : NSObject <WKWebExtensionTab>
@property(assign, nonatomic) NwWin *win;
@property(assign, nonatomic) int slot;
@end

@implementation NioExtTab
- (WKWebView *)webViewForWebExtensionContext:(WKWebExtensionContext *)context {
    (void)context;
    return self.win->tabs[self.slot];
}
- (id <WKWebExtensionWindow>)windowForWebExtensionContext:(WKWebExtensionContext *)context {
    (void)context;
    return (id)self.win->extWindow;
}
- (NSUInteger)indexInWindowForWebExtensionContext:(WKWebExtensionContext *)context {
    (void)context;
    NSUInteger idx = 0;
    for (int i = 0; i < self.slot; i++) {
        if (self.win->tabs[i]) idx++;
    }
    return idx;
}
- (NSURL *)urlForWebExtensionContext:(WKWebExtensionContext *)context {
    (void)context;
    WKWebView *wv = self.win->tabs[self.slot];
    return wv ? wv.URL : nil;
}
- (NSString *)titleForWebExtensionContext:(WKWebExtensionContext *)context {
    (void)context;
    WKWebView *wv = self.win->tabs[self.slot];
    return wv ? wv.title : nil;
}
- (BOOL)isLoadingCompleteForWebExtensionContext:(WKWebExtensionContext *)context {
    (void)context;
    WKWebView *wv = self.win->tabs[self.slot];
    return wv ? !wv.isLoading : YES;
}
- (BOOL)isSelectedForWebExtensionContext:(WKWebExtensionContext *)context {
    (void)context;
    return self.slot == self.win->active;
}
- (void)loadURL:(NSURL *)url forWebExtensionContext:(WKWebExtensionContext *)context
    completionHandler:(void (^)(NSError * _Nullable))completionHandler {
    (void)context;
    WKWebView *wv = self.win->tabs[self.slot];
    if (wv) [wv loadRequest:[NSURLRequest requestWithURL:url]];
    completionHandler(nil);
}
- (void)reloadFromOrigin:(BOOL)fromOrigin forWebExtensionContext:(WKWebExtensionContext *)context
    completionHandler:(void (^)(NSError * _Nullable))completionHandler {
    (void)context;
    WKWebView *wv = self.win->tabs[self.slot];
    if (wv) {
        if (fromOrigin) [wv reloadFromOrigin]; else [wv reload];
    }
    completionHandler(nil);
}
- (void)goBackForWebExtensionContext:(WKWebExtensionContext *)context
    completionHandler:(void (^)(NSError * _Nullable))completionHandler {
    (void)context;
    WKWebView *wv = self.win->tabs[self.slot];
    if (wv) [wv goBack];
    completionHandler(nil);
}
- (void)goForwardForWebExtensionContext:(WKWebExtensionContext *)context
    completionHandler:(void (^)(NSError * _Nullable))completionHandler {
    (void)context;
    WKWebView *wv = self.win->tabs[self.slot];
    if (wv) [wv goForward];
    completionHandler(nil);
}
@end

// The window, as extensions see it, holding the live tabs in slot order. AppKit
// frames are bottom-up and the extension API's screen coordinates top-down, so
// the two frame answers flip y.
API_AVAILABLE(macos(15.4))
@interface NioExtWindow : NSObject <WKWebExtensionWindow>
@property(assign, nonatomic) NwWin *win;
@end

@implementation NioExtWindow
- (NSArray<id <WKWebExtensionTab>> *)tabsForWebExtensionContext:(WKWebExtensionContext *)context {
    (void)context;
    NSMutableArray *out = [NSMutableArray array];
    for (int i = 0; i < NW_MAX_TABS; i++) {
        if (self.win->tabs[i]) [out addObject:nw_ext_tab(self.win, i)];
    }
    return out;
}
- (id <WKWebExtensionTab>)activeTabForWebExtensionContext:(WKWebExtensionContext *)context {
    (void)context;
    NwWin *w = self.win;
    if (!w->tabs[w->active]) return nil;
    return nw_ext_tab(w, w->active);
}
- (WKWebExtensionWindowType)windowTypeForWebExtensionContext:(WKWebExtensionContext *)context {
    (void)context;
    return WKWebExtensionWindowTypeNormal;
}
- (WKWebExtensionWindowState)windowStateForWebExtensionContext:(WKWebExtensionContext *)context {
    (void)context;
    return WKWebExtensionWindowStateNormal;
}
- (BOOL)isPrivateForWebExtensionContext:(WKWebExtensionContext *)context {
    (void)context;
    return NO;
}
- (CGRect)frameForWebExtensionContext:(WKWebExtensionContext *)context {
    (void)context;
    NSWindow *win = self.win->window;
    if (!win) return CGRectZero;
    NSRect f = win.frame;
    NSRect s = win.screen ? win.screen.frame : NSZeroRect;
    return CGRectMake(f.origin.x, s.size.height - f.origin.y - f.size.height,
                      f.size.width, f.size.height);
}
- (CGRect)screenFrameForWebExtensionContext:(WKWebExtensionContext *)context {
    (void)context;
    NSWindow *win = self.win->window;
    NSScreen *s = win ? win.screen : nil;
    if (!s) return CGRectZero;
    return CGRectMake(0, 0, s.frame.size.width, s.frame.size.height);
}
- (void)focusForWebExtensionContext:(WKWebExtensionContext *)context
    completionHandler:(void (^)(NSError * _Nullable))completionHandler {
    (void)context;
    if (self.win->window) [self.win->window makeKeyAndOrderFront:nil];
    completionHandler(nil);
}
@end

static NioExtTab *nw_ext_tab(NwWin *w, int slot) {
    if (!w->extTabs[slot]) {
        NioExtTab *t = [[NioExtTab alloc] init];
        t.win = w;
        t.slot = slot;
        w->extTabs[slot] = t;
    }
    return w->extTabs[slot];
}

// The controller's delegate: where WebKit asks the host what its world looks
// like. Questions the window can answer are answered inline. Decisions the
// program owns are parked and pushed.
API_AVAILABLE(macos(15.4))
@interface NioExtDelegate : NSObject <WKWebExtensionControllerDelegate>
@property(assign, nonatomic) NwWin *win;
@end

@implementation NioExtDelegate
- (NSArray<id <WKWebExtensionWindow>> *)webExtensionController:(WKWebExtensionController *)controller
    openWindowsForExtensionContext:(WKWebExtensionContext *)extensionContext {
    (void)controller;
    (void)extensionContext;
    return @[ (id)self.win->extWindow ];
}
- (id <WKWebExtensionWindow>)webExtensionController:(WKWebExtensionController *)controller
    focusedWindowForExtensionContext:(WKWebExtensionContext *)extensionContext {
    (void)controller;
    (void)extensionContext;
    return (id)self.win->extWindow;
}
// The three permission prompts share one shape: park the completion, push
// "__extperm" with the request id and the items asked for, then grant all of
// them on "grant" and none on any other answer.
- (void)webExtensionController:(WKWebExtensionController *)controller
    promptForPermissions:(NSSet<WKWebExtensionPermission> *)permissions
    inTab:(id <WKWebExtensionTab>)tab
    forExtensionContext:(WKWebExtensionContext *)extensionContext
    completionHandler:(void (^)(NSSet<WKWebExtensionPermission> *, NSDate * _Nullable))completionHandler {
    (void)controller;
    (void)tab;
    NwWin *w = self.win;
    int64_t req = nw_park(w, ^(NSString *answer) {
        completionHandler([answer isEqualToString:@"grant"] ? permissions : [NSSet set], nil);
    });
    nw_push_event(w, "__extperm", nw_ext_id_of(w, extensionContext),
        nw_json(@{ @"req": @(req), @"kind": @"permissions",
                   @"items": [permissions allObjects] }));
}
- (void)webExtensionController:(WKWebExtensionController *)controller
    promptForPermissionMatchPatterns:(NSSet<WKWebExtensionMatchPattern *> *)matchPatterns
    inTab:(id <WKWebExtensionTab>)tab
    forExtensionContext:(WKWebExtensionContext *)extensionContext
    completionHandler:(void (^)(NSSet<WKWebExtensionMatchPattern *> *, NSDate * _Nullable))completionHandler {
    (void)controller;
    (void)tab;
    NwWin *w = self.win;
    NSMutableArray *items = [NSMutableArray array];
    for (WKWebExtensionMatchPattern *p in matchPatterns) [items addObject:p.string];
    int64_t req = nw_park(w, ^(NSString *answer) {
        completionHandler([answer isEqualToString:@"grant"] ? matchPatterns : [NSSet set], nil);
    });
    nw_push_event(w, "__extperm", nw_ext_id_of(w, extensionContext),
        nw_json(@{ @"req": @(req), @"kind": @"patterns", @"items": items }));
}
- (void)webExtensionController:(WKWebExtensionController *)controller
    promptForPermissionToAccessURLs:(NSSet<NSURL *> *)urls
    inTab:(id <WKWebExtensionTab>)tab
    forExtensionContext:(WKWebExtensionContext *)extensionContext
    completionHandler:(void (^)(NSSet<NSURL *> *, NSDate * _Nullable))completionHandler {
    (void)controller;
    (void)tab;
    NwWin *w = self.win;
    NSMutableArray *items = [NSMutableArray array];
    for (NSURL *u in urls) [items addObject:u.absoluteString];
    int64_t req = nw_park(w, ^(NSString *answer) {
        completionHandler([answer isEqualToString:@"grant"] ? urls : [NSSet set], nil);
    });
    nw_push_event(w, "__extperm", nw_ext_id_of(w, extensionContext),
        nw_json(@{ @"req": @(req), @"kind": @"urls", @"items": items }));
}
// The toolbar button's state changed. The event carries the cheap facts. The
// program pulls the icon through nw_ext_icon when it draws the button.
- (void)webExtensionController:(WKWebExtensionController *)controller
    didUpdateAction:(WKWebExtensionAction *)action
    forExtensionContext:(WKWebExtensionContext *)context {
    (void)controller;
    NwWin *w = self.win;
    nw_push_event(w, "__extaction", nw_ext_id_of(w, context),
        nw_json(@{ @"enabled": action.enabled ? @YES : @NO,
                   @"badge": action.badgeText ? action.badgeText : @"",
                   @"label": action.label ? action.label : @"" }));
}
// WebKit hands a ready-made NSPopover holding the popup web view. Presentation
// anchors it at the rect the UI page measured (nw_ext_action_click stored it).
- (void)webExtensionController:(WKWebExtensionController *)controller
    presentPopupForAction:(WKWebExtensionAction *)action
    forExtensionContext:(WKWebExtensionContext *)context
    completionHandler:(void (^)(NSError * _Nullable))completionHandler {
    (void)controller;
    NwWin *w = self.win;
    NSPopover *pop = action.popupPopover;
    if (!pop || !w->container) {
        completionHandler(nil);
        return;
    }
    NSRect b = [w->container bounds];
    CGFloat x = (CGFloat)w->extRect[0];
    CGFloat y = (CGFloat)w->extRect[1];
    CGFloat wd = (CGFloat)w->extRect[2];
    CGFloat ht = (CGFloat)w->extRect[3];
    if (wd < 1) wd = 1;
    if (ht < 1) ht = 1;
    NSRect anchor = NSMakeRect(x, b.size.height - y - ht, wd, ht);
    // Presenting a popup does not dismiss another extension's, so close the old
    // one first or it stays on screen owned by nothing.
    nw_ext_popup_dismiss(w);
    w->extPopover = [pop retain];
    w->extPopoverAction = [action retain];
    w->extPopoverExt = nw_ext_id_of(w, context);
    [pop showRelativeToRect:anchor ofView:w->container preferredEdge:NSRectEdgeMinY];
    nw_push_event(w, "__extpopup", nw_ext_id_of(w, context), @"");
    completionHandler(nil);
}
// An extension asks for a new tab (window.open, tabs.create). Which tabs exist
// is the program's session, so the ask is parked and pushed. The program makes
// the tab and replies with its handle; an empty reply refuses.
- (void)webExtensionController:(WKWebExtensionController *)controller
    openNewTabUsingConfiguration:(WKWebExtensionTabConfiguration *)configuration
    forExtensionContext:(WKWebExtensionContext *)extensionContext
    completionHandler:(void (^)(id <WKWebExtensionTab>, NSError * _Nullable))completionHandler {
    (void)controller;
    NwWin *w = self.win;
    int64_t req = nw_park(w, ^(NSString *answer) {
        int64_t t = [answer longLongValue];
        if (t >= 1 && t <= NW_MAX_TABS && w->tabs[t - 1]) {
            completionHandler(nw_ext_tab(w, (int)t - 1), nil);
        } else {
            completionHandler(nil, [NSError errorWithDomain:@"NioWebview" code:1
                userInfo:@{ NSLocalizedDescriptionKey: @"the app declined to open a tab" }]);
        }
    });
    NSString *url = configuration.url ? configuration.url.absoluteString : @"";
    nw_push_event(w, "__extnewtab", nw_ext_id_of(w, extensionContext),
        nw_json(@{ @"req": @(req), @"url": url }));
}
// An extension asks for a whole window (windows.create). It is answered with a
// tab in the program's one window, through the same __extnewtab flow.
- (void)webExtensionController:(WKWebExtensionController *)controller
    openNewWindowUsingConfiguration:(WKWebExtensionWindowConfiguration *)configuration
    forExtensionContext:(WKWebExtensionContext *)extensionContext
    completionHandler:(void (^)(id <WKWebExtensionWindow>, NSError * _Nullable))completionHandler {
    (void)controller;
    NwWin *w = self.win;
    int64_t req = nw_park(w, ^(NSString *answer) {
        int64_t t = [answer longLongValue];
        if (t >= 1 && t <= NW_MAX_TABS && w->tabs[t - 1]) {
            completionHandler((id)w->extWindow, nil);
        } else {
            completionHandler(nil, [NSError errorWithDomain:@"NioWebview" code:1
                userInfo:@{ NSLocalizedDescriptionKey: @"the app declined to open a window" }]);
        }
    });
    NSURL *first = configuration.tabURLs.firstObject;
    nw_push_event(w, "__extnewtab", nw_ext_id_of(w, extensionContext),
        nw_json(@{ @"req": @(req), @"url": first ? first.absoluteString : @"" }));
}
// Native messaging answers an error. An extension that probes for a companion
// app then fails cleanly instead of finding a missing API.
- (void)webExtensionController:(WKWebExtensionController *)controller
    sendMessage:(id)message
    toApplicationWithIdentifier:(NSString *)applicationIdentifier
    forExtensionContext:(WKWebExtensionContext *)extensionContext
    replyHandler:(void (^)(id, NSError * _Nullable))replyHandler {
    (void)controller;
    (void)message;
    (void)applicationIdentifier;
    (void)extensionContext;
    replyHandler(nil, [NSError errorWithDomain:@"NioWebview" code:2
        userInfo:@{ NSLocalizedDescriptionKey: @"native messaging is not supported" }]);
}
@end

// Remove the window's extension world. Parked decisions answer "" (declined),
// so no WebKit completion handler is dropped.
static void nw_ext_teardown(NwWin *w) {
    if (@available(macOS 15.4, *)) {
        if (!w->extc) return;
        for (int i = 0; i < NW_MAX_EXTS; i++) {
            if (!w->exts[i].used) continue;
            [w->extc unloadExtensionContext:w->exts[i].ctx error:NULL];
            [w->exts[i].ctx release];
            [w->exts[i].ext release];
            w->exts[i].used = 0;
            w->exts[i].ctx = nil;
            w->exts[i].ext = nil;
        }
        nw_flush_replies(w);
        nw_ext_popup_dismiss(w);
        [w->extc release];
        w->extc = nil;
    }
}

// Build the window's extension controller. Its identity is the profile UUID,
// which owns chrome.storage, granted permissions and DNR state.
static void nw_ext_setup(NwWin *w) {
    if (@available(macOS 15.4, *)) {
        if (w->extc) return;
        WKWebExtensionControllerConfiguration *cfg;
        if (w->ephemeral) {
            // An incognito window's extension storage dies with the session.
            cfg = [WKWebExtensionControllerConfiguration nonPersistentConfiguration];
        } else if (w->profileName && w->profileName.length) {
            const char *utf8 = w->profileName.UTF8String;
            cfg = [WKWebExtensionControllerConfiguration
                configurationWithIdentifier:nw_name_uuid((const unsigned char *)utf8,
                                                         (int64_t)strlen(utf8))];
        } else {
            cfg = [WKWebExtensionControllerConfiguration defaultConfiguration];
        }
        if (w->store) cfg.defaultWebsiteDataStore = w->store;
        w->extc = [[WKWebExtensionController alloc] initWithConfiguration:cfg];
        if (!w->extDelegate) {
            w->extDelegate = [[NioExtDelegate alloc] init];
            w->extDelegate.win = w;
        }
        w->extc.delegate = w->extDelegate;
        if (!w->extWindow) {
            w->extWindow = [[NioExtWindow alloc] init];
            w->extWindow.win = w;
        }
        [w->extc didOpenWindow:w->extWindow];
        [w->extc didFocusWindow:w->extWindow];
        for (int i = 0; i < NW_MAX_TABS; i++) {
            if (w->tabs[i]) [w->extc didOpenTab:nw_ext_tab(w, i)];
        }
        if (w->tabs[w->active]) {
            [w->extc didActivateTab:nw_ext_tab(w, w->active) previousActiveTab:nil];
        }
    }
}

#endif  /* NW_HAS_WEBEXT */

// ---- the application delegate: the Dock menu and the reopen event ----
//
// A Dock pick reports "dock:<id>" on the app event queue. A Dock click with no
// visible window reports "reopen". A URL that another application gives to
// this one reports "open:<url>".

static NwMsg *nw_app_head, *nw_app_tail;
static NSMenu *nw_dock_menu;
static NSString *nw_nsstring(Str *s);

@interface NioAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation NioAppDelegate
- (NSMenu *)applicationDockMenu:(NSApplication *)sender {
    (void)sender;
    return nw_dock_menu;    // nil until the program adds an item; AppKit then uses its own
}
// Return NO to suppress the AppKit default, which raises every window. The
// program decides which window comes up.
- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender
                    hasVisibleWindows:(BOOL)visible {
    (void)sender;
    (void)visible;
    nw_push_msg(&nw_app_head, &nw_app_tail, "reopen", 6);
    nw_wake();
    return NO;
}
// The URLs another application gives to this one: a link clicked outside the
// program when this app is the handler for the scheme, and a document opened
// onto it. Each URL reports "open:<url>" and nothing more happens. The program
// decides which window takes a URL, and if it opens one at all. All URLs are
// pushed before the wake, so a group stays a group.
//
// This method also makes AppKit deliver the launch event instead of dropping
// it. A start from a click thus reports the URL when the program next drains
// the queue.
- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls {
    (void)app;
    for (NSURL *u in urls) {
        NSString *s = u.absoluteString;
        if (!s.length) continue;
        NSString *ev = [@"open:" stringByAppendingString:s];
        const char *utf8 = ev.UTF8String;
        if (!utf8) continue;
        nw_push_msg(&nw_app_head, &nw_app_tail, utf8, strlen(utf8));
    }
    nw_wake();
}
- (void)nioDockItem:(NSMenuItem *)item {
    if (![item.representedObject isKindOfClass:[NSString class]]) return;
    NSString *s = [@"dock:" stringByAppendingString:(NSString *)item.representedObject];
    const char *utf8 = s.UTF8String;
    if (!utf8) return;
    nw_push_msg(&nw_app_head, &nw_app_tail, utf8, strlen(utf8));
    nw_wake();
}
// Main-menu items are targetless. With no key window the responder chain
// reaches the app delegate here, or AppKit greys every program item. A pick
// reports "menu:<id>" on the app queue.
- (void)nioAppMenuItem:(NSMenuItem *)item {
    if (![item.representedObject isKindOfClass:[NSString class]]) return;
    NSString *s = [@"menu:" stringByAppendingString:(NSString *)item.representedObject];
    const char *utf8 = s.UTF8String;
    if (!utf8) return;
    nw_push_msg(&nw_app_head, &nw_app_tail, utf8, strlen(utf8));
    nw_wake();
}
@end

static NioAppDelegate *nw_app_delegate;

// The app object, once per process. Use finishLaunching and not [NSApp run]:
// the program owns its own loop and must keep the main thread.
static void nw_app_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    [NSApplication sharedApplication];
    nw_app_delegate = [[NioAppDelegate alloc] init];
    [NSApp setDelegate:nw_app_delegate];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    [NSApp activateIgnoringOtherApps:YES];
}

// One entry of the Dock icon's right-click menu. Re-adding an id replaces the
// title and does not duplicate the entry.
int64_t nw_dock_menu_item(Str *id, Str *title) {
    @autoreleasepool {
        nw_app_init();
        NSString *i = [nw_nsstring(id) autorelease];
        NSString *t = [nw_nsstring(title) autorelease];
        if (!i || !t || !i.length || !t.length) return NW_ERR_BAD_INPUT;
        if (!nw_dock_menu) nw_dock_menu = [[NSMenu alloc] initWithTitle:@""];
        for (NSMenuItem *it in nw_dock_menu.itemArray) {
            if ([it.representedObject isEqual:i]) {
                it.title = t;
                return 0;
            }
        }
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:t
                                                      action:@selector(nioDockItem:)
                                               keyEquivalent:@""];
        item.target = nw_app_delegate;
        item.representedObject = i;
        [nw_dock_menu addItem:item];
        [item release];
        return 0;
    }
}

// A separator line in the Dock menu, under an id like any other entry.
// Re-adding an existing id is a no-op.
int64_t nw_dock_menu_separator(Str *id) {
    @autoreleasepool {
        nw_app_init();
        NSString *i = [nw_nsstring(id) autorelease];
        if (!i || !i.length) return NW_ERR_BAD_INPUT;
        if (!nw_dock_menu) nw_dock_menu = [[NSMenu alloc] initWithTitle:@""];
        for (NSMenuItem *it in nw_dock_menu.itemArray) {
            if ([it.representedObject isEqual:i]) return 0;
        }
        NSMenuItem *item = [NSMenuItem separatorItem];
        item.representedObject = i;
        [nw_dock_menu addItem:item];
        return 0;
    }
}

// Remove one Dock menu entry by its id. An id that is not there is a no-op.
int64_t nw_dock_menu_remove(Str *id) {
    @autoreleasepool {
        NSString *i = [nw_nsstring(id) autorelease];
        if (!i || !i.length) return NW_ERR_BAD_INPUT;
        if (!nw_dock_menu) return 0;
        for (NSMenuItem *it in nw_dock_menu.itemArray) {
            if ([it.representedObject isEqual:i]) {
                [nw_dock_menu removeItem:it];
                return 0;
            }
        }
        return 0;
    }
}

// The handle of the last window to become main, or 0 when none is open.
int64_t nw_last_active(void) {
    if (nw_last_main && nw_last_main->used && !nw_last_main->closed) {
        return nw_handle_of(nw_last_main);
    }
    return 0;
}

int64_t nw_focus_window(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w || w->closed || !w->window) return NW_ERR_INVALID;
        if (w->window.miniaturized) [w->window deminiaturize:nil];
        [w->window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        return 0;
    }
}

// Pump the shared event loop for at most timeout_ms, tied to no window. After
// the first event the deadline drops to distantPast, as in nw_step.
int64_t nw_app_step(int64_t timeout_ms) {
    @autoreleasepool {
        nw_app_init();
        NSDate *deadline =
            [NSDate dateWithTimeIntervalSinceNow:(NSTimeInterval)timeout_ms / 1000.0];
        for (;;) {
            NSEvent *ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:deadline
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES];
            if (!ev) break;
            [NSApp sendEvent:ev];
            deadline = [NSDate distantPast];
        }
        return 0;
    }
}

int64_t nw_app_has_event(void) {
    return nw_app_head ? 1 : 0;
}

Str *nw_app_take_event(void) {
    return nw_pop_msg(&nw_app_head, &nw_app_tail);
}

static NSString *nw_nsstring(Str *s) {
    return [[NSString alloc] initWithBytes:s->data
                                    length:(NSUInteger)s->len
                                  encoding:NSUTF8StringEncoding];
}

// One process pool shared by every view, so WebKit can coalesce WebContent
// processes instead of spawning one per view.
static WKProcessPool *nw_pool(void) {
    static WKProcessPool *pool = nil;
    if (!pool) pool = [[WKProcessPool alloc] init];
    return pool;
}

static WKWebView *nw_make_view(NwWin *w, NioWebHelper *helper, NSRect frame) {
    WKWebViewConfiguration *cfg = [[WKWebViewConfiguration alloc] init];
    [cfg setProcessPool:nw_pool()];
    if (w->store) cfg.websiteDataStore = w->store;
#ifdef NW_HAS_WEBEXT
    // A view only injects content scripts for the controller its configuration
    // names. The UI view gets none: extensions must not reach the program chrome.
    if (!helper.isUi) {
        if (@available(macOS 15.4, *)) {
            if (w->extc) cfg.webExtensionController = w->extc;
        }
    }
#endif
    nw_add_bridge(w, cfg, helper);
    nw_add_geo(w, cfg, helper);
    if (w->noBackspaceNav) {
        WKPreferences *p = cfg.preferences;
        if ([p respondsToSelector:@selector(_setBackspaceKeyNavigationEnabled:)]) {
            [p _setBackspaceKeyNavigationEnabled:NO];
        }
    }
    // Screen sharing (getDisplayMedia) is off in a WKWebView by default. The
    // switch is preference-plist spelling like developerExtrasEnabled, so KVC
    // inside a guard is the only reach; without it the "screen" asks never
    // fire and the page's call rejects. Content views only: the UI page
    // shares nothing.
    if (!helper.isUi) {
        @try {
            [cfg.preferences setValue:@YES forKey:@"screenCaptureEnabled"];
        } @catch (NSException *e) {
            (void)e;
        }
    }

    WKWebView *wv;
    if (helper.isUi) {
        NioUiView *ui = [[NioUiView alloc] initWithFrame:frame configuration:cfg];
        ui.nwin = w;
        wv = ui;
    } else {
        wv = nw_new_content_view(w, cfg, frame);
    }
    [cfg release];    // the view keeps its own copy of the configuration
    [wv setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    // nw_adopt_view wires the delegates and the title KVO. Wiring them here too
    // would register the observer twice.
    return wv;
}

int64_t nw_create(Str *title, int64_t width, int64_t height) {
    @autoreleasepool {
        nw_app_init();
        int slot = -1;
        for (int i = 0; i < NW_MAX_WINDOWS; i++) {
            if (!nw_wins[i].used) { slot = i; break; }
        }
        if (slot < 0) return NW_ERR_FULL;
        NwWin *w = &nw_wins[slot];
        memset(w, 0, sizeof *w);
        w->used = 1;

        NSRect rect = NSMakeRect(0, 0, (CGFloat)width, (CGFloat)height);
        NSWindow *win = [[NSWindow alloc]
            initWithContentRect:rect
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        // The handle outlives the close: nw_close decides when the struct is done.
        [win setReleasedWhenClosed:NO];
        NSString *t = nw_nsstring(title);
        if (t) [win setTitle:t];

        NioWebHelper *helper = [[NioWebHelper alloc] init];
        helper.win = w;
        helper.isUi = 0;

#ifdef NW_HAS_WEBEXT
        // Before the first view: nw_make_view reads w->extc.
        nw_ext_setup(w);
#endif

        NSView *container = [[NSView alloc] initWithFrame:rect];
        [container setAutoresizesSubviews:YES];
        [win setContentView:container];
        [win setDelegate:helper];

        w->window = win;
        w->container = container;
        w->helper = helper;
        WKWebView *wv = nw_make_view(w, helper, rect);
        nw_adopt_view(w, wv);
        [w->wraps[0] setHidden:NO];
        w->webview = wv;
        w->active = 0;
        [win center];
        [win makeKeyAndOrderFront:nil];
#ifdef NW_HAS_WEBEXT
        if (@available(macOS 15.4, *)) {
            if (w->extc) [w->extc didActivateTab:nw_ext_tab(w, 0) previousActiveTab:nil];
        }
#endif
        return (int64_t)slot + 1;
    }
}

// ---- tabs ----
//
// All tabs share the window's one content queue and helper. A message does not
// say which tab sent it.

// The frame the active content view's wrap occupies. The wrap stays the
// superview WebKit's attached inspector docks into.
static NSRect nw_content_frame(NwWin *w) {
    NSRect b = [w->container bounds];
    if (!w->has_insets) return b;
    NSRect f = NSMakeRect((CGFloat)w->ins[1], (CGFloat)w->ins[2],
                          b.size.width - (CGFloat)(w->ins[1] + w->ins[3]),
                          b.size.height - (CGFloat)(w->ins[0] + w->ins[2]));
    if (f.size.width < 0) f.size.width = 0;
    if (f.size.height < 0) f.size.height = 0;
    return f;
}

int64_t nw_tab_new(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        WKWebView *wv = nw_make_view(w, w->helper, nw_content_frame(w));
        int slot = nw_adopt_view(w, wv);
        if (slot < 0) { [wv release]; return NW_ERR_FULL; }
        return (int64_t)slot + 1;
    }
}

int64_t nw_tab_show(int64_t h, int64_t tab) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        if (tab < 1 || tab > NW_MAX_TABS || !w->tabs[tab - 1]) return NW_ERR_BAD_INPUT;
        WKWebView *next = w->tabs[tab - 1];
        if (next == w->webview) return 0;
        [w->wraps[tab - 1] setFrame:nw_content_frame(w)];
        [w->wraps[tab - 1] setHidden:NO];
        [w->wraps[w->active] setHidden:YES];
#ifdef NW_HAS_WEBEXT
        int prev = w->active;
#endif
        w->webview = next;
        w->active = (int)tab - 1;
#ifdef NW_HAS_WEBEXT
        if (@available(macOS 15.4, *)) {
            if (w->extc) {
                [w->extc didActivateTab:nw_ext_tab(w, w->active)
                      previousActiveTab:(w->tabs[prev] ? nw_ext_tab(w, prev) : nil)];
            }
        }
#endif
        return 0;
    }
}

// Make the shown content view the first responder, so key events reach the
// page. nw_tab_show does not do this: the focus policy is the program's.
int64_t nw_tab_focus(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w || !w->webview) return NW_ERR_INVALID;
        [w->window makeFirstResponder:w->webview];
        return 0;
    }
}

// Give the keyboard to the UI layer. A DOM focus() in the UI page is not
// enough: while a content view is first responder the keys go to the site.
int64_t nw_ui_focus(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w || !w->ui) return NW_ERR_INVALID;
        [w->window makeFirstResponder:w->ui];
        return 0;
    }
}

// The first responder is usually a view inside the web view, so test for a
// descendant and not for the layer itself.
static int64_t nw_layer_focused(NwWin *w, NSView *layer) {
    if (!w || !w->window || !layer) return 0;
    NSResponder *r = [w->window firstResponder];
    if (![r isKindOfClass:[NSView class]]) return 0;
    NSView *v = (NSView *)r;
    return (v == layer || [v isDescendantOf:layer]) ? 1 : 0;
}

int64_t nw_ui_focused(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        return nw_layer_focused(w, w->ui);
    }
}

// Backspace goes back in history. WebKit applies it only outside a text field.
// Views made later get the same setting.
int64_t nw_set_backspace_nav(int64_t h, int64_t on) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        WKPreferences *probe = [[[WKPreferences alloc] init] autorelease];
        if (![probe respondsToSelector:@selector(_setBackspaceKeyNavigationEnabled:)]) {
            return NW_ERR_UNSUPPORTED;
        }
        w->noBackspaceNav = on ? 0 : 1;
        nw_apply_backspace_nav(w->ui, (int)on);
        for (int i = 0; i < NW_MAX_TABS; i++) {
            nw_apply_backspace_nav(w->tabs[i], (int)on);
        }
        return 0;
    }
}

// Stop the shown view's load. The loading KVO reports the flag going false
// ("__load"), so the caller does not assume the stop landed.
int64_t nw_stop_loading(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w || !w->webview) return NW_ERR_INVALID;
        [w->webview stopLoading];
        return 0;
    }
}

// Closing the active tab is refused. Every other function needs w->webview to
// be a live, visible view, so the caller shows another tab first.
int64_t nw_tab_close(int64_t h, int64_t tab) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        if (tab < 1 || tab > NW_MAX_TABS || !w->tabs[tab - 1]) return NW_ERR_BAD_INPUT;
        WKWebView *wv = w->tabs[tab - 1];
        if (wv == w->webview) return NW_ERR_BAD_INPUT;
#ifdef NW_HAS_WEBEXT
        // Before the view goes: the tabs API may look at it one last time.
        if (@available(macOS 15.4, *)) {
            if (w->extc && w->extTabs[tab - 1]) {
                [w->extc didCloseTab:w->extTabs[tab - 1] windowIsClosing:NO];
            }
        }
#endif
        [wv removeObserver:w->helper forKeyPath:@"title"];
        [wv removeObserver:w->helper forKeyPath:@"loading"];
        [wv removeObserver:w->helper forKeyPath:@"URL"];
        [wv removeFromSuperview];
        [wv release];
        [w->wraps[tab - 1] removeFromSuperview];
        [w->wraps[tab - 1] release];
        w->tabs[tab - 1] = NULL;
        w->wraps[tab - 1] = NULL;
#ifdef NW_HAS_WEBEXT
        if (w->extTabs[tab - 1]) {
            [w->extTabs[tab - 1] release];
            w->extTabs[tab - 1] = nil;
        }
#endif
        return 0;
    }
}

// ---- tab state and media ----

// The tab snapshot as base64, or "" when there is nothing to give. The caller's
// fallback for "" is to reload the URL.
Str *nw_tab_state(int64_t h, int64_t tab) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w || tab < 1 || tab > NW_MAX_TABS || !w->tabs[tab - 1]) return rt_str_alloc(0);
#ifdef NW_HAS_TABSTATE
        if (@available(macOS 12.0, *)) {
            id st = w->tabs[tab - 1].interactionState;
            // interactionState is documented as opaque. Any shape but NSData
            // reads as "nothing to give".
            if ([st isKindOfClass:[NSData class]]) {
                NSString *b64 = [(NSData *)st base64EncodedStringWithOptions:0];
                const char *utf8 = [b64 UTF8String];
                size_t len = strlen(utf8);
                Str *out = rt_str_alloc((int64_t)len);
                memcpy(out->data, utf8, len);
                return out;
            }
        }
#endif
        return rt_str_alloc(0);
    }
}

// Restore a nw_tab_state snapshot into a tab that has not navigated yet. The
// engine ignores an unrecognizable snapshot, so only bad base64 is refused.
int64_t nw_tab_set_state(int64_t h, int64_t tab, Str *state) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        if (tab < 1 || tab > NW_MAX_TABS || !w->tabs[tab - 1]) return NW_ERR_BAD_INPUT;
#ifdef NW_HAS_TABSTATE
        if (@available(macOS 12.0, *)) {
            NSString *b64 = [nw_nsstring(state) autorelease];
            if (!b64 || b64.length == 0) return NW_ERR_BAD_INPUT;
            NSData *data = [[[NSData alloc] initWithBase64EncodedString:b64
                options:NSDataBase64DecodingIgnoreUnknownCharacters] autorelease];
            if (!data) return NW_ERR_BAD_INPUT;
            w->tabs[tab - 1].interactionState = data;
            return 0;
        }
#endif
        return NW_ERR_UNSUPPORTED;
    }
}

// Page zoom on one tab, in percent (25..500). This is layout zoom: text
// reflows, unlike magnification's pixel scaling.
int64_t nw_tab_zoom(int64_t h, int64_t tab, int64_t percent) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        if (tab < 1 || tab > NW_MAX_TABS || !w->tabs[tab - 1]) return NW_ERR_BAD_INPUT;
        if (percent < 25 || percent > 500) return NW_ERR_BAD_INPUT;
        if (@available(macOS 11.0, *)) {
            w->tabs[tab - 1].pageZoom = (CGFloat)percent / 100.0;
            return 0;
        }
        return NW_ERR_UNSUPPORTED;
    }
}

// Whether the tab makes sound now: 1 audible, 0 silent. Read the private
// _isPlayingAudio: the public requestMediaPlaybackState: also answers "playing"
// for a muted autoplay video. A WebKit that drops the property gives UNSUPPORTED.
int64_t nw_tab_audible(int64_t h, int64_t tab) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        if (tab < 1 || tab > NW_MAX_TABS || !w->tabs[tab - 1]) return NW_ERR_BAD_INPUT;
        @try {
            id v = [w->tabs[tab - 1] valueForKey:@"_isPlayingAudio"];
            if ([v isKindOfClass:[NSNumber class]]) {
                return [(NSNumber *)v boolValue] ? 1 : 0;
            }
        } @catch (NSException *e) {
        }
        return NW_ERR_UNSUPPORTED;
    }
}

// Whether the tab holds the camera or the microphone: 0 neither, bit 1 camera,
// bit 2 microphone. Muted counts as holding: WebKit keeps the device session.
int64_t nw_tab_capture_state(int64_t h, int64_t tab) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        if (tab < 1 || tab > NW_MAX_TABS || !w->tabs[tab - 1]) return NW_ERR_BAD_INPUT;
#ifdef NW_HAS_TABSTATE
        if (@available(macOS 12.0, *)) {
            WKWebView *wv = w->tabs[tab - 1];
            int64_t bits = 0;
            if (wv.cameraCaptureState != WKMediaCaptureStateNone) bits |= 1;
            if (wv.microphoneCaptureState != WKMediaCaptureStateNone) bits |= 2;
            return bits;
        }
#endif
        return NW_ERR_UNSUPPORTED;
    }
}

// What the page does with media now: 0 none, 1 playing, 2 paused, 3 suspended.
// The completion lands on the main queue, so the call runs the loop until it
// does. The spin cannot reenter the program: NSEvents wait for nw_step's pump.
int64_t nw_tab_media_state(int64_t h, int64_t tab) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        if (tab < 1 || tab > NW_MAX_TABS || !w->tabs[tab - 1]) return NW_ERR_BAD_INPUT;
#ifdef NW_HAS_TABSTATE
        if (@available(macOS 12.0, *)) {
            // A box and not a __block stack flag. The retained block writes
            // into a live object when the completion fires after a timeout
            // return.
            NSMutableArray *box = [NSMutableArray array];
            [w->tabs[tab - 1] requestMediaPlaybackState:^(WKMediaPlaybackState st) {
                int64_t v = 0;
                switch (st) {
                    case WKMediaPlaybackStatePlaying:   v = 1; break;
                    case WKMediaPlaybackStatePaused:    v = 2; break;
                    case WKMediaPlaybackStateSuspended: v = 3; break;
                    default:                            v = 0; break;
                }
                [box addObject:@(v)];
            }];
            NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:0.3];
            while ([box count] == 0 && [deadline timeIntervalSinceNow] > 0) {
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:deadline];
            }
            if ([box count] == 0) return NW_ERR_INVALID;
            return [[box objectAtIndex:0] longLongValue];
        }
#endif
        return NW_ERR_UNSUPPORTED;
    }
}

// The UI layer, created on first use below the content view. The UI page leaves
// a hole, and nw_set_content_insets parks the content view over it.
int64_t nw_set_ui_html(int64_t h, Str *html) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSString *s = nw_nsstring(html);
        if (!s) return NW_ERR_BAD_INPUT;
        if (!w->ui) {
            NioWebHelper *ch = [[NioWebHelper alloc] init];
            ch.win = w;
            ch.isUi = 1;
            WKWebView *cv = nw_make_view(w, ch, [w->container bounds]);
            // The UI view draws no background, so it is glass wherever its page
            // paints nothing. Raised above the content views it keeps the page
            // visible under a popover. drawsBackground is a private key.
            @try {
                [cv setValue:@NO forKey:@"drawsBackground"];
            } @catch (NSException *e) {
                (void)e;
            }
#ifdef NW_HAS_TABSTATE
            if (@available(macOS 12.0, *)) {
                cv.underPageBackgroundColor = [NSColor clearColor];
            }
#endif
            // Below every sibling: the content wraps all sit above the UI.
            [w->container addSubview:cv positioned:NSWindowBelow relativeTo:nil];
            w->ui = cv;
            w->uiHelper = ch;
        }
        [w->ui loadHTMLString:s baseURL:nil];
        return 0;
    }
}

// The subview order for nw_set_ui_front: the UI view topmost while raised,
// bottom while not, every wrap keeping its place.
static NSComparisonResult nw_ui_compare(__kindof NSView *a, __kindof NSView *b, void *ctx) {
    NwWin *w = (NwWin *)ctx;
    if (a == w->ui) return w->uiFront ? NSOrderedDescending : NSOrderedAscending;
    if (b == w->ui) return w->uiFront ? NSOrderedAscending : NSOrderedDescending;
    return NSOrderedSame;
}

// Raise the UI layer above the content views (1), or sink it under them (0).
// While raised the UI page takes every mouse event. Reorder in place with
// sortSubviews: re-parenting a view tears its tracking areas down, and hover
// stays dead until the mouse re-crosses the window.
int64_t nw_set_ui_front(int64_t h, int64_t on) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w || !w->ui) return NW_ERR_INVALID;
        w->uiFront = on ? 1 : 0;    // NioContentView's hover guard reads this
        [w->container sortSubviewsUsingFunction:nw_ui_compare context:w];
        return 0;
    }
}

int64_t nw_ui_eval(int64_t h, Str *js) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w || !w->ui) return NW_ERR_INVALID;
        NSString *s = nw_nsstring(js);
        if (!s) return NW_ERR_BAD_INPUT;
        [w->ui evaluateJavaScript:s completionHandler:nil];
        return 0;
    }
}

// Insets are measured from the window edges, top-down as CSS does. AppKit
// frames are bottom-up, so the conversion happens here once.
int64_t nw_set_content_insets(int64_t h, int64_t top, int64_t left, int64_t bottom, int64_t right) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        if (top < 0 || left < 0 || bottom < 0 || right < 0) return NW_ERR_BAD_INPUT;
        // Remembered as well as applied: a tab created later takes the same frame.
        w->ins[0] = top;
        w->ins[1] = left;
        w->ins[2] = bottom;
        w->ins[3] = right;
        w->has_insets = 1;
        [w->wraps[w->active] setFrame:nw_content_frame(w)];
        return 0;
    }
}

int64_t nw_navigate(int64_t h, Str *url) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSString *s = nw_nsstring(url);
        NSURL *u = s ? [NSURL URLWithString:s] : nil;
        if (!u) return NW_ERR_BAD_INPUT;
        // loadRequest gives no read-access grant for a file URL. Grant the
        // parent directory, so the page's relative assets load.
        if ([u isFileURL]) {
            [w->webview loadFileURL:u
                allowingReadAccessToURL:[u URLByDeletingLastPathComponent]];
            return 0;
        }
        [w->webview loadRequest:[NSURLRequest requestWithURL:u]];
        return 0;
    }
}

int64_t nw_load_html(int64_t h, Str *html) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSString *s = nw_nsstring(html);
        if (!s) return NW_ERR_BAD_INPUT;
        [w->webview loadHTMLString:s baseURL:nil];
        return 0;
    }
}

int64_t nw_eval_js(int64_t h, Str *js) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSString *s = nw_nsstring(js);
        if (!s) return NW_ERR_BAD_INPUT;
        [w->webview evaluateJavaScript:s completionHandler:nil];
        return 0;
    }
}

// Eval-with-answer. The value rides the trusted UI queue as "__evalresult" with
// body "<id>,ok,<string>" or "<id>,err,<message>", and this call answers the id.
// The script must return a string: a JS Date arrives as NSDate, which no JSON
// writer takes. tab 0 means the shown tab, named because the answer arrives late.
static int64_t nw_eval_seq = 0;

int64_t nw_eval_js_result(int64_t h, int64_t tab, Str *js) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        WKWebView *wv = w->webview;
        if (tab != 0) {
            if (tab < 1 || tab > NW_MAX_TABS || !w->tabs[tab - 1]) return NW_ERR_BAD_INPUT;
            wv = w->tabs[tab - 1];
        }
        NSString *s = nw_nsstring(js);
        if (!s) return NW_ERR_BAD_INPUT;
        int64_t evalId = ++nw_eval_seq;
        // The block re-resolves the handle: the window may close before the
        // engine answers, and a freed NwWin must not be touched.
        [wv evaluateJavaScript:s completionHandler:^(id result, NSError *err) {
            NwWin *ww = nw_get(h);
            if (!ww) return;
            if (err) {
                NSString *msg = err.localizedDescription;
                if (!msg) msg = @"evaluation failed";
                nw_push_event(ww, "__evalresult", evalId,
                    [@"err," stringByAppendingString:msg]);
                return;
            }
            if (!result || ![result isKindOfClass:[NSString class]]) {
                nw_push_event(ww, "__evalresult", evalId,
                    @"err,the script must evaluate to a string");
                return;
            }
            nw_push_event(ww, "__evalresult", evalId,
                [@"ok," stringByAppendingString:(NSString *)result]);
        }];
        [s release];
        return evalId;
    }
}

// Search the page of one tab. Tab 0 is the tab on screen. The count arrives
// later on "__find". The same text again moves to the next match. maxCount
// limits the count, not the search.
int64_t nw_find(int64_t h, int64_t tab, Str *text, int64_t flags, int64_t maxCount) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        WKWebView *wv = w->webview;
        if (tab != 0) {
            if (tab < 1 || tab > NW_MAX_TABS || !w->tabs[tab - 1]) return NW_ERR_BAD_INPUT;
            wv = w->tabs[tab - 1];
        }
        if (!wv) return NW_ERR_INVALID;
        if (![wv respondsToSelector:@selector(_findString:options:maxCount:)]) return NW_ERR_INVALID;
        NSString *s = [nw_nsstring(text) autorelease];
        if (!s) return NW_ERR_BAD_INPUT;
        // An empty text is not a search. Use nw_find_clear for a find bar
        // that the user made empty.
        if (!s.length) return NW_ERR_BAD_INPUT;
        if (maxCount < 1) return NW_ERR_BAD_INPUT;
        NSUInteger opts = NW_FIND_OPT_SHOW_OVERLAY | NW_FIND_OPT_SHOW_INDICATOR
            | NW_FIND_OPT_DETERMINE_INDEX;
        if (!(flags & NW_FIND_CASE_SENSITIVE)) opts |= NW_FIND_OPT_CASE_INSENSITIVE;
        if (flags & NW_FIND_BACKWARDS) opts |= NW_FIND_OPT_BACKWARDS;
        if (flags & NW_FIND_WRAP) opts |= NW_FIND_OPT_WRAP;
        [wv _findString:s options:opts maxCount:(NSUInteger)maxCount];
        return 0;
    }
}

// Remove the highlights from the page of one tab. They stay on the page
// until this call, or until the page goes to a new URL.
int64_t nw_find_clear(int64_t h, int64_t tab) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        WKWebView *wv = w->webview;
        if (tab != 0) {
            if (tab < 1 || tab > NW_MAX_TABS || !w->tabs[tab - 1]) return NW_ERR_BAD_INPUT;
            wv = w->tabs[tab - 1];
        }
        if (!wv) return NW_ERR_INVALID;
        if (![wv respondsToSelector:@selector(_hideFindUI)]) return NW_ERR_INVALID;
        [wv _hideFindUI];
        return 0;
    }
}

int64_t nw_set_title(int64_t h, Str *title) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSString *t = nw_nsstring(title);
        if (!t) return NW_ERR_BAD_INPUT;
        [w->window setTitle:t];
        return 0;
    }
}

// The User-Agent the content views send. An empty string restores the engine's
// default, which names the host app. The UI view is not touched.
int64_t nw_set_user_agent(int64_t h, Str *ua) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSString *s = nw_nsstring(ua);
        if (!s) return NW_ERR_BAD_INPUT;
        // Remembered for tabs created later, applied to every live one now.
        // The replay asks nw_ua_for per tab, so a rule's host keeps the
        // rule's string.
        [w->ua release];
        w->ua = s.length ? [s retain] : nil;
        for (int i = 0; i < NW_MAX_TABS; i++) {
            if (w->tabs[i]) w->tabs[i].customUserAgent = nw_ua_for(w, w->tabs[i].URL);
        }
        return 0;
    }
}

// A per-host exception to nw_set_user_agent: main-frame navigations to `host`
// or a subdomain of it send `ua` instead of the window's string. An empty ua
// removes the rule. The check runs in the navigation policy, so the rule
// applies from the next navigation and never mid-load.
int64_t nw_set_ua_rule(int64_t h, Str *host, Str *ua) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSString *hs = [[nw_nsstring(host) autorelease] lowercaseString];
        NSString *us = [nw_nsstring(ua) autorelease];
        if (!hs.length || !us) return NW_ERR_BAD_INPUT;
        if (!w->uaRules) w->uaRules = [[NSMutableDictionary alloc] init];
        if (us.length) {
            [w->uaRules setObject:us forKey:hs];
        } else {
            [w->uaRules removeObjectForKey:hs];
        }
        return 0;
    }
}

// The application's main menu. The Edit menu is necessary: its responder
// actions make Cmd+C/V/X/A work in every text field, web views included. The
// program items carry no target, so they resolve through the responder chain
// and the "__menu" event lands on the key window's queue.
int64_t nw_set_app_menu(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSMenu *main = [[NSMenu alloc] init];

        NSMenuItem *appItem = [[NSMenuItem alloc] init];
        [main addItem:appItem];
        NSMenu *appMenu = [[NSMenu alloc] init];
        NSString *name = [[NSProcessInfo processInfo] processName];
        NSMenuItem *settings = [[NSMenuItem alloc] initWithTitle:@"Settings…"
                                                          action:@selector(nioOpenSettings:)
                                                   keyEquivalent:@","];
        [appMenu addItem:settings];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItem:[[NSMenuItem alloc] initWithTitle:[@"Quit " stringByAppendingString:name]
                                                    action:@selector(terminate:)
                                             keyEquivalent:@"q"]];
        [appItem setSubmenu:appMenu];

        // An empty File menu in the conventional slot. nw_app_menu_item adds a
        // missing menu at the end of the bar, so File items would follow View.
        NSMenuItem *fileItem = [[NSMenuItem alloc] init];
        [main addItem:fileItem];
        [fileItem setSubmenu:[[NSMenu alloc] initWithTitle:@"File"]];

        NSMenuItem *editItem = [[NSMenuItem alloc] init];
        [main addItem:editItem];
        NSMenu *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
        // Reload reports a "__menu" event with body "0,reload".
        NSMenuItem *reload = [[NSMenuItem alloc] initWithTitle:@"Reload"
                                                        action:@selector(nioReload:)
                                                 keyEquivalent:@"r"];
        [edit addItem:reload];
        [edit addItem:[NSMenuItem separatorItem]];
        [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"]];
        [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"Z"]];
        [edit addItem:[NSMenuItem separatorItem]];
        [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"]];
        [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"]];
        [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"]];
        [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"]];
        [editItem setSubmenu:edit];

        // The zoom items report "__menu" events ("0,zoomin" / "0,zoomout" /
        // "0,zoomreset"). A hidden twin answers Cmd+Equals as well as Cmd+Plus,
        // and stays hidden so one command does not list twice.
        NSMenuItem *viewItem = [[NSMenuItem alloc] init];
        [main addItem:viewItem];
        NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
        [view addItem:[[NSMenuItem alloc] initWithTitle:@"Zoom In"
                                                 action:@selector(nioZoomIn:)
                                          keyEquivalent:@"+"]];
        NSMenuItem *zoomInAlt = [[NSMenuItem alloc] initWithTitle:@"Zoom In"
                                                           action:@selector(nioZoomIn:)
                                                    keyEquivalent:@"="];
        [zoomInAlt setHidden:YES];
        if (@available(macOS 10.13, *)) {
            zoomInAlt.allowsKeyEquivalentWhenHidden = YES;
        }
        [view addItem:zoomInAlt];
        [view addItem:[[NSMenuItem alloc] initWithTitle:@"Zoom Out"
                                                 action:@selector(nioZoomOut:)
                                          keyEquivalent:@"-"]];
        [view addItem:[[NSMenuItem alloc] initWithTitle:@"Actual Size"
                                                 action:@selector(nioZoomReset:)
                                          keyEquivalent:@"0"]];
        [viewItem setSubmenu:view];

        [NSApp setMainMenu:main];
        return 0;
    }
}

// One item of the named top-level menu, created at the end of the bar when the
// menu is absent. A pick reports "__menu" with body "0,<id>"; key is the Cmd
// shortcut ("" for none). Re-adding an id retitles it. Call after
// nw_set_app_menu, or there is no main menu to add to.
int64_t nw_app_menu_item(int64_t h, Str *menu, Str *id, Str *title, Str *key) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSString *m = [nw_nsstring(menu) autorelease];
        NSString *i = [nw_nsstring(id) autorelease];
        NSString *t = [nw_nsstring(title) autorelease];
        NSString *k = [nw_nsstring(key) autorelease];
        if (!m || !i || !t || !m.length || !i.length || !t.length) return NW_ERR_BAD_INPUT;
        NSMenu *main = [NSApp mainMenu];
        if (!main) return NW_ERR_INVALID;
        NSMenu *sub = nil;
        for (NSMenuItem *top in main.itemArray) {
            if (top.submenu && [top.submenu.title isEqualToString:m]) {
                sub = top.submenu;
                break;
            }
        }
        if (!sub) {
            NSMenuItem *top = [[[NSMenuItem alloc] init] autorelease];
            sub = [[[NSMenu alloc] initWithTitle:m] autorelease];
            [top setSubmenu:sub];
            [main addItem:top];
        }
        for (NSMenuItem *it in sub.itemArray) {
            if ([it.representedObject isEqual:i]) {
                it.title = t;
                return 0;
            }
        }
        NSMenuItem *item = [[[NSMenuItem alloc] initWithTitle:t
                                                       action:@selector(nioAppMenuItem:)
                                                keyEquivalent:(k ? k : @"")] autorelease];
        item.representedObject = i;
        [sub addItem:item];
        return 0;
    }
}

// A separator line in the named top-level menu. It carries an id like every
// program item, so re-adding the same id is a no-op.
int64_t nw_app_menu_separator(int64_t h, Str *menu, Str *id) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSString *m = [nw_nsstring(menu) autorelease];
        NSString *i = [nw_nsstring(id) autorelease];
        if (!m || !i || !m.length || !i.length) return NW_ERR_BAD_INPUT;
        NSMenu *main = [NSApp mainMenu];
        if (!main) return NW_ERR_INVALID;
        NSMenu *sub = nil;
        for (NSMenuItem *top in main.itemArray) {
            if (top.submenu && [top.submenu.title isEqualToString:m]) {
                sub = top.submenu;
                break;
            }
        }
        if (!sub) {
            NSMenuItem *top = [[[NSMenuItem alloc] init] autorelease];
            sub = [[[NSMenu alloc] initWithTitle:m] autorelease];
            [top setSubmenu:sub];
            [main addItem:top];
        }
        for (NSMenuItem *it in sub.itemArray) {
            if ([it.representedObject isEqual:i]) return 0;
        }
        NSMenuItem *item = [NSMenuItem separatorItem];
        item.representedObject = i;
        [sub addItem:item];
        return 0;
    }
}

// The window's appearance: 0 follows the system, 1 forces light, 2 forces dark.
// Every web view inherits it, so it also decides prefers-color-scheme.
int64_t nw_set_appearance(int64_t h, int64_t mode) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        if (mode == 1) {
            w->window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
        } else if (mode == 2) {
            w->window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        } else {
            w->window.appearance = nil;
        }
        return 0;
    }
}

// WebKit names a profile by UUID and the program by name. nw_name_uuid bridges
// the two, and the extension controller uses the same hash.
static NSUUID *nw_profile_uuid(Str *name) {
    return nw_name_uuid((const unsigned char *)name->data, name->len);
}

// The data store future views are created with. An empty name selects the
// default. A store is fixed at view creation, so existing views do not change.
int64_t nw_set_profile(int64_t h, Str *name) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        if (name->len == 0) {
            [w->store release];
            w->store = nil;
            [w->profileName release];
            w->profileName = nil;
            w->ephemeral = 0;
#ifdef NW_HAS_WEBEXT
            // The extension world is per-profile. Loaded extensions do not
            // survive the switch; the caller reloads them.
            nw_ext_teardown(w);
            nw_ext_setup(w);
#endif
            return 0;
        }
        if (@available(macOS 14.0, *)) {
            WKWebsiteDataStore *s = [WKWebsiteDataStore dataStoreForIdentifier:nw_profile_uuid(name)];
            if (!s) return NW_ERR_INVALID;
            [w->store release];
            w->store = [s retain];
            [w->profileName release];
            w->profileName = nw_nsstring(name);    // +1 from alloc/init, ours
            w->ephemeral = 0;
#ifdef NW_HAS_WEBEXT
            nw_ext_teardown(w);
            nw_ext_setup(w);
#endif
            return 0;
        }
        return NW_ERR_UNSUPPORTED;
    }
}

// The incognito store keeps cookies, storage and cache in memory only. They die
// with the last reference, which windowWillClose: drops. Otherwise the
// nw_set_profile contract holds: only future views carry the store.
int64_t nw_set_incognito(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        [w->store release];
        w->store = [[WKWebsiteDataStore nonPersistentDataStore] retain];
        [w->profileName release];
        w->profileName = nil;
        w->ephemeral = 1;
#ifdef NW_HAS_WEBEXT
        nw_ext_teardown(w);
        nw_ext_setup(w);
#endif
        return 0;
    }
}

// Remove a named profile's engine storage. WebKit works asynchronously and can
// refuse a store a live view holds, so the caller closes those views first.
int64_t nw_delete_profile(int64_t h, Str *name) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        if (name->len == 0) return NW_ERR_BAD_INPUT;
        if (@available(macOS 14.0, *)) {
            [WKWebsiteDataStore removeDataStoreForIdentifier:nw_profile_uuid(name)
                                           completionHandler:^(NSError *err) {
                (void)err;
            }];
            return 0;
        }
        return NW_ERR_UNSUPPORTED;
    }
}

// The smallest content size the user may resize to. AppKit applies a minimum
// only on the next user resize, so a smaller window is grown at once.
int64_t nw_set_min_size(int64_t h, int64_t width, int64_t height) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        if (width < 0 || height < 0) return NW_ERR_BAD_INPUT;
        [w->window setContentMinSize:NSMakeSize((CGFloat)width, (CGFloat)height)];
        NSRect c = [w->window contentRectForFrameRect:w->window.frame];
        if (c.size.width < (CGFloat)width || c.size.height < (CGFloat)height) {
            if (c.size.width < (CGFloat)width) c.size.width = (CGFloat)width;
            if (c.size.height < (CGFloat)height) c.size.height = (CGFloat)height;
            [w->window setFrame:[w->window frameRectForContentRect:c] display:YES];
        }
        return 0;
    }
}

// Print the shown content view through the system print panel, as a sheet.
// runOperationModalForWindow returns at once and the event pump drives it.
// WKWebView hands back a print view sized zero, which paginates to nothing, so
// give the view a real frame first.
int64_t nw_print_page(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        if (@available(macOS 11.0, *)) {
            NSPrintInfo *info = [[[NSPrintInfo sharedPrintInfo] copy] autorelease];
            info.horizontalPagination = NSPrintingPaginationModeFit;
            info.verticalPagination = NSPrintingPaginationModeAutomatic;
            NSPrintOperation *op = [w->webview printOperationWithPrintInfo:info];
            if (!op) return NW_ERR_INVALID;
            op.showsPrintPanel = YES;
            op.showsProgressPanel = YES;
            [op.view setFrame:[w->webview bounds]];
            [op runOperationModalForWindow:w->window
                                  delegate:nil
                            didRunSelector:NULL
                               contextInfo:NULL];
            return 0;
        }
        return NW_ERR_UNSUPPORTED;
    }
}

// Developer tools on every content view, current and future. The UI view is
// left alone.
int64_t nw_set_devtools(int64_t h, int64_t on) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        w->devtools = on ? 1 : 0;
        for (int i = 0; i < NW_MAX_TABS; i++) {
            if (w->tabs[i]) nw_apply_devtools(w->tabs[i], w->devtools);
        }
        return 0;
    }
}

// Let the content views talk to the program (window.nio). Call once, before
// anything loads: a user script takes effect only on the next load. Off pulls
// the message handler out and leaves the shim, because WebKit can only drop
// every user script at once.
int64_t nw_set_page_bridge(int64_t h, int64_t on) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        w->pageBridge = on ? 1 : 0;
        for (int i = 0; i < NW_MAX_TABS; i++) {
            if (!w->tabs[i]) continue;
            // .configuration answers a copy, but the copy names the same
            // WKUserContentController the live view is reading.
            WKWebViewConfiguration *cfg = w->tabs[i].configuration;
            if (w->pageBridge) {
                nw_add_bridge(w, cfg, w->helper);
            } else {
                [cfg.userContentController removeScriptMessageHandlerForName:@"nio"];
            }
        }
        return 0;
    }
}

// Give the content views a working navigator.geolocation. Same contract as
// nw_set_page_bridge: call once before anything loads, and off leaves the shim
// with nothing behind it, so every ask fails.
int64_t nw_set_geolocation(int64_t h, int64_t on) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        w->geoOn = on ? 1 : 0;
        for (int i = 0; i < NW_MAX_TABS; i++) {
            if (!w->tabs[i]) continue;
            WKWebViewConfiguration *cfg = w->tabs[i].configuration;
            if (w->geoOn) {
                nw_add_geo(w, cfg, w->helper);
            } else {
                [cfg.userContentController removeScriptMessageHandlerForName:@"nioGeo"];
            }
        }
        return 0;
    }
}

// Rename one of the engine's own context-menu items, found by the identifier
// WebKit stamps on it ("WKMenuItemIdentifierOpenLinkInNewWindow" and family).
// An unknown identifier never matches. Re-adding one replaces its title.
int64_t nw_ctx_retitle(int64_t h, Str *id, Str *title) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSString *i = [nw_nsstring(id) autorelease];
        NSString *t = [nw_nsstring(title) autorelease];
        if (!i || !i.length || !t || !t.length) return NW_ERR_BAD_INPUT;
        if (!w->ctxRetitles) w->ctxRetitles = [[NSMutableDictionary alloc] init];
        w->ctxRetitles[i] = t;
        return 0;
    }
}

// Remove one of the engine's own context-menu items, by the identifiers
// nw_ctx_retitle matches on. An unknown identifier never matches.
int64_t nw_ctx_hide(int64_t h, Str *id) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSString *i = [nw_nsstring(id) autorelease];
        if (!i || !i.length) return NW_ERR_BAD_INPUT;
        if (!w->ctxHides) w->ctxHides = [[NSMutableSet alloc] init];
        [w->ctxHides addObject:i];
        return 0;
    }
}

// Take over the pick of one of the engine's own context-menu items. The item
// stays where the engine put it, a pick reports "__ctx" with the identifier as
// the id, and the engine's own action never runs. Some engine actions leave the
// program: "Search with Google" opens the default browser.
int64_t nw_ctx_capture(int64_t h, Str *id) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSString *i = [nw_nsstring(id) autorelease];
        if (!i || !i.length) return NW_ERR_BAD_INPUT;
        if (!w->ctxCaptures) w->ctxCaptures = [[NSMutableSet alloc] init];
        [w->ctxCaptures addObject:i];
        return 0;
    }
}

// Register one context-menu item for the content views. A pick reports "__ctx".
// Items accumulate in the order added and there is no removal.
int64_t nw_context_menu_item(int64_t h, Str *id, Str *title) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSString *i = [nw_nsstring(id) autorelease];
        NSString *t = [nw_nsstring(title) autorelease];
        if (!i || !i.length || !t || !t.length) return NW_ERR_BAD_INPUT;
        if (!w->ctxItems) w->ctxItems = [[NSMutableArray alloc] init];
        [w->ctxItems addObject:@{ @"id": i, @"title": t }];
        return 0;
    }
}

// The unified-titlebar look: the title bar turns transparent and the content
// extends under it. The UI page must leave room for the traffic lights and must
// call nw_start_drag, because a web view swallows the mouse.
int64_t nw_set_inline_titlebar(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        w->inlineTitlebar = 1;
        NSWindow *win = w->window;
        win.styleMask |= NSWindowStyleMaskFullSizeContentView;
        win.titlebarAppearsTransparent = YES;
        win.titleVisibility = NSWindowTitleHidden;
        // An empty unified toolbar moves the traffic lights: AppKit centers them
        // in the taller titlebar it reserves for a toolbar. Nothing is drawn.
        if (@available(macOS 11.0, *)) {
            NSToolbar *tb = [[NSToolbar alloc] initWithIdentifier:@"nw-inline-titlebar"];
            tb.showsBaselineSeparator = NO;
            tb.allowsUserCustomization = NO;
            win.toolbar = tb;
            win.toolbarStyle = NSWindowToolbarStyleUnified;
        }
        return 0;
    }
}

// Begin a window drag. A web view eats every mouse event, so the page sends a
// channel message on mousedown and this hands the gesture to AppKit.
int64_t nw_start_drag(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSWindow *win = w->window;
        // The ask crossed the message queue, so [NSApp currentEvent] is a later
        // event whose location would teleport the window. Build a fresh
        // mousedown at the real mouse position, in window coordinates.
        NSPoint p = [win mouseLocationOutsideOfEventStream];
        NSEvent *ev = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                         location:p
                                    modifierFlags:0
                                        timestamp:[[NSProcessInfo processInfo] systemUptime]
                                     windowNumber:win.windowNumber
                                          context:nil
                                      eventNumber:0
                                       clickCount:1
                                         pressure:1.0];
        if (ev) [win performWindowDragWithEvent:ev];
        return 0;
    }
}

// Zoom the window. The zooming flag lets this ask through windowShouldZoom: on
// an inline-titlebar window, where the gesture is refused.
int64_t nw_zoom(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        w->zooming = 1;
        [w->window performZoom:nil];
        w->zooming = 0;
        return 0;
    }
}

// The system character palette. It types into the first responder, so the
// caller focuses its text field first.
int64_t nw_emoji_picker(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        [NSApp orderFrontCharacterPalette:nil];
        return 0;
    }
}

// Where the platform drew its window buttons, in CSS pixels of a page filling
// the window (origin top-left). buttons_right is the right edge of the
// rightmost button; center_y is the buttons' vertical center.
int64_t nw_titlebar_buttons_right(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        [w->window layoutIfNeeded];
        NSButton *b = [w->window standardWindowButton:NSWindowZoomButton];
        if (!b) return NW_ERR_INVALID;
        NSRect f = [b convertRect:b.bounds toView:nil];
        return (int64_t)(f.origin.x + f.size.width + 0.5);
    }
}

int64_t nw_titlebar_center_y(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        [w->window layoutIfNeeded];
        NSButton *b = [w->window standardWindowButton:NSWindowCloseButton];
        if (!b) return NW_ERR_INVALID;
        NSRect f = [b convertRect:b.bounds toView:nil];
        double topCSS = w->window.frame.size.height - (f.origin.y + f.size.height);
        return (int64_t)(topCSS + f.size.height / 2.0 + 0.5);
    }
}

// The content view's session history. No back entry is a no-op, not an error.
int64_t nw_go_back(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        [w->webview goBack];
        return 0;
    }
}

int64_t nw_go_forward(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        [w->webview goForward];
        return 0;
    }
}

int64_t nw_can_back(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        return w->webview.canGoBack ? 1 : 0;
    }
}

int64_t nw_can_forward(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        return w->webview.canGoForward ? 1 : 0;
    }
}

int64_t nw_reload(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        [w->webview reload];
        return 0;
    }
}

// ---- the download calls ----

// Where downloads land: an absolute directory, or "" for the platform's own
// Downloads folder. A transfer already running keeps its path.
int64_t nw_set_download_dir(int64_t h, Str *dir) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSString *s = nw_nsstring(dir);
        if (!s) return NW_ERR_BAD_INPUT;
        [w->downloadDir release];
        if (s.length) {
            w->downloadDir = s;    // +1 from alloc/init, ours
        } else {
            w->downloadDir = nil;
            [s release];
        }
        return 0;
    }
}

// The directory downloads land in: the set one, else the platform's Downloads
// folder. Answered even where WKDownload is too old to download.
Str *nw_download_dir(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return rt_str_alloc(0);
        NSString *dir = nw_download_dir_of(w);
        const char *utf8 = dir.UTF8String;
        if (!utf8) return rt_str_alloc(0);
        size_t len = strlen(utf8);
        Str *s = rt_str_alloc((int64_t)len);
        memcpy(s->data, utf8, len);
        return s;
    }
}

// Every download this window has seen, as a JSON array of
// { id, state, file, path, url, received, total, error }. "[]" where downloads
// are unsupported.
Str *nw_downloads(int64_t h) {
    @autoreleasepool {
        NSString *out = @"[]";
#ifdef NW_HAS_DOWNLOAD
        NwWin *w = nw_get(h);
        if (w) {
            if (@available(macOS 11.3, *)) {
                NSMutableArray *list = [NSMutableArray array];
                for (int i = 0; i < NW_MAX_DOWNLOADS; i++) {
                    if (w->dls[i].used) [list addObject:nw_dl_dict(&w->dls[i])];
                }
                out = nw_json(list);
            }
        }
#else
        (void)h;
#endif
        const char *utf8 = out.UTF8String;
        size_t len = strlen(utf8);
        Str *s = rt_str_alloc((int64_t)len);
        memcpy(s->data, utf8, len);
        return s;
    }
}

// Stop a download. It settles at once as cancelled, and nw_dl_settle's guard
// drops the didFailWithError the engine's cancel also sends.
int64_t nw_download_cancel(int64_t h, int64_t id) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
#ifdef NW_HAS_DOWNLOAD
        if (@available(macOS 11.3, *)) {
            NwDl *d = nw_dl_by_id(w, id);
            if (!d) return NW_ERR_BAD_INPUT;
            if (d->state != NW_DL_ACTIVE) return 0;
            [d->dl cancel:^(NSData *resumeData) { (void)resumeData; }];
            nw_dl_settle(w, d, NW_DL_CANCELLED, nil);
            return 0;
        }
#else
        (void)id;
#endif
        return NW_ERR_UNSUPPORTED;
    }
}

// Open a path the way a double-click in the file browser does. It takes no
// window handle: the file system belongs to the machine.
int64_t nw_open_path(Str *path) {
    @autoreleasepool {
        NSString *p = [nw_nsstring(path) autorelease];
        if (!p || !p.length) return NW_ERR_BAD_INPUT;
        if (![[NSFileManager defaultManager] fileExistsAtPath:p]) return NW_ERR_BAD_INPUT;
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:p]];
        return 0;
    }
}

// Show a path in the platform's file browser. A directory opens as a folder
// window, a file is revealed selected in its folder.
int64_t nw_reveal_path(Str *path) {
    @autoreleasepool {
        NSString *p = [nw_nsstring(path) autorelease];
        if (!p || !p.length) return NW_ERR_BAD_INPUT;
        BOOL isDir = NO;
        if (![[NSFileManager defaultManager] fileExistsAtPath:p isDirectory:&isDir]) {
            return NW_ERR_BAD_INPUT;
        }
        if (isDir) {
            [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:p isDirectory:YES]];
        } else {
            [[NSWorkspace sharedWorkspace]
                activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:p] ]];
        }
        return 0;
    }
}

// ---- the extension calls ----
//
// Each one answers NW_ERR_UNSUPPORTED below macOS 15.4, or when the build SDK
// predates the classes.

// The last extension failure's prose. WebKit's NSError says why a manifest was
// refused, and NW_ERR_BAD_INPUT alone would lose it.
static char nw_ext_err[512];

#ifdef NW_HAS_WEBEXT

static void nw_ext_set_error(NSError *err, const char *fallback) {
    const char *msg = fallback;
    if (err && err.localizedDescription) msg = err.localizedDescription.UTF8String;
    if (!msg) msg = fallback;
    snprintf(nw_ext_err, sizeof nw_ext_err, "%s", msg);
}

// Spin the run loop until an extension completion lands. Nothing here pumps
// NSEvents, so no Nio handler can run under the wait.
static void nw_ext_wait(volatile int *done) {
    while (!*done) {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    }
}

#endif  /* NW_HAS_WEBEXT */

// The Safari web extensions on this machine, as a JSON array of { path, name }.
// It scans /Applications and ~/Applications. path is the .app, which is a form
// nw_ext_load takes.
Str *nw_ext_installed(void) {
    @autoreleasepool {
        NSMutableArray *list = [NSMutableArray array];
        NSFileManager *fm = [NSFileManager defaultManager];
        NSArray *roots = @[ @"/Applications",
                            [NSHomeDirectory() stringByAppendingPathComponent:@"Applications"] ];
        for (NSString *root in roots) {
            NSArray *apps = [fm contentsOfDirectoryAtPath:root error:NULL];
            for (NSString *app in apps) {
                if (![app.pathExtension isEqualToString:@"app"]) continue;
                NSString *appPath = [root stringByAppendingPathComponent:app];
                if (!nw_safari_appex(appPath)) continue;
                NSBundle *b = [NSBundle bundleWithPath:appPath];
                NSString *name = [b objectForInfoDictionaryKey:@"CFBundleDisplayName"];
                if (![name isKindOfClass:[NSString class]] || !name.length) {
                    name = [b objectForInfoDictionaryKey:@"CFBundleName"];
                }
                if (![name isKindOfClass:[NSString class]] || !name.length) {
                    name = [app stringByDeletingPathExtension];
                }
                [list addObject:@{ @"path": appPath, @"name": name }];
            }
        }
        NSString *out = nw_json(list);
        const char *utf8 = out.UTF8String;
        size_t len = strlen(utf8);
        Str *s = rt_str_alloc((int64_t)len);
        memcpy(s->data, utf8, len);
        return s;
    }
}

// The platform's open panel: one file or directory, or "" when the user
// cancels. The call is app-modal and blocks until the panel closes.
Str *nw_pick_path(void) {
    @autoreleasepool {
        nw_app_init();
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.canChooseFiles = YES;
        p.canChooseDirectories = YES;
        p.allowsMultipleSelection = NO;
        NSString *res = @"";
        if ([p runModal] == NSModalResponseOK && p.URL) {
            res = p.URL.path;
        }
        const char *utf8 = res.UTF8String;
        size_t len = strlen(utf8);
        Str *s = rt_str_alloc((int64_t)len);
        memcpy(s->data, utf8, len);
        return s;
    }
}

int64_t nw_ext_load(int64_t h, Str *path) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
#ifdef NW_HAS_WEBEXT
        if (@available(macOS 15.4, *)) {
            NSString *p = [nw_nsstring(path) autorelease];
            if (!p || !p.length) return NW_ERR_BAD_INPUT;
            if (![p isAbsolutePath]) {
                p = [[[NSFileManager defaultManager] currentDirectoryPath]
                        stringByAppendingPathComponent:p];
            }
            p = [p stringByStandardizingPath];
            nw_ext_setup(w);
            int slot = -1;
            for (int i = 0; i < NW_MAX_EXTS; i++) {
                if (!w->exts[i].used) { slot = i; break; }
            }
            if (slot < 0) return NW_ERR_FULL;

            // Four forms are accepted: a container .app, a bare .appex, and,
            // through resourceBaseURL, an unpacked directory or a .zip.
            NSBundle *bundle = nil;
            if ([p.pathExtension isEqualToString:@"app"]) {
                bundle = nw_safari_appex(p);
                if (!bundle) {
                    nw_ext_set_error(nil, "no Safari web extension inside the app");
                    return NW_ERR_BAD_INPUT;
                }
            } else if ([p.pathExtension isEqualToString:@"appex"]) {
                bundle = [NSBundle bundleWithPath:p];
                if (!bundle) {
                    nw_ext_set_error(nil, "cannot open the extension bundle");
                    return NW_ERR_BAD_INPUT;
                }
            }
            __block WKWebExtension *ext = nil;
            __block NSError *lerr = nil;
            __block volatile int done = 0;
            void (^got)(WKWebExtension *, NSError *) = ^(WKWebExtension *e, NSError *err) {
                ext = [e retain];
                lerr = [err retain];
                done = 1;
            };
            if (bundle) {
                [WKWebExtension extensionWithAppExtensionBundle:bundle completionHandler:got];
            } else {
                [WKWebExtension extensionWithResourceBaseURL:[NSURL fileURLWithPath:p]
                                           completionHandler:got];
            }
            nw_ext_wait(&done);
            [ext autorelease];
            [lerr autorelease];
            if (!ext) {
                nw_ext_set_error(lerr, "cannot parse the extension");
                return NW_ERR_BAD_INPUT;
            }

            WKWebExtensionContext *ctx = [WKWebExtensionContext contextForExtension:ext];
            // Use Safari's base-URL scheme: extension login services reject the
            // webkit-extension:// WebKit gives an embedder. The host is stable
            // per (profile, extension path), so the origin's site data survives
            // a restart. A reused base URL also stops WebKit from starting an
            // MV3 service worker, so the background loads by hand below.
            static int schemeRegistered = 0;
            if (!schemeRegistered) {
                schemeRegistered = 1;
                [WKWebExtensionMatchPattern registerCustomURLScheme:@"safari-web-extension"];
            }
            NSString *seed = [NSString stringWithFormat:@"%@|%@",
                w->profileName ? w->profileName : @"", p];
            const char *seedUtf8 = seed.UTF8String;
            NSString *stableId = [nw_name_uuid((const unsigned char *)seedUtf8,
                                               (int64_t)strlen(seedUtf8)) UUIDString].lowercaseString;
            ctx.baseURL = [NSURL URLWithString:[NSString stringWithFormat:
                @"safari-web-extension://%@/", stableId]];
            // The controller keys chrome.storage by this identifier, and
            // browser.runtime.id answers it. Its default follows the default
            // base URL, so overriding baseURL alone leaves it fresh at every
            // launch and chrome.storage comes back empty. Set it before load.
            ctx.uniqueIdentifier = stableId;
            // Grant everything the manifest requests at load. The program
            // narrows with nw_ext_set_permission, and run-time asks still
            // prompt through __extperm. Use allRequestedMatchPatterns: a
            // content script's own `matches` appear only in that union.
            for (NSString *perm in ext.requestedPermissions) {
                [ctx setPermissionStatus:WKWebExtensionContextPermissionStatusGrantedExplicitly
                           forPermission:perm];
            }
            for (WKWebExtensionMatchPattern *pat in ext.allRequestedMatchPatterns) {
                [ctx setPermissionStatus:WKWebExtensionContextPermissionStatusGrantedExplicitly
                         forMatchPattern:pat];
            }
            NSError *loadErr = nil;
            if (![w->extc loadExtensionContext:ctx error:&loadErr]) {
                nw_ext_set_error(loadErr, "cannot load the extension");
                return NW_ERR_BAD_INPUT;
            }
            w->exts[slot].used = 1;
            w->exts[slot].ext = [ext retain];
            w->exts[slot].ctx = [ctx retain];
            int64_t id = (int64_t)slot + 1;
            NSString *name = ext.displayName ? ext.displayName : @"";
            NSString *version = ext.displayVersion ? ext.displayVersion : @"";
            // __extloaded lands when the background content is up, because that
            // is when the extension runs.
            if (ext.hasBackgroundContent) {
                [ctx loadBackgroundContentWithCompletionHandler:^(NSError *bgErr) {
                    if (!w->exts[slot].used) return;    // unloaded meanwhile
                    nw_push_event(w, "__extloaded", id,
                        nw_json(@{ @"name": name, @"version": version,
                                   @"error": bgErr ? bgErr.localizedDescription : @"" }));
                }];
            } else {
                nw_push_event(w, "__extloaded", id,
                    nw_json(@{ @"name": name, @"version": version, @"error": @"" }));
            }
            return id;
        }
#else
        (void)path;
#endif
        return NW_ERR_UNSUPPORTED;
    }
}

int64_t nw_ext_unload(int64_t h, int64_t ext) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
#ifdef NW_HAS_WEBEXT
        if (@available(macOS 15.4, *)) {
            if (ext < 1 || ext > NW_MAX_EXTS || !w->exts[ext - 1].used) return NW_ERR_BAD_INPUT;
            [w->extc unloadExtensionContext:w->exts[ext - 1].ctx error:NULL];
            [w->exts[ext - 1].ctx release];
            [w->exts[ext - 1].ext release];
            w->exts[ext - 1].used = 0;
            w->exts[ext - 1].ctx = nil;
            w->exts[ext - 1].ext = nil;
            return 0;
        }
#else
        (void)ext;
#endif
        return NW_ERR_UNSUPPORTED;
    }
}

// The loaded extensions, as a JSON array of { id, name, version, url }. "[]"
// where extensions are unsupported.
Str *nw_ext_list(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        NSString *out = @"[]";
#ifdef NW_HAS_WEBEXT
        if (w) {
            if (@available(macOS 15.4, *)) {
                NSMutableArray *list = [NSMutableArray array];
                for (int i = 0; i < NW_MAX_EXTS; i++) {
                    if (!w->exts[i].used) continue;
                    WKWebExtension *e = w->exts[i].ext;
                    NSURL *base = w->exts[i].ctx.baseURL;
                    [list addObject:@{ @"id": @(i + 1),
                                       @"name": e.displayName ? e.displayName : @"",
                                       @"version": e.displayVersion ? e.displayVersion : @"",
                                       @"url": base ? base.absoluteString : @"" }];
                }
                out = nw_json(list);
            }
        }
#endif
        const char *utf8 = out.UTF8String;
        size_t len = strlen(utf8);
        Str *s = rt_str_alloc((int64_t)len);
        memcpy(s->data, utf8, len);
        return s;
    }
}

// Grant (1), deny (-1) or reset (0) one permission or host match pattern. A
// match pattern holds "://" or is "<all_urls>"; a permission name never does.
int64_t nw_ext_set_permission(int64_t h, int64_t ext, Str *perm, int64_t status) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
#ifdef NW_HAS_WEBEXT
        if (@available(macOS 15.4, *)) {
            if (ext < 1 || ext > NW_MAX_EXTS || !w->exts[ext - 1].used) return NW_ERR_BAD_INPUT;
            NSString *p = [nw_nsstring(perm) autorelease];
            if (!p || !p.length) return NW_ERR_BAD_INPUT;
            WKWebExtensionContextPermissionStatus st = WKWebExtensionContextPermissionStatusUnknown;
            if (status > 0) st = WKWebExtensionContextPermissionStatusGrantedExplicitly;
            if (status < 0) st = WKWebExtensionContextPermissionStatusDeniedExplicitly;
            WKWebExtensionContext *ctx = w->exts[ext - 1].ctx;
            if ([p containsString:@"://"] || [p isEqualToString:@"<all_urls>"]) {
                WKWebExtensionMatchPattern *pat = [WKWebExtensionMatchPattern matchPatternWithString:p];
                if (!pat) return NW_ERR_BAD_INPUT;
                [ctx setPermissionStatus:st forMatchPattern:pat];
            } else {
                [ctx setPermissionStatus:st forPermission:p];
            }
            return 0;
        }
#else
        (void)ext; (void)perm; (void)status;
#endif
        return NW_ERR_UNSUPPORTED;
    }
}

// The toolbar button was clicked, at this rect of the UI page (CSS pixels,
// top-left origin). The rect is remembered to anchor the popup.
int64_t nw_ext_action_click(int64_t h, int64_t ext, int64_t x, int64_t y, int64_t wpx, int64_t hpx) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
#ifdef NW_HAS_WEBEXT
        if (@available(macOS 15.4, *)) {
            if (ext < 1 || ext > NW_MAX_EXTS || !w->exts[ext - 1].used) return NW_ERR_BAD_INPUT;
            // A second click on the button dismisses its popup. Only this side
            // knows the popover still shows: a click-away reports no event.
            if (w->extPopover && w->extPopover.shown && w->extPopoverExt == ext) {
                nw_ext_popup_dismiss(w);
                return 0;
            }
            w->extRect[0] = x;
            w->extRect[1] = y;
            w->extRect[2] = wpx;
            w->extRect[3] = hpx;
            WKWebExtensionContext *ctx = w->exts[ext - 1].ctx;
            id <WKWebExtensionTab> tab = w->tabs[w->active] ? (id)nw_ext_tab(w, w->active) : nil;
            [ctx performActionForTab:tab];
            return 0;
        }
#else
        (void)ext; (void)x; (void)y; (void)wpx; (void)hpx;
#endif
        return NW_ERR_UNSUPPORTED;
    }
}

// A tab for an extension's own pages. An extension URL only loads in a view
// built from the extension context's configuration, so this is nw_tab_new with
// that configuration. The answer is an ordinary tab handle.
int64_t nw_ext_tab_new(int64_t h, int64_t ext) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
#ifdef NW_HAS_WEBEXT
        if (@available(macOS 15.4, *)) {
            if (ext < 1 || ext > NW_MAX_EXTS || !w->exts[ext - 1].used) return NW_ERR_BAD_INPUT;
            WKWebViewConfiguration *cfg = [[w->exts[ext - 1].ctx.webViewConfiguration copy] autorelease];
            if (!cfg) return NW_ERR_INVALID;
            // The same bridge every content view gets, so an extension page
            // reports navigations and titles like any tab.
            nw_add_bridge(w, cfg, w->helper);
            nw_add_geo(w, cfg, w->helper);
            WKWebView *wv = nw_new_content_view(w, cfg, nw_content_frame(w));
            int slot = nw_adopt_view(w, wv);
            if (slot < 0) { [wv release]; return NW_ERR_FULL; }
            return (int64_t)slot + 1;
        }
#else
        (void)ext;
#endif
        return NW_ERR_UNSUPPORTED;
    }
}

// The program's answer to a parked delegate decision (__extperm, __extnewtab,
// __perm). An unknown id is BAD_INPUT: a decision answers only once.
static int64_t nw_reply(int64_t h, int64_t req, Str *answer) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
        NSString *a = [nw_nsstring(answer) autorelease];
        if (!w->replies) return NW_ERR_BAD_INPUT;
        void (^cb)(NSString *) = [w->replies objectForKey:@(req)];
        if (!cb) return NW_ERR_BAD_INPUT;
        cb = [[cb retain] autorelease];
        [w->replies removeObjectForKey:@(req)];
        cb(a ? a : @"");
        return 0;
    }
}

int64_t nw_ext_reply(int64_t h, int64_t req, Str *answer) {
    return nw_reply(h, req, answer);
}

int64_t nw_perm_reply(int64_t h, int64_t req, Str *answer) {
    return nw_reply(h, req, answer);
}

// Close the presented popup. A no-op when none is up.
int64_t nw_ext_popup_close(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w) return NW_ERR_INVALID;
#ifdef NW_HAS_WEBEXT
        if (@available(macOS 15.4, *)) {
            nw_ext_popup_dismiss(w);
            return 0;
        }
#endif
        return NW_ERR_UNSUPPORTED;
    }
}

// The action's icon, or the extension's, as a data: URI. "" when there is none.
Str *nw_ext_icon(int64_t h, int64_t ext, int64_t size) {
    @autoreleasepool {
        NSString *uri = @"";
#ifdef NW_HAS_WEBEXT
        NwWin *w = nw_get(h);
        if (w) {
            if (@available(macOS 15.4, *)) {
                if (ext >= 1 && ext <= NW_MAX_EXTS && w->exts[ext - 1].used) {
                    WKWebExtensionContext *ctx = w->exts[ext - 1].ctx;
                    NSImage *img = [[ctx actionForTab:nil] iconForSize:NSMakeSize(size, size)];
                    if (!img) img = [w->exts[ext - 1].ext iconForSize:NSMakeSize(size, size)];
                    if (img) {
                        NSRect r = NSMakeRect(0, 0, (CGFloat)size, (CGFloat)size);
                        CGImageRef cg = [img CGImageForProposedRect:&r context:nil hints:nil];
                        if (cg) {
                            NSBitmapImageRep *rep = [[[NSBitmapImageRep alloc] initWithCGImage:cg] autorelease];
                            NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                            if (png) {
                                uri = [@"data:image/png;base64,"
                                    stringByAppendingString:[png base64EncodedStringWithOptions:0]];
                            }
                        }
                    }
                }
            }
        }
#else
        (void)h; (void)ext; (void)size;
#endif
        const char *utf8 = uri.UTF8String;
        size_t len = strlen(utf8);
        Str *s = rt_str_alloc((int64_t)len);
        memcpy(s->data, utf8, len);
        return s;
    }
}

// Why the last nw_ext_load or nw_ext_set_permission answered BAD_INPUT.
Str *nw_ext_last_error(void) {
    size_t len = strlen(nw_ext_err);
    Str *s = rt_str_alloc((int64_t)len);
    memcpy(s->data, nw_ext_err, len);
    return s;
}

// Pump the event loop for at most timeout_ms. nextEventMatchingMask runs the
// thread's run loop, which services WebKit's timers and its IPC. After the
// first event the deadline drops to distantPast, so it never waits twice.
// nw_wake posts a synthetic event, so a queued page message ends the wait.
int64_t nw_step(int64_t h, int64_t timeout_ms) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w || w->closed) return 0;
        NSDate *deadline =
            [NSDate dateWithTimeIntervalSinceNow:(NSTimeInterval)timeout_ms / 1000.0];
        for (;;) {
            NSEvent *ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:deadline
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES];
            if (!ev) break;
            [NSApp sendEvent:ev];
            deadline = [NSDate distantPast];
        }
        return w->closed ? 0 : 1;
    }
}

int64_t nw_has_message(int64_t h) {
    NwWin *w = nw_get(h);
    return (w && w->head) ? 1 : 0;
}

Str *nw_take_message(int64_t h) {
    NwWin *w = nw_get(h);
    if (!w) return rt_str_alloc(0);
    return nw_pop_msg(&w->head, &w->tail);
}

int64_t nw_has_ui_message(int64_t h) {
    NwWin *w = nw_get(h);
    return (w && w->uhead) ? 1 : 0;
}

Str *nw_take_ui_message(int64_t h) {
    NwWin *w = nw_get(h);
    if (!w) return rt_str_alloc(0);
    return nw_pop_msg(&w->uhead, &w->utail);
}

void nw_close(int64_t h) {
    @autoreleasepool {
        NwWin *w = nw_get(h);
        if (!w || w->closed) return;
        [w->window close];    // the delegate records the close
    }
}

// Contents/Resources when this process runs from an .app bundle, else "".
// NSBundle answers an object for a bare binary too, so the test is the
// CFBundleIdentifier and not the path shape.
Str *nw_resources_path(void) {
    @autoreleasepool {
        NSBundle *b = [NSBundle mainBundle];
        if (!b || ![b objectForInfoDictionaryKey:@"CFBundleIdentifier"]) return rt_str_alloc(0);
        NSString *p = [b resourcePath];
        if (!p) return rt_str_alloc(0);
        const char *utf8 = [p UTF8String];
        size_t len = strlen(utf8);
        Str *out = rt_str_alloc((int64_t)len);
        memcpy(out->data, utf8, len);
        return out;
    }
}

#else  /* not __APPLE__: stubs; no other platform has a body */

int64_t nw_create(Str *title, int64_t width, int64_t height) {
    (void)title; (void)width; (void)height;
    return NW_ERR_UNSUPPORTED;
}
int64_t nw_navigate(int64_t h, Str *url) { (void)h; (void)url; return NW_ERR_INVALID; }
int64_t nw_load_html(int64_t h, Str *html) { (void)h; (void)html; return NW_ERR_INVALID; }
int64_t nw_eval_js(int64_t h, Str *js) { (void)h; (void)js; return NW_ERR_INVALID; }
int64_t nw_eval_js_result(int64_t h, int64_t tab, Str *js) {
    (void)h; (void)tab; (void)js;
    return NW_ERR_INVALID;
}
int64_t nw_find(int64_t h, int64_t tab, Str *text, int64_t flags, int64_t maxCount) {
    (void)h; (void)tab; (void)text; (void)flags; (void)maxCount;
    return NW_ERR_INVALID;
}
int64_t nw_find_clear(int64_t h, int64_t tab) { (void)h; (void)tab; return NW_ERR_INVALID; }
int64_t nw_set_title(int64_t h, Str *title) { (void)h; (void)title; return NW_ERR_INVALID; }
int64_t nw_set_user_agent(int64_t h, Str *ua) { (void)h; (void)ua; return NW_ERR_INVALID; }
int64_t nw_set_ua_rule(int64_t h, Str *host, Str *ua) {
    (void)h; (void)host; (void)ua;
    return NW_ERR_INVALID;
}
int64_t nw_set_profile(int64_t h, Str *name) { (void)h; (void)name; return NW_ERR_INVALID; }
int64_t nw_set_incognito(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_delete_profile(int64_t h, Str *name) { (void)h; (void)name; return NW_ERR_INVALID; }
int64_t nw_set_min_size(int64_t h, int64_t width, int64_t height) {
    (void)h; (void)width; (void)height;
    return NW_ERR_INVALID;
}
int64_t nw_print_page(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_set_devtools(int64_t h, int64_t on) { (void)h; (void)on; return NW_ERR_INVALID; }
int64_t nw_set_page_bridge(int64_t h, int64_t on) { (void)h; (void)on; return NW_ERR_INVALID; }
int64_t nw_context_menu_item(int64_t h, Str *id, Str *title) {
    (void)h; (void)id; (void)title;
    return NW_ERR_INVALID;
}
int64_t nw_ctx_retitle(int64_t h, Str *id, Str *title) {
    (void)h; (void)id; (void)title;
    return NW_ERR_INVALID;
}
int64_t nw_ctx_hide(int64_t h, Str *id) {
    (void)h; (void)id;
    return NW_ERR_INVALID;
}
int64_t nw_ctx_capture(int64_t h, Str *id) {
    (void)h; (void)id;
    return NW_ERR_INVALID;
}
int64_t nw_set_app_menu(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_app_menu_item(int64_t h, Str *menu, Str *id, Str *title, Str *key) {
    (void)h; (void)menu; (void)id; (void)title; (void)key;
    return NW_ERR_INVALID;
}
int64_t nw_app_menu_separator(int64_t h, Str *menu, Str *id) {
    (void)h; (void)menu; (void)id;
    return NW_ERR_INVALID;
}
int64_t nw_set_appearance(int64_t h, int64_t mode) { (void)h; (void)mode; return NW_ERR_INVALID; }
int64_t nw_tab_new(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_tab_show(int64_t h, int64_t tab) { (void)h; (void)tab; return NW_ERR_INVALID; }
int64_t nw_tab_focus(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_ui_focus(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_ui_focused(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_set_backspace_nav(int64_t h, int64_t on) {
    (void)h; (void)on;
    return NW_ERR_INVALID;
}
int64_t nw_stop_loading(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_tab_close(int64_t h, int64_t tab) { (void)h; (void)tab; return NW_ERR_INVALID; }
Str *nw_tab_state(int64_t h, int64_t tab) { (void)h; (void)tab; return rt_str_alloc(0); }
int64_t nw_tab_set_state(int64_t h, int64_t tab, Str *state) {
    (void)h; (void)tab; (void)state;
    return NW_ERR_INVALID;
}
int64_t nw_tab_media_state(int64_t h, int64_t tab) { (void)h; (void)tab; return NW_ERR_UNSUPPORTED; }
int64_t nw_tab_audible(int64_t h, int64_t tab) { (void)h; (void)tab; return NW_ERR_UNSUPPORTED; }
int64_t nw_tab_capture_state(int64_t h, int64_t tab) { (void)h; (void)tab; return NW_ERR_UNSUPPORTED; }
int64_t nw_set_inline_titlebar(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_start_drag(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_zoom(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_tab_zoom(int64_t h, int64_t tab, int64_t percent) {
    (void)h; (void)tab; (void)percent;
    return NW_ERR_UNSUPPORTED;
}
int64_t nw_emoji_picker(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_dock_menu_item(Str *id, Str *title) { (void)id; (void)title; return NW_ERR_INVALID; }
int64_t nw_dock_menu_separator(Str *id) { (void)id; return NW_ERR_INVALID; }
int64_t nw_dock_menu_remove(Str *id) { (void)id; return NW_ERR_INVALID; }
int64_t nw_last_active(void) { return 0; }
int64_t nw_focus_window(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_app_step(int64_t timeout_ms) { (void)timeout_ms; return NW_ERR_INVALID; }
int64_t nw_app_has_event(void) { return 0; }
Str *nw_app_take_event(void) { return rt_str_alloc(0); }
int64_t nw_titlebar_buttons_right(int64_t h) { (void)h; return NW_ERR_UNSUPPORTED; }
int64_t nw_titlebar_center_y(int64_t h) { (void)h; return NW_ERR_UNSUPPORTED; }
int64_t nw_go_back(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_go_forward(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_can_back(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_can_forward(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_reload(int64_t h) { (void)h; return NW_ERR_INVALID; }
int64_t nw_set_download_dir(int64_t h, Str *dir) { (void)h; (void)dir; return NW_ERR_INVALID; }
Str *nw_download_dir(int64_t h) { (void)h; return rt_str_alloc(0); }
Str *nw_downloads(int64_t h) {
    (void)h;
    Str *s = rt_str_alloc(2);
    memcpy(s->data, "[]", 2);
    return s;
}
int64_t nw_download_cancel(int64_t h, int64_t id) { (void)h; (void)id; return NW_ERR_UNSUPPORTED; }
int64_t nw_open_path(Str *path) { (void)path; return NW_ERR_UNSUPPORTED; }
int64_t nw_reveal_path(Str *path) { (void)path; return NW_ERR_UNSUPPORTED; }
int64_t nw_ext_load(int64_t h, Str *path) { (void)h; (void)path; return NW_ERR_UNSUPPORTED; }
int64_t nw_ext_unload(int64_t h, int64_t ext) { (void)h; (void)ext; return NW_ERR_UNSUPPORTED; }
Str *nw_ext_list(int64_t h) {
    (void)h;
    Str *s = rt_str_alloc(2);
    memcpy(s->data, "[]", 2);
    return s;
}
int64_t nw_ext_set_permission(int64_t h, int64_t ext, Str *perm, int64_t status) {
    (void)h; (void)ext; (void)perm; (void)status;
    return NW_ERR_UNSUPPORTED;
}
int64_t nw_ext_action_click(int64_t h, int64_t ext, int64_t x, int64_t y, int64_t wpx, int64_t hpx) {
    (void)h; (void)ext; (void)x; (void)y; (void)wpx; (void)hpx;
    return NW_ERR_UNSUPPORTED;
}
int64_t nw_ext_reply(int64_t h, int64_t req, Str *answer) {
    (void)h; (void)req; (void)answer;
    return NW_ERR_UNSUPPORTED;
}
int64_t nw_perm_reply(int64_t h, int64_t req, Str *answer) {
    (void)h; (void)req; (void)answer;
    return NW_ERR_UNSUPPORTED;
}
int64_t nw_set_geolocation(int64_t h, int64_t on) {
    (void)h; (void)on;
    return NW_ERR_UNSUPPORTED;
}
int64_t nw_ext_popup_close(int64_t h) { (void)h; return NW_ERR_UNSUPPORTED; }
int64_t nw_ext_tab_new(int64_t h, int64_t ext) { (void)h; (void)ext; return NW_ERR_UNSUPPORTED; }
Str *nw_ext_icon(int64_t h, int64_t ext, int64_t size) {
    (void)h; (void)ext; (void)size;
    return rt_str_alloc(0);
}
Str *nw_ext_last_error(void) { return rt_str_alloc(0); }
Str *nw_ext_installed(void) {
    Str *s = rt_str_alloc(2);
    memcpy(s->data, "[]", 2);
    return s;
}
Str *nw_pick_path(void) { return rt_str_alloc(0); }
int64_t nw_set_ui_html(int64_t h, Str *html) { (void)h; (void)html; return NW_ERR_INVALID; }
int64_t nw_set_ui_front(int64_t h, int64_t on) { (void)h; (void)on; return NW_ERR_INVALID; }
int64_t nw_ui_eval(int64_t h, Str *js) { (void)h; (void)js; return NW_ERR_INVALID; }
int64_t nw_set_content_insets(int64_t h, int64_t top, int64_t left, int64_t bottom, int64_t right) {
    (void)h; (void)top; (void)left; (void)bottom; (void)right;
    return NW_ERR_INVALID;
}
int64_t nw_step(int64_t h, int64_t timeout_ms) { (void)h; (void)timeout_ms; return 0; }
int64_t nw_has_message(int64_t h) { (void)h; return 0; }
Str *nw_take_message(int64_t h) { (void)h; return rt_str_alloc(0); }
int64_t nw_has_ui_message(int64_t h) { (void)h; return 0; }
Str *nw_take_ui_message(int64_t h) { (void)h; return rt_str_alloc(0); }
void nw_close(int64_t h) { (void)h; }
Str *nw_resources_path(void) { return rt_str_alloc(0); }

#endif
