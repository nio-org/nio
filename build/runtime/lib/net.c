// The `net` module (import 'net'): sockets over TCP, UDP and Unix domain
// (§6.15), linked only when a program imports it. Signatures must agree with
// netCallType/netNsCallType in the checker and genNetCall/genNetNsCall in
// codegen; the shapes they share are in runtime.h.
//
// Every descriptor is non-blocking. There is one thread, so a blocking call
// stops the program instead of the task that asked for it. An operation that
// cannot finish now becomes a pending future the scheduler completes.
//
// The readiness poller is the platform's own: kqueue on macOS and the BSDs,
// epoll on Linux, WSAPoll on Windows, behind poll_start/poll_set/poll_wait.
//
// A socket is a block that holds its own descriptor (NetSock, runtime.h), so a
// close is visible through every copy of the value at once. A bare descriptor
// number goes stale: the operating system gives it to the next socket opened,
// and an old copy then reads someone else's connection. Here it reads -1.
//
// The blocks are held in a weak table and closed by the collector's weak hook
// (rt_gc_weak_hook, runtime.h). A socket with an operation in flight stays
// alive: the scheduler's extern list roots it through `aux`.

#include "runtime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
typedef int socklen_arg_t;
// The MSVC CRT has no ssize_t. Winsock's recv and send answer an int.
#if !defined(_SSIZE_T_DEFINED)
typedef intptr_t ssize_t;
#define _SSIZE_T_DEFINED
#endif
#define NET_INVALID INVALID_SOCKET
#define net_close_fd closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
typedef int sock_t;
typedef socklen_t socklen_arg_t;
#define NET_INVALID (-1)
#define net_close_fd close
#endif

#if defined(__linux__)
#include <sys/epoll.h>
#elif !defined(_WIN32)
#include <sys/event.h>
#endif

// Readable operations first, then writable ones: op_writes reads the order.
enum {
    OP_ACCEPT,
    OP_READ,
    OP_READ_EXACT,
    OP_RECEIVE,
    OP_CONNECT,
    OP_WRITE,
    OP_SEND
};

static int op_writes(int op) { return op >= OP_CONNECT; }

// ---- errors ----

// Winsock has its own error numbers instead of errno, so it has its own map
// onto the NIO_ERR_* set.
static int64_t net_errno_code(void) {
#if defined(_WIN32)
    switch (WSAGetLastError()) {
    case WSAECONNREFUSED: return NIO_ERR_CONNECTION_REFUSED;
    case WSAECONNRESET:   return NIO_ERR_CONNECTION_RESET;
    case WSAECONNABORTED: return NIO_ERR_CONNECTION_ABORTED;
    case WSAENOTCONN:     return NIO_ERR_NOT_CONNECTED;
    case WSAEISCONN:      return NIO_ERR_ALREADY_CONNECTED;
    case WSAEADDRINUSE:   return NIO_ERR_ADDRESS_IN_USE;
    case WSAEADDRNOTAVAIL:return NIO_ERR_ADDRESS_NOT_AVAILABLE;
    case WSAENETUNREACH:  return NIO_ERR_NETWORK_UNREACHABLE;
    case WSAEHOSTUNREACH: return NIO_ERR_HOST_UNREACHABLE;
    case WSAETIMEDOUT:    return NIO_ERR_TIMED_OUT;
    case WSAEMSGSIZE:     return NIO_ERR_MESSAGE_TOO_LONG;
    case WSAEACCES:       return NIO_ERR_PERMISSION;
    case WSAEINVAL:       return NIO_ERR_INVALID;
    case WSAEMFILE:       return NIO_ERR_TOO_MANY_FILES;
    case WSAEINTR:        return NIO_ERR_INTERRUPTED;
    default:              return NIO_ERR_OTHER;
    }
#else
    return rt_err_from_errno(errno);
#endif
}

static int net_would_block(void) {
#if defined(_WIN32)
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
}

static int net_interrupted(void) {
#if defined(_WIN32)
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

// The message shape is `net.<what>: <reason>`. Winsock has no strerror, so
// Windows reports the code instead.
static void *net_error(const char *what, int64_t code) {
    char msg[512];
#if defined(_WIN32)
    snprintf(msg, sizeof msg, "net.%s failed (%lld)", what, (long long)code);
#else
    snprintf(msg, sizeof msg, "net.%s: %s", what, strerror(errno));
#endif
    return rt_error_new(msg, code);
}

static void *net_errno_error(const char *what) { return net_error(what, net_errno_code()); }

// A failure the operating system does not report, in the library's own words.
static void *net_say(const char *what, const char *why, int64_t code) {
    char msg[512];
    snprintf(msg, sizeof msg, "net.%s: %s", what, why);
    return rt_error_new(msg, code);
}

// ---- options (net.Options; see runtime.h) ----

// An optional field of net.Options, read out of its box. An absent record, an
// absent field and a null field all answer `absent`.
static int64_t opt_get(int64_t *opts, int at, int64_t absent) {
    if (!opts) return absent;
    TypeDesc *td = REC_TD(opts);
    void *box = (void *)(intptr_t)rt_rec_get(opts, at, td);
    if (!box) return absent;
    return rt_box_get(box, td->field_types[at]->elem);
}

// The timeout in milliseconds, or -1 for none. A Duration already counts
// milliseconds (§2.1).
static int64_t opt_timeout(int64_t *opts) {
    int64_t ms = opt_get(opts, 0, -1);
    return ms < 0 ? -1 : ms;
}

static int opt_backlog(int64_t *opts) {
    int64_t v = opt_get(opts, 1, 0);
    if (v <= 0) return 128;
    if (v > 65535) return 65535;
    return (int)v;
}

static int opt_nodelay(int64_t *opts) {
    return opt_get(opts, 2, 0) != 0;
}

// ---- descriptors ----

// A write to a socket the peer closed must raise and not kill the process,
// so platforms without MSG_NOSIGNAL ignore SIGPIPE here. A program cannot set a
// disposition of its own.
static int net_started = 0;
static void net_weak_sweep(void);
static int net_extern_wait(int64_t budget_ms);
static int poll_start(void);

static int net_start(void) {
    if (net_started) return 1;
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    if (!poll_start()) return 0;
    rt_gc_weak_hook(net_weak_sweep);
    net_started = 1;
    return 1;
}

static int set_nonblocking(sock_t fd) {
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(fd, FIONBIO, &on) == 0;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    return fl != -1 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) != -1;
#endif
}

static void set_nodelay(sock_t fd) {
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
}

// ---- the readiness poller ----

// The poller reports the socket block. A Windows SOCKET is a handle, so an
// array indexed by the descriptor cannot find the block.
typedef struct {
    NetSock *s;
    int what; // NIO_NET_R | NIO_NET_W
} NetEvent;

#define NET_EVENTS 64

// kqueue and epoll keep the read side registered, edge-triggered, from the
// first read that parks until the socket closes. A registration per parked read
// costs two system calls per request, a quarter of a keep-alive server's time.
// An edge is reported once, so this is correct only because every operation
// here keeps two rules:
//
//   1. An operation tries the socket before it parks (finish_begin), so data
//      that arrived while nothing waited is found without an event.
//   2. A woken operation that finds nothing (EAGAIN) stays parked, and an edge
//      for a socket with no waiter is dropped (net_extern_wait).
//
// The write side follows `want`, because a write waits only on a full send
// buffer, which is rare. Closing goes through poll_forget. An epoll
// registration belongs to the open file and not to the descriptor number, so a
// registration that a child process inherited would outlive the close.

#if defined(__linux__)

static int ep = -1;
static int poll_start(void) {
    if (ep < 0) ep = epoll_create1(EPOLL_CLOEXEC);
    return ep >= 0;
}

// Brings the kernel's registration in line with s->want.
static void poll_set(NetSock *s) {
    int next = (int)((s->polled & NIO_NET_R) | (s->want & (NIO_NET_R | NIO_NET_W)));
    if (next == s->polled) return;
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.data.ptr = s;
    ev.events = EPOLLET;
    if (next & NIO_NET_R) ev.events |= EPOLLIN;
    if (next & NIO_NET_W) ev.events |= EPOLLOUT;
    // An ADD or a MOD reports a condition that already holds, so an edge that
    // came before the registration is not lost.
    if (!next) {
        epoll_ctl(ep, EPOLL_CTL_DEL, (sock_t)s->fd, &ev);
    } else {
        epoll_ctl(ep, s->polled ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, (sock_t)s->fd, &ev);
    }
    s->polled = next;
}

static void poll_forget(NetSock *s) {
    if (!s->polled) return;
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    epoll_ctl(ep, EPOLL_CTL_DEL, (sock_t)s->fd, &ev);
    s->polled = 0;
}

static int poll_wait(int64_t budget_ms, NetEvent *out, int max) {
    struct epoll_event evs[NET_EVENTS];
    if (max > NET_EVENTS) max = NET_EVENTS;
    int n = epoll_wait(ep, evs, max, budget_ms < 0 ? -1 : (int)budget_ms);
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) {
        out[i].s = evs[i].data.ptr;
        out[i].what = 0;
        // An error or a hangup wakes both sides. The operation turns it into
        // the right message, because only it knows what it attempted.
        if (evs[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) out[i].what |= NIO_NET_R;
        if (evs[i].events & (EPOLLOUT | EPOLLHUP | EPOLLERR)) out[i].what |= NIO_NET_W;
    }
    return n;
}

#elif !defined(_WIN32)

static int kq = -1;
static int poll_start(void) {
    if (kq < 0) kq = kqueue();
    return kq >= 0;
}

// Brings the kernel's registration in line with s->want. EV_CLEAR makes the
// read filter edge-triggered, and adding it reports data already there.
static void poll_set(NetSock *s) {
    struct kevent ch[2];
    int n = 0;
    if ((s->want & NIO_NET_R) && !(s->polled & NIO_NET_R)) {
        EV_SET(&ch[n++], (sock_t)s->fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, s);
        s->polled |= NIO_NET_R;
    }
    if ((s->want & NIO_NET_W) != (s->polled & NIO_NET_W)) {
        EV_SET(&ch[n++], (sock_t)s->fd, EVFILT_WRITE,
               (s->want & NIO_NET_W) ? EV_ADD : EV_DELETE, 0, 0, s);
        s->polled ^= NIO_NET_W;
    }
    if (n) kevent(kq, ch, n, NULL, 0, NULL);
}

static void poll_forget(NetSock *s) {
    struct kevent ch[2];
    int n = 0;
    if (s->polled & NIO_NET_R) EV_SET(&ch[n++], (sock_t)s->fd, EVFILT_READ, EV_DELETE, 0, 0, s);
    if (s->polled & NIO_NET_W) EV_SET(&ch[n++], (sock_t)s->fd, EVFILT_WRITE, EV_DELETE, 0, 0, s);
    if (n) kevent(kq, ch, n, NULL, 0, NULL);
    s->polled = 0;
}

static int poll_wait(int64_t budget_ms, NetEvent *out, int max) {
    struct kevent evs[NET_EVENTS];
    struct timespec ts;
    struct timespec *tp = NULL;
    if (max > NET_EVENTS) max = NET_EVENTS;
    if (budget_ms >= 0) {
        ts.tv_sec = budget_ms / 1000;
        ts.tv_nsec = (budget_ms % 1000) * 1000000;
        tp = &ts;
    }
    int n = kevent(kq, NULL, 0, evs, max, tp);
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) {
        out[i].s = evs[i].udata;
        // EV_EOF still runs the read: what arrived before the end is the
        // answer, and no bytes is how a stream end is reported (§6.15).
        out[i].what = evs[i].filter == EVFILT_WRITE ? NIO_NET_W : NIO_NET_R;
    }
    return n;
}

#else

// WSAPoll takes the whole interest set on every call, so the registration is
// kept here and rebuilt into a WSAPOLLFD array each time: O(connections) per
// turn, which kqueue and epoll avoid.
static struct {
    NetSock *s;
    int want;
} *reg = NULL;
static int nreg = 0, reg_cap = 0;

static int poll_start(void) { return 1; }

// WSAPoll is level-triggered, so the table always matches s->want.
static void poll_set(NetSock *s) {
    int want = (int)s->want;
    for (int i = 0; i < nreg; i++) {
        if (reg[i].s != s) continue;
        if (want) {
            reg[i].want = want;
        } else {
            reg[i] = reg[--nreg];
        }
        return;
    }
    if (!want) return;
    if (nreg == reg_cap) {
        reg_cap = reg_cap ? reg_cap * 2 : 16;
        reg = realloc(reg, (size_t)reg_cap * sizeof *reg);
        if (!reg) rt_panic("out of memory");
    }
    reg[nreg].s = s;
    reg[nreg].want = want;
    nreg++;
}

static void poll_forget(NetSock *s) {
    s->want = 0;
    poll_set(s);
}

static int poll_wait(int64_t budget_ms, NetEvent *out, int max) {
    if (nreg == 0) return 0;
    WSAPOLLFD *fds = malloc((size_t)nreg * sizeof *fds);
    if (!fds) rt_panic("out of memory");
    for (int i = 0; i < nreg; i++) {
        fds[i].fd = (sock_t)reg[i].s->fd;
        fds[i].events = 0;
        if (reg[i].want & NIO_NET_R) fds[i].events |= POLLRDNORM;
        if (reg[i].want & NIO_NET_W) fds[i].events |= POLLWRNORM;
        fds[i].revents = 0;
    }
    int n = WSAPoll(fds, (ULONG)nreg, budget_ms < 0 ? -1 : (int)budget_ms);
    int got = 0;
    for (int i = 0; i < nreg && got < max && n > 0; i++) {
        if (!fds[i].revents) continue;
        out[got].s = reg[i].s;
        out[got].what = 0;
        if (fds[i].revents & (POLLRDNORM | POLLHUP | POLLERR)) out[got].what |= NIO_NET_R;
        if (fds[i].revents & (POLLWRNORM | POLLHUP | POLLERR)) out[got].what |= NIO_NET_W;
        got++;
    }
    free(fds);
    return got;
}

#endif

// ---- the weak table of handed-out sockets ----

typedef struct {
    NetSock *block; // held weakly: this table is not a root
    char *path;     // a Unix listener's path, to remove at close; else NULL
} Handle;

static Handle *handles = NULL;
static int64_t nhandles = 0, handles_cap = 0;

static void handle_add(NetSock *s, char *path) {
    if (nhandles == handles_cap) {
        handles_cap = handles_cap ? handles_cap * 2 : 16;
        handles = realloc(handles, (size_t)handles_cap * sizeof *handles);
        if (!handles) rt_panic("out of memory");
    }
    handles[nhandles].block = s;
    handles[nhandles].path = path;
    nhandles++;
}

// Closes the descriptor and writes -1, which every copy of the value sees.
// Every close goes through here.
static void sock_shut(NetSock *s, char *path) {
    if (s->fd < 0) return;
    poll_forget(s);
    s->want = 0;
    net_close_fd((sock_t)s->fd);
    s->fd = -1;
#if !defined(_WIN32)
    // A Unix listener owns the name it bound. A leftover name stops the next
    // run of the server (§6.15).
    if (path) unlink(path);
#else
    (void)path;
#endif
}

// rt_gc_weak_hook (runtime.h): a block the mark phase did not reach is one the
// program has dropped, so its descriptor closes here. This runs inside a
// collection and must not allocate.
static void net_weak_sweep(void) {
    for (int64_t i = 0; i < nhandles;) {
        if (rt_gc_marked(handles[i].block)) {
            i++;
            continue;
        }
        sock_shut(handles[i].block, handles[i].path);
        free(handles[i].path);
        handles[i] = handles[--nhandles];
    }
}

static void handle_forget(NetSock *s) {
    for (int64_t i = 0; i < nhandles; i++) {
        if (handles[i].block != s) continue;
        free(handles[i].path);
        handles[i] = handles[--nhandles];
        return;
    }
}

// `path` is malloc'd memory this function takes over.
static NetSock *sock_new(sock_t fd, int64_t proto, int64_t role, char *path) {
    NetSock *s = rt_alloc(sizeof(NetSock));
    s->fd = (int64_t)fd;
    s->proto = proto;
    s->role = role;
    s->want = 0;
    s->rwait = 0;
    s->wwait = 0;
    s->polled = 0;
    // Must be last: the block is not rooted yet, and an allocation after the
    // table holds it lets the weak sweep close a live descriptor.
    handle_add(s, path);
    return s;
}

// ---- pending operations ----
//
// One per operation that could not finish at once. The scheduler's extern list
// roots the future, and that entry's `aux` roots the socket. The buffer is C
// memory, which keeps a partly written payload out of the collector's way.
typedef struct Wait {
    struct Wait *prev, *next;
    Future *fu;
    NetSock *s;
    int op;
    int linked;  // registered with the poller and on the list below
    int dead;    // unlinked, and waiting to be freed at a safe moment
    int nodelay; // accept and connect: set TCP_NODELAY on what comes out
    char *buf;
    void *gcbuf; // OP_READ_EXACT: the byte[] being filled. GC memory kept alive
                 // by the future's aux pair, never freed here
    int64_t len; // bytes to write or send, or bytes to read at most
    int64_t off; // bytes written or read so far
    int64_t deadline;
    struct sockaddr_storage peer;
    socklen_arg_t peerlen;
} Wait;

static Wait *waits = NULL;     // every pending operation, for expiry
static int64_t nwaits = 0;
static Wait *graveyard = NULL; // unlinked, not yet freed (see net_extern_wait)
static int64_t earliest = -1;  // the soonest deadline among waits, or -1

// One slot per side: two operations on one side have no meaning (wait_start).
static Wait **sock_slot(NetSock *s, int op) {
    return (Wait **)(op_writes(op) ? &s->wwait : &s->rwait);
}

static void wait_link(Wait *w) {
    *sock_slot(w->s, w->op) = w;
    int bit = op_writes(w->op) ? NIO_NET_W : NIO_NET_R;
    if (!(w->s->want & bit)) {
        w->s->want |= bit;
        poll_set(w->s);
    }
    w->prev = NULL;
    w->next = waits;
    if (waits) waits->prev = w;
    waits = w;
    w->linked = 1;
    nwaits++;
    if (w->deadline >= 0 && (earliest < 0 || w->deadline < earliest)) earliest = w->deadline;
}

// Takes it back out and leaves it for net_extern_wait to free. A batch of ready
// operations holds pointers to them, and completing one runs program code that
// may unlink another, so `dead` marks it instead of freeing it here.
//
// An operation that never linked is freed at once: nothing else points at it,
// and a socket whose reads all finish inline would fill the graveyard.
static void wait_unlink(Wait *w) {
    if (w->dead) return;
    if (!w->linked) {
        w->dead = 1;
        free(w->buf);
        free(w);
        return;
    }
    if (w->linked) {
        Wait **slot = sock_slot(w->s, w->op);
        if (*slot == w) *slot = NULL;
        if (w->s->fd >= 0) {
            int bit = op_writes(w->op) ? NIO_NET_W : NIO_NET_R;
            if (w->s->want & bit) {
                w->s->want &= ~(int64_t)bit;
                poll_set(w->s);
            }
        }
        if (w->prev) {
            w->prev->next = w->next;
        } else {
            waits = w->next;
        }
        if (w->next) w->next->prev = w->prev;
        w->linked = 0;
        nwaits--;
        // `earliest` may now be too early. Expiry recomputes it.
    }
    w->dead = 1;
    w->next = graveyard;
    graveyard = w;
}

static void graveyard_clear(void) {
    while (graveyard) {
        Wait *w = graveyard;
        graveyard = w->next;
        free(w->buf);
        free(w);
    }
}

// The future is read out first because unlinking may free the operation.
static void wait_finish(Wait *w, int64_t result, TypeDesc *td, void *error) {
    Future *fu = w->fu;
    wait_unlink(w);
    rt_async_extern_done(fu, result, td, error);
}

// ---- addresses ----

// Splits "host:port", or "[host]:port" for an IPv6 literal, whose colons are
// otherwise the separator. An empty host means every interface.
static int split_hostport(Str *addr, char *host, size_t hostn, char *port, size_t portn) {
    if (addr->len == 0 || (size_t)addr->len > hostn + portn) return 0;
    const char *p = addr->data;
    int64_t n = addr->len;
    int64_t sep = -1;
    int64_t hstart = 0, hend = 0;
    if (p[0] == '[') {
        for (int64_t i = 1; i < n; i++) {
            if (p[i] != ']') continue;
            hstart = 1;
            hend = i;
            sep = i + 1;
            break;
        }
        if (sep < 0 || sep >= n || p[sep] != ':') return 0;
    } else {
        // The last colon, so an unbracketed IPv6 literal fails in getaddrinfo
        // instead of splitting at the wrong place.
        for (int64_t i = n - 1; i >= 0; i--) {
            if (p[i] == ':') {
                sep = i;
                break;
            }
        }
        if (sep < 0) return 0;
        hend = sep;
    }
    size_t hlen = (size_t)(hend - hstart);
    size_t plen = (size_t)(n - sep - 1);
    if (hlen >= hostn || plen >= portn || plen == 0) return 0;
    memcpy(host, p + hstart, hlen);
    host[hlen] = 0;
    memcpy(port, p + sep + 1, plen);
    port[plen] = 0;
    if (memchr(host, 0, hlen) || memchr(port, 0, plen)) return 0;
    return 1;
}

// Resolves "host:port" to one address. Numeric services only: /etc/services
// would make one string mean different things on different machines.
// getaddrinfo blocks, and with one thread there is nowhere else to put it. A
// server never resolves; a client resolves once per connection (§6.15).
static int resolve(Str *addr, int passive, int dgram, struct sockaddr_storage *out,
                   socklen_arg_t *outlen, int64_t *code) {
    char host[256], port[32];
    if (!split_hostport(addr, host, sizeof host, port, sizeof port)) {
        *code = NIO_ERR_INVALID;
        return 0;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = dgram ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);
    int rc = getaddrinfo(host[0] ? host : NULL, port, &hints, &res);
    if (rc != 0 || !res) {
        if (res) freeaddrinfo(res);
        *code = rc == EAI_NONAME ? NIO_ERR_NOT_FOUND : NIO_ERR_INVALID;
        return 0;
    }
    memcpy(out, res->ai_addr, res->ai_addrlen);
    *outlen = (socklen_arg_t)res->ai_addrlen;
    freeaddrinfo(res);
    return 1;
}

#if !defined(_WIN32)
// A Unix address is the path itself. It must fit sun_path, whose size differs
// between operating systems.
static int unix_addr(Str *path, struct sockaddr_un *out, socklen_arg_t *outlen) {
    if (path->len == 0 || (size_t)path->len >= sizeof out->sun_path) return 0;
    if (memchr(path->data, 0, (size_t)path->len)) return 0;
    memset(out, 0, sizeof *out);
    out->sun_family = AF_UNIX;
    memcpy(out->sun_path, path->data, (size_t)path->len);
    *outlen = (socklen_arg_t)(sizeof *out);
    return 1;
}
#endif

// The text form every call here accepts back. IPv6 is bracketed, so its colons
// cannot read as the separator.
static Str *addr_text(const struct sockaddr *sa, socklen_arg_t len) {
#if !defined(_WIN32)
    if (sa->sa_family == AF_UNIX) {
        const struct sockaddr_un *un = (const struct sockaddr_un *)sa;
        size_t n = strnlen(un->sun_path, sizeof un->sun_path);
        Str *s = rt_str_alloc((int64_t)n);
        memcpy(s->data, un->sun_path, n);
        return s;
    }
#endif
    char host[NI_MAXHOST], port[NI_MAXSERV];
    if (getnameinfo(sa, len, host, sizeof host, port, sizeof port,
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return rt_str_from_c("");
    }
    char out[NI_MAXHOST + NI_MAXSERV + 4];
    if (sa->sa_family == AF_INET6) {
        snprintf(out, sizeof out, "[%s]:%s", host, port);
    } else {
        snprintf(out, sizeof out, "%s:%s", host, port);
    }
    return rt_str_from_c(out);
}

// ---- results ----

static TypeDesc td_byte_array = {TD_ARRAY, &rt_td_uint8, 0, 0, 0, 0};

// The aux of a parked readExactly. It must keep the byte[] alive as well as the
// socket, and a future traces one (aux, aux_td) pair (runtime.h), so the two
// ride in a heap pair traced as TD_SLOTS.
static TypeDesc *read_pair_types[2] = {&rt_td_socket, &td_byte_array};
static TypeDesc td_read_pair = {TD_SLOTS, NULL, 2, NULL, read_pair_types, 0};

// The offsets must match what codegen computes for types.DatagramT (§5.7).
static const char *datagram_field_names[2] = {"address", "data"};
static TypeDesc *datagram_field_types[2] = {&rt_td_string, &td_byte_array};
static const int64_t datagram_field_offsets[2] = {8, 16};
static TypeDesc td_datagram = {TD_RECORD, 0,      2, datagram_field_names,
                               datagram_field_types, 0, datagram_field_offsets, 24};

// A byte[]'s storage is bytes (§5.7), so this is a memcpy.
static Arr *bytes_to_arr(const char *buf, int64_t n) {
    Arr *a = rt_arr_new(n, 1);
    memcpy(rt_arr_bytes(a), buf, (size_t)n);
    return a;
}

// The array is rooted across the address allocation, and both across the
// record's.
static void *datagram_new(const char *buf, int64_t n, const struct sockaddr *sa,
                          socklen_arg_t salen) {
    TypeDesc *tds[2] = {&td_byte_array, &rt_td_string};
    int64_t slots[2] = {0, 0};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;

    slots[0] = (int64_t)(intptr_t)bytes_to_arr(buf, n);
    slots[1] = (int64_t)(intptr_t)addr_text(sa, salen);
    void *rec = rt_rec_new(&td_datagram);
    rt_rec_set(rec, 0, slots[1], &td_datagram);
    rt_rec_set(rec, 1, slots[0], &td_datagram);

    rt_gc_top = f.prev;
    return rec;
}

// ---- starting an operation ----

// s is the value that must outlive the wait (runtime.h).
static Future *op_future(NetSock *s) {
    return rt_async_extern_new(net_extern_wait, s, &rt_td_socket);
}

static Future *reject_closed(NetSock *s, const char *what) {
    if (s && s->fd >= 0) return NULL;
    Future *fu = op_future(s);
    rt_async_extern_done(fu, 0, NULL,
        net_say(what, "the socket is closed", NIO_ERR_NOT_CONNECTED));
    return fu;
}

// Two reads outstanding on one stream have no meaning: the bytes go to
// whichever the scheduler wakes first. The second one is refused.
static int wait_start(Wait *w, const char *what) {
    Wait **slot = sock_slot(w->s, w->op);
    if (*slot) {
        rt_async_extern_done(w->fu, 0, NULL,
            net_say(what, op_writes(w->op)
                ? "the socket already has a write outstanding"
                : "the socket already has a read outstanding",
                NIO_ERR_INVALID));
        free(w->buf);
        free(w);
        return 0;
    }
    wait_link(w);
    return 1;
}

static Wait *wait_new(NetSock *s, Future *fu, int op, int64_t timeout) {
    Wait *w = calloc(1, sizeof *w);
    if (!w) rt_panic("out of memory");
    w->fu = fu;
    w->s = s;
    w->op = op;
    w->deadline = timeout < 0 ? -1 : rt_mono_ms() + timeout;
    return w;
}

// ---- carrying an operation out ----
//
// Each attempts the operation once. It answers 1 when the future is completed
// and 0 when it must wait for readiness again. The entry point and the poller
// call the same code, so a socket that is ready already never parks.

static int try_accept(Wait *w) {
    struct sockaddr_storage sa;
    socklen_arg_t salen = sizeof sa;
    for (;;) {
        sock_t fd = accept((sock_t)w->s->fd, (struct sockaddr *)&sa, &salen);
        if (fd != NET_INVALID) {
            if (!set_nonblocking(fd)) {
                void *e = net_errno_error("accept");
                net_close_fd(fd);
                wait_finish(w, 0, NULL, e);
                return 1;
            }
            if (w->nodelay && w->s->proto == NIO_NET_TCP) set_nodelay(fd);
            NetSock *c = sock_new(fd, w->s->proto, NIO_NET_CONN, NULL);
            wait_finish(w, (int64_t)(intptr_t)c, &rt_td_socket, NULL);
            return 1;
        }
        if (net_interrupted()) continue;
        if (net_would_block()) return 0;
        wait_finish(w, 0, NULL, net_errno_error("accept"));
        return 1;
    }
}

static int try_connect(Wait *w) {
    // Write readiness tells only that the connect ended. SO_ERROR tells
    // whether it succeeded.
    int err = 0;
    socklen_arg_t n = sizeof err;
    if (getsockopt((sock_t)w->s->fd, SOL_SOCKET, SO_ERROR, (char *)&err, &n) != 0) {
        wait_finish(w, 0, NULL, net_errno_error("connect"));
        return 1;
    }
    if (err != 0) {
#if defined(_WIN32)
        WSASetLastError(err);
#else
        errno = err;
#endif
        wait_finish(w, 0, NULL, net_errno_error("connect"));
        return 1;
    }
    if (w->nodelay && w->s->proto == NIO_NET_TCP) set_nodelay((sock_t)w->s->fd);
    wait_finish(w, (int64_t)(intptr_t)w->s, &rt_td_socket, NULL);
    return 1;
}

static int try_read(Wait *w) {
    for (;;) {
        ssize_t n = recv((sock_t)w->s->fd, w->buf, (size_t)w->len, 0);
        if (n >= 0) {
            // A short answer is not an error, and an empty one is the end of
            // the stream (§6.15).
            Arr *a = bytes_to_arr(w->buf, (int64_t)n);
            wait_finish(w, (int64_t)(intptr_t)a, &td_byte_array, NULL);
            return 1;
        }
        if (net_interrupted()) continue;
        if (net_would_block()) return 0;
        wait_finish(w, 0, NULL, net_errno_error("read"));
        return 1;
    }
}

// Accumulates into the result array across partial arrivals, so the scheduler
// parks once for the whole body. The array is GC memory the future's aux pair
// keeps alive, so w->buf stays NULL.
static int try_read_exact(Wait *w) {
    Arr *a = w->gcbuf;
    char *base = (char *)rt_arr_bytes(a);
    for (;;) {
        ssize_t n = recv((sock_t)w->s->fd, base + w->off,
                         (size_t)(w->len - w->off), 0);
        if (n > 0) {
            w->off += (int64_t)n;
            if (w->off == w->len) {
                wait_finish(w, (int64_t)(intptr_t)a, &td_byte_array, NULL);
                return 1;
            }
            continue;
        }
        if (n == 0) {
            wait_finish(w, 0, NULL, net_say("readExactly",
                "the connection closed before the requested bytes arrived",
                NIO_ERR_END_OF_FILE));
            return 1;
        }
        if (net_interrupted()) continue;
        if (net_would_block()) return 0;
        wait_finish(w, 0, NULL, net_errno_error("readExactly"));
        return 1;
    }
}

static int try_write(Wait *w) {
    while (w->off < w->len) {
        ssize_t n = send((sock_t)w->s->fd, w->buf + w->off, (size_t)(w->len - w->off),
#if defined(MSG_NOSIGNAL)
                         MSG_NOSIGNAL
#else
                         0
#endif
        );
        if (n > 0) {
            w->off += n;
            continue;
        }
        if (n < 0 && net_interrupted()) continue;
        if (n < 0 && net_would_block()) return 0;
        wait_finish(w, 0, NULL, net_errno_error("write"));
        return 1;
    }
    wait_finish(w, 0, NULL, NULL);
    return 1;
}

static int try_receive(Wait *w) {
    for (;;) {
        struct sockaddr_storage sa;
        socklen_arg_t salen = sizeof sa;
        ssize_t n = recvfrom((sock_t)w->s->fd, w->buf, (size_t)w->len, 0,
                             (struct sockaddr *)&sa, &salen);
        if (n >= 0) {
            void *d = datagram_new(w->buf, (int64_t)n, (struct sockaddr *)&sa, salen);
            wait_finish(w, (int64_t)(intptr_t)d, &td_datagram, NULL);
            return 1;
        }
        if (net_interrupted()) continue;
        if (net_would_block()) return 0;
        wait_finish(w, 0, NULL, net_errno_error("receive"));
        return 1;
    }
}

static int try_send(Wait *w) {
    for (;;) {
        ssize_t n = sendto((sock_t)w->s->fd, w->buf, (size_t)w->len, 0,
                           (struct sockaddr *)&w->peer, w->peerlen);
        if (n >= 0) {
            // A datagram is sent whole or not at all, so a short count is a
            // failure.
            if (n < w->len) {
                wait_finish(w, 0, NULL,
                    net_say("send", "the datagram was truncated", NIO_ERR_MESSAGE_TOO_LONG));
                return 1;
            }
            wait_finish(w, 0, NULL, NULL);
            return 1;
        }
        if (net_interrupted()) continue;
        if (net_would_block()) return 0;
        wait_finish(w, 0, NULL, net_errno_error("send"));
        return 1;
    }
}

static int try_op(Wait *w) {
    switch (w->op) {
    case OP_ACCEPT:     return try_accept(w);
    case OP_CONNECT:    return try_connect(w);
    case OP_READ:       return try_read(w);
    case OP_READ_EXACT: return try_read_exact(w);
    case OP_WRITE:      return try_write(w);
    case OP_RECEIVE:    return try_receive(w);
    default:            return try_send(w);
    }
}

static const char *op_name(int op) {
    switch (op) {
    case OP_ACCEPT:     return "accept";
    case OP_CONNECT:    return "connect";
    case OP_READ:       return "read";
    case OP_READ_EXACT: return "readExactly";
    case OP_WRITE:      return "write";
    case OP_RECEIVE:    return "receive";
    default:            return "send";
    }
}

// ---- the scheduler's waiter (runtime.h) ----

// Fails whatever has run out of its timeout. The scan restarts after each one:
// completing an operation runs program code that may unlink any of the others,
// so a saved `next` is not safe to use. Expiry is rare, so the quadratic worst
// case is acceptable.
static int net_expire(int64_t now) {
    if (earliest < 0 || now < earliest) return 0;
    int done = 0;
    for (int again = 1; again;) {
        again = 0;
        for (Wait *w = waits; w; w = w->next) {
            if (w->deadline < 0 || w->deadline > now) continue;
            wait_finish(w, 0, NULL, net_say(op_name(w->op), "timed out", NIO_ERR_TIMED_OUT));
            done++;
            again = 1;
            break;
        }
    }
    // Recomputed, which also forgets the deadline of an operation that finished
    // early. Nothing completes during this walk, so it may follow the list.
    earliest = -1;
    for (Wait *w = waits; w; w = w->next) {
        if (w->deadline >= 0 && (earliest < 0 || w->deadline < earliest)) earliest = w->deadline;
    }
    return done;
}

// Waits up to budget_ms for a pending operation to become ready and runs
// whichever are. The budget is shortened to the soonest deadline, so a timeout
// is honoured when the descriptor never becomes ready.
//
// The ready operations are collected before any of them runs: running one can
// run program code that closes a socket and unlinks another. Unlinking only
// marks it dead, so the loop skips it instead of reading freed memory. Only the
// outermost call empties the graveyard, because an inner one would free
// operations the outer batch still points at.
static int net_depth = 0;

static int net_extern_wait(int64_t budget_ms) {
    if (net_depth == 0) graveyard_clear();
    if (nwaits == 0) return 0;
    net_depth++;

    int64_t now = rt_mono_ms();
    int64_t wait_ms = budget_ms;
    if (earliest >= 0) {
        int64_t left = earliest - now;
        if (left < 0) left = 0;
        if (wait_ms < 0 || left < wait_ms) wait_ms = left;
    }

    NetEvent evs[NET_EVENTS];
    int n = poll_wait(wait_ms, evs, NET_EVENTS);

    Wait *ready[NET_EVENTS * 2];
    int nready = 0;
    for (int i = 0; i < n; i++) {
        NetSock *s = evs[i].s;
        if (!s) continue;
        if ((evs[i].what & NIO_NET_R) && s->rwait) ready[nready++] = (Wait *)(intptr_t)s->rwait;
        if ((evs[i].what & NIO_NET_W) && s->wwait) ready[nready++] = (Wait *)(intptr_t)s->wwait;
    }

    int done = 0;
    for (int i = 0; i < nready; i++) {
        if (ready[i]->dead) continue;
        if (try_op(ready[i])) done++;
    }
    done += net_expire(rt_mono_ms());
    net_depth--;
    return done > 0;
}

// ---- entry points ----

void *rt_net_listen(int64_t proto, Str *addr, int64_t *opts, void **err) {
    if (!net_start()) {
        *err = net_errno_error("listen");
        return NULL;
    }
    struct sockaddr_storage ss;
    socklen_arg_t salen = 0;
    char *path = NULL;
    int64_t code = 0;
    if (proto == NIO_NET_UNIX) {
#if defined(_WIN32)
        *err = net_say("listen", "Unix domain sockets are not available here", NIO_ERR_INVALID);
        return NULL;
#else
        if (!unix_addr(addr, (struct sockaddr_un *)&ss, &salen)) {
            *err = net_say("listen", "not a usable socket path", NIO_ERR_INVALID);
            return NULL;
        }
        path = malloc((size_t)addr->len + 1);
        if (!path) rt_panic("out of memory");
        memcpy(path, addr->data, (size_t)addr->len);
        path[addr->len] = 0;
        // The path is not removed first. It may belong to a running server,
        // and taking it would leave that one listening on a name nothing can
        // reach. The bind fails with ADDRESS_IN_USE and the program decides.
#endif
    } else if (!resolve(addr, 1, 0, &ss, &salen, &code)) {
        *err = net_say("listen", "not a usable address", code);
        return NULL;
    }

    sock_t fd = socket(ss.ss_family, SOCK_STREAM, 0);
    if (fd == NET_INVALID) {
        free(path);
        *err = net_errno_error("listen");
        return NULL;
    }
    // SO_REUSEADDR is always set. Without it a listener cannot restart while
    // its last connections are in TIME_WAIT. On these platforms it does not let
    // one program take a port that another program listens on.
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
    if (bind(fd, (struct sockaddr *)&ss, salen) != 0 || listen(fd, opt_backlog(opts)) != 0
        || !set_nonblocking(fd)) {
        void *e = net_errno_error("listen");
        net_close_fd(fd);
        free(path);
        *err = e;
        return NULL;
    }
    // noDelay belongs to accept and connect, which produce a connection. A
    // listener carries no data.
    return sock_new(fd, proto, NIO_NET_LISTENER, path);
}

void *rt_net_udp_bind(Str *addr, void **err) {
    if (!net_start()) {
        *err = net_errno_error("bind");
        return NULL;
    }
    struct sockaddr_storage ss;
    socklen_arg_t salen = 0;
    int64_t code = 0;
    if (!resolve(addr, 1, 1, &ss, &salen, &code)) {
        *err = net_say("bind", "not a usable address", code);
        return NULL;
    }
    sock_t fd = socket(ss.ss_family, SOCK_DGRAM, 0);
    if (fd == NET_INVALID) {
        *err = net_errno_error("bind");
        return NULL;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
    if (bind(fd, (struct sockaddr *)&ss, salen) != 0 || !set_nonblocking(fd)) {
        void *e = net_errno_error("bind");
        net_close_fd(fd);
        *err = e;
        return NULL;
    }
    return sock_new(fd, NIO_NET_UDP, NIO_NET_CONN, NULL);
}

Future *rt_net_connect(int64_t proto, Str *addr, int64_t *opts) {
    // Read the options before anything allocates (see begin). After this they
    // are numbers, and a collection cannot free them.
    int64_t timeout = opt_timeout(opts);
    int nodelay = opt_nodelay(opts) && proto == NIO_NET_TCP;
    if (!net_start()) {
        Future *fu = rt_async_extern_new(net_extern_wait, NULL, NULL);
        rt_async_extern_done(fu, 0, NULL, net_errno_error("connect"));
        return fu;
    }
    struct sockaddr_storage ss;
    socklen_arg_t salen = 0;
    int64_t code = 0;
    if (proto == NIO_NET_UNIX) {
#if defined(_WIN32)
        Future *fu = rt_async_extern_new(net_extern_wait, NULL, NULL);
        rt_async_extern_done(fu, 0, NULL,
            net_say("connect", "Unix domain sockets are not available here", NIO_ERR_INVALID));
        return fu;
#else
        if (!unix_addr(addr, (struct sockaddr_un *)&ss, &salen)) {
            Future *fu = rt_async_extern_new(net_extern_wait, NULL, NULL);
            rt_async_extern_done(fu, 0, NULL,
                net_say("connect", "not a usable socket path", NIO_ERR_INVALID));
            return fu;
        }
#endif
    } else if (!resolve(addr, 0, 0, &ss, &salen, &code)) {
        Future *fu = rt_async_extern_new(net_extern_wait, NULL, NULL);
        rt_async_extern_done(fu, 0, NULL, net_say("connect", "not a usable address", code));
        return fu;
    }

    sock_t fd = socket(ss.ss_family, SOCK_STREAM, 0);
    if (fd == NET_INVALID) {
        Future *fu = rt_async_extern_new(net_extern_wait, NULL, NULL);
        rt_async_extern_done(fu, 0, NULL, net_errno_error("connect"));
        return fu;
    }
    if (!set_nonblocking(fd)) {
        void *e = net_errno_error("connect");
        net_close_fd(fd);
        Future *fu = rt_async_extern_new(net_extern_wait, NULL, NULL);
        rt_async_extern_done(fu, 0, NULL, e);
        return fu;
    }

    // The socket is rooted while the future allocates.
    NetSock *s = sock_new(fd, proto, NIO_NET_CONN, NULL);
    TypeDesc *tds[1] = {&rt_td_socket};
    int64_t slots[1] = {(int64_t)(intptr_t)s};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;
    Future *fu = op_future(s);
    rt_gc_top = f.prev;
    s = (NetSock *)(intptr_t)slots[0];

    int rc = connect((sock_t)s->fd, (struct sockaddr *)&ss, salen);
    if (rc == 0) {
        if (nodelay) set_nodelay((sock_t)s->fd);
        rt_async_extern_done(fu, (int64_t)(intptr_t)s, &rt_td_socket, NULL);
        return fu;
    }
    if (!net_would_block()) {
        void *e = net_errno_error("connect");
        sock_shut(s, NULL);
        handle_forget(s);
        rt_async_extern_done(fu, 0, NULL, e);
        return fu;
    }
    Wait *w = wait_new(s, fu, OP_CONNECT, timeout);
    w->nodelay = nodelay;
    wait_start(w, "connect");
    return fu;
}

// A Socket is one type to the checker whichever call made it, so the operations
// that suit only streams or only datagrams tell the difference here.
static Future *reject_wrong_kind(NetSock *s, int op) {
    int wants_dgram = op == OP_RECEIVE || op == OP_SEND;
    int is_dgram = s->proto == NIO_NET_UDP;
    if (wants_dgram == is_dgram) return NULL;
    Future *fu = op_future(s);
    rt_async_extern_done(fu, 0, NULL,
        net_say(op_name(op), wants_dgram
            ? "this is a stream socket; use net.read and net.write"
            : "this is a datagram socket; use net.receive and net.send",
            NIO_ERR_INVALID));
    return fu;
}

// The shape the five calls on an existing socket share, up to where they
// differ: reject a closed socket and one of the wrong kind, then make the
// future.
//
// It takes the timeout as a number, not the options record. The future
// allocates, and the Nio arguments of each caller are unrooted parameters that
// a collection can free. Each caller copies them into C memory first.
static Wait *begin(NetSock *s, int op, int64_t timeout, Future **out) {
    Future *bad = reject_closed(s, op_name(op));
    if (!bad && op != OP_ACCEPT) bad = reject_wrong_kind(s, op);
    if (bad) {
        *out = bad;
        return NULL;
    }
    TypeDesc *tds[1] = {&rt_td_socket};
    int64_t slots[1] = {(int64_t)(intptr_t)s};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;
    Future *fu = op_future(s);
    rt_gc_top = f.prev;
    *out = fu;
    return wait_new((NetSock *)(intptr_t)slots[0], fu, op, timeout);
}

// The bytes of a byte[], in C memory. It is a copy because a write may outlive
// the call by any number of turns, and the array is the program's to change.
static char *pack(Arr *data, int64_t *len) {
    *len = data ? data->len : 0;
    char *buf = malloc((size_t)(*len ? *len : 1));
    if (!buf) rt_panic("out of memory");
    if (*len) memcpy(buf, rt_arr_bytes(data), (size_t)*len);
    return buf;
}

// Runs the operation now if it can run, and registers it otherwise.
static Future *finish_begin(Wait *w, Future *fu, const char *what) {
    if (try_op(w)) {
        return fu;
    }
    wait_start(w, what);
    return fu;
}

Future *rt_net_accept(void *l, int64_t *opts) {
    NetSock *s = l;
    int64_t timeout = opt_timeout(opts);
    int nodelay = opt_nodelay(opts);
    Future *fu = NULL;
    Wait *w = begin(s, OP_ACCEPT, timeout, &fu);
    if (!w) return fu;
    w->nodelay = nodelay; // applies to the accepted connection
    return finish_begin(w, fu, "accept");
}

Future *rt_net_read(void *sp, int64_t n, int64_t *opts) {
    NetSock *s = sp;
    int64_t timeout = opt_timeout(opts);
    Future *fu = NULL;
    if (n <= 0) {
        Future *bad = op_future(s);
        rt_async_extern_done(bad, 0, NULL,
            net_say("read", "the byte count must be positive", NIO_ERR_INVALID));
        return bad;
    }
    Wait *w = begin(s, OP_READ, timeout, &fu);
    if (!w) return fu;
    w->len = n;
    w->buf = malloc((size_t)n);
    if (!w->buf) rt_panic("out of memory");
    return finish_begin(w, fu, "read");
}

// Answers a byte[] of exactly n bytes, however many arrivals it takes. A peer
// that closes first fails it with END_OF_FILE. It cannot go through begin():
// the future's aux must root the result array and the socket, and td_read_pair
// describes that pair.
Future *rt_net_readExactly(void *sp, int64_t n, int64_t *opts) {
    NetSock *s = sp;
    int64_t timeout = opt_timeout(opts);
    Future *bad = reject_closed(s, "readExactly");
    if (!bad) bad = reject_wrong_kind(s, OP_READ_EXACT);
    if (bad) return bad;
    if (n <= 0) {
        Future *fu = op_future(s);
        rt_async_extern_done(fu, 0, NULL,
            net_say("readExactly", "the byte count must be positive", NIO_ERR_INVALID));
        return fu;
    }

    // s arrives unrooted and both allocations below can collect, so everything
    // is framed until rt_async_extern_new starts tracing the pair.
    TypeDesc *tds[3] = {&rt_td_socket, &td_byte_array, &td_read_pair};
    int64_t slots[3] = {(int64_t)(intptr_t)s, 0, 0};
    GCFrame f = {rt_gc_top, 3, tds, slots, NULL};
    rt_gc_top = &f;
    Arr *a = rt_arr_new(n, 1);
    slots[1] = (int64_t)(intptr_t)a;
    void **pair = rt_alloc(2 * sizeof(void *));
    slots[2] = (int64_t)(intptr_t)pair;
    pair[0] = s;
    pair[1] = a;
    Future *fu = rt_async_extern_new(net_extern_wait, pair, &td_read_pair);
    rt_gc_top = f.prev;

    Wait *w = wait_new(s, fu, OP_READ_EXACT, timeout);
    w->gcbuf = a;
    w->len = n;
    return finish_begin(w, fu, "readExactly");
}

Future *rt_net_write(void *sp, Arr *data, int64_t *opts) {
    NetSock *s = sp;
    int64_t timeout = opt_timeout(opts);
    int64_t len = 0;
    char *buf = pack(data, &len);
    Future *fu = NULL;
    Wait *w = begin(s, OP_WRITE, timeout, &fu);
    if (!w) {
        free(buf);
        return fu;
    }
    w->len = len;
    w->buf = buf;
    return finish_begin(w, fu, "write");
}

Future *rt_net_receive(void *sp, int64_t n, int64_t *opts) {
    NetSock *s = sp;
    int64_t timeout = opt_timeout(opts);
    Future *fu = NULL;
    if (n <= 0) {
        Future *bad = op_future(s);
        rt_async_extern_done(bad, 0, NULL,
            net_say("receive", "the byte count must be positive", NIO_ERR_INVALID));
        return bad;
    }
    Wait *w = begin(s, OP_RECEIVE, timeout, &fu);
    if (!w) return fu;
    w->len = n;
    w->buf = malloc((size_t)n);
    if (!w->buf) rt_panic("out of memory");
    return finish_begin(w, fu, "receive");
}

Future *rt_net_send(void *sp, Str *addr, Arr *data, int64_t *opts) {
    NetSock *s = sp;
    int64_t timeout = opt_timeout(opts);
    struct sockaddr_storage peer;
    socklen_arg_t peerlen = 0;
    int64_t code = 0;
    int ok = resolve(addr, 0, 1, &peer, &peerlen, &code);
    int64_t len = 0;
    char *buf = pack(data, &len);
    Future *fu = NULL;
    Wait *w = begin(s, OP_SEND, timeout, &fu);
    if (!w) {
        free(buf);
        return fu;
    }
    if (!ok) {
        free(buf);
        wait_finish(w, 0, NULL, net_say("send", "not a usable address", code));
        return fu;
    }
    w->peer = peer;
    w->peerlen = peerlen;
    w->len = len;
    w->buf = buf;
    return finish_begin(w, fu, "send");
}

void rt_net_close(void *h) {
    NetSock *s = h;
    if (!s || s->fd < 0) return;
    // Each operation that waits on the socket fails here. Otherwise its future
    // stays pending and the scheduler reports a deadlock. The descriptor
    // closes first, so no waiting operation runs again.
    sock_t fd = (sock_t)s->fd;
    char *path = NULL;
    for (int64_t i = 0; i < nhandles; i++) {
        if (handles[i].block == s) {
            path = handles[i].path;
            break;
        }
    }
    (void)fd;
    sock_shut(s, path);
    for (int side = 0; side < 2; side++) {
        Wait *w = (Wait *)(intptr_t)(side ? s->wwait : s->rwait);
        if (!w) continue;
        wait_finish(w, 0, NULL,
            net_say(op_name(w->op), "the socket was closed", NIO_ERR_NOT_CONNECTED));
    }
    handle_forget(s);
}

Str *rt_net_address(void *h, void **err) {
    NetSock *s = h;
    if (!s || s->fd < 0) {
        *err = net_say("address", "the socket is closed", NIO_ERR_NOT_CONNECTED);
        return NULL;
    }
    struct sockaddr_storage ss;
    socklen_arg_t n = sizeof ss;
    if (getsockname((sock_t)s->fd, (struct sockaddr *)&ss, &n) != 0) {
        *err = net_errno_error("address");
        return NULL;
    }
    return addr_text((struct sockaddr *)&ss, n);
}

Str *rt_net_peer(void *h, void **err) {
    NetSock *s = h;
    if (!s || s->fd < 0) {
        *err = net_say("peer", "the socket is closed", NIO_ERR_NOT_CONNECTED);
        return NULL;
    }
    struct sockaddr_storage ss;
    socklen_arg_t n = sizeof ss;
    if (getpeername((sock_t)s->fd, (struct sockaddr *)&ss, &n) != 0) {
        *err = net_errno_error("peer");
        return NULL;
    }
    return addr_text((struct sockaddr *)&ss, n);
}
