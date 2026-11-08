// nn::socket (the bsd socket service). Offline by default: every call that contacts the service returns -1 with errno ENETDOWN, the game takes its offline branch.
// With local play over a LAN on (DM_LAN=1, hle_ldn.c) the calls go to the host's own BSD sockets: the local-wireless network that hle_ldn.c makes is the real LAN (as Atmosphère's
// ldn_mitm does it), so the game's own UDP / TCP traffic (Pia) goes out on the device's Wi-Fi interface with the device's own address.
// Horizon's socket ABI mixes the two families: FreeBSD's sockaddr with a length byte, SOL_SOCKET 0xffff and its option numbers, MSG_DONTWAIT 0x80, but Linux's error numbers
// (EAGAIN 11, as Ryujinx's bsd service answers; Animal Crossing: New Horizons' Pia aborted on a non-blocking RecvFrom's EAGAIN given as FreeBSD's 35) and O_NONBLOCK 0x800. Darwin shares the options;
// Linux (Android) does not, so the values are translated under __linux__. Descriptors are the guest's own small numbers (a table of host descriptors), so fd_set bit maps stay in range.
// Not cut, in both modes: Inet* / nnsocketInet* byte order and address text conversion (pure computation, they keep running in the SDK). Name resolution stays cut (no internet play).
#include "hle.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

int dm_lan_on(void);   // hle_ldn.c: local play over the LAN is on (DM_LAN=1)
enum { SK_ENETDOWN = 100, SK_EAI_FAIL = 4, SK_EBADF = 9, SK_EINVAL = 22, SK_EMFILE = 24, SK_EAFNOSUPPORT = 97 };   // Linux numbering (above)
static __thread int t_skerr = SK_ENETDOWN;
static int sk_fail(void) { t_skerr = SK_ENETDOWN; return -1; }
static int sk_seterr(int e) { t_skerr = e; return -1; }

// ---------- offline: Poll / Select with nothing to wait on ----------
// The host creates no socket offline, so a game's descriptors are all -1, or it passes none. POSIX and Horizon's bsd service ignore negative descriptors and wait out the timeout,
// returning 0. Failing at once made Dark Souls Remastered's network thread ("NexusRevolution Socket", main+0x92c7c4) spin a core at 100% on Poll(fds, 0, 33 ms), 11,000 calls a second.
// An endless wait on nothing never returns on hardware; here it returns 0 after 100 ms: a thread in a host hook pauses for a garbage collector (IL2CPP's, hle_thread.c) only once the
// hook returns, and the caller can still see a shutdown
void dm_guest_sleep_ns(int64_t ns);
static int sk_idle(int64_t ms) { dm_guest_sleep_ns((ms < 0 ? 100 : ms) * 1000000); return 0; }
typedef struct { int32_t fd; int16_t events, revents; } sk_pollfd;
static int off_poll(void *fds, uint64_t n, int ms) {
    sk_pollfd *p = fds;
    for (uint64_t i = 0; i < n; i++) if (p[i].fd >= 0) return sk_fail();
    for (uint64_t i = 0; i < n; i++) p[i].revents = 0;
    return sk_idle(ms);
}
static int off_select(int nfds, const uint64_t *r, const uint64_t *w, const uint64_t *e, const int64_t *tv) {   // fd_set: 1024 bits; timeval: {i64 seconds, i64 microseconds}
    const uint64_t *sets[3] = {r, w, e};
    for (int k = 0; k < 3; k++) for (int fd = 0; sets[k] && fd < nfds && fd < 1024; fd++) if (sets[k][fd >> 6] >> (fd & 63) & 1) return sk_fail();
    return sk_idle(tv ? tv[0] * 1000 + tv[1] / 1000 : -1);
}

// DM_LAN_TRACE=1: one line per call with the LAN on (its arguments and result, the error number on failure)
static int lan_trace(void) { static int t = -1; if (t < 0) { const char *e = getenv("DM_LAN_TRACE"); t = e && *e == '1'; } return t; }
#define TR(r, fmt, ...) do { if (!lan_trace()) break; if ((r) < 0) LOG("[lan] " fmt " -> %ld errno %d", __VA_ARGS__, (long)(r), t_skerr); else LOG("[lan] " fmt " -> %ld", __VA_ARGS__, (long)(r)); } while (0)
// ---------- LAN: the guest's descriptors ----------
#define SK_MAX 256
static int g_fd[SK_MAX];   // host descriptor + 1 (0 = free); guest descriptors start at 3 as a process's do
static pthread_mutex_t g_fdmu = PTHREAD_MUTEX_INITIALIZER;
static int fd_new(int h) {
    pthread_mutex_lock(&g_fdmu);
    for (int i = 3; i < SK_MAX; i++) if (!g_fd[i]) { g_fd[i] = h + 1; pthread_mutex_unlock(&g_fdmu); return i; }
    pthread_mutex_unlock(&g_fdmu); close(h); return sk_seterr(SK_EMFILE);
}
static int fd_host(int s) { return s >= 0 && s < SK_MAX ? g_fd[s] - 1 : -1; }
#define HFD(s) int h = fd_host(s); if (h < 0) return sk_seterr(SK_EBADF)

// Host error number → Horizon's (Linux numbering: the same on Android; Darwin's from 35 on are FreeBSD's)
static int bsd_errno(int e) {
#ifdef __APPLE__
    switch (e) {
    case EAGAIN: return 11; case EINPROGRESS: return 115; case EALREADY: return 114; case ENOTSOCK: return 88; case EDESTADDRREQ: return 89; case EMSGSIZE: return 90;
    case EPROTOTYPE: return 91; case ENOPROTOOPT: return 92; case EPROTONOSUPPORT: return 93; case EOPNOTSUPP: return 95; case EAFNOSUPPORT: return 97; case EADDRINUSE: return 98;
    case EADDRNOTAVAIL: return 99; case ENETDOWN: return 100; case ENETUNREACH: return 101; case ENETRESET: return 102; case ECONNABORTED: return 103; case ECONNRESET: return 104;
    case ENOBUFS: return 105; case EISCONN: return 106; case ENOTCONN: return 107; case ESHUTDOWN: return 108; case ETIMEDOUT: return 110; case ECONNREFUSED: return 111;
    case EHOSTDOWN: return 112; case EHOSTUNREACH: return 113;
    }
#endif
    return e;
}
static int sk_err(void) { return sk_seterr(bsd_errno(errno)); }
#define SKR(x) do { long r_ = (x); return r_ < 0 ? sk_err() : r_; } while (0)

// sockaddr_in as Horizon has it: {u8 len, u8 family (2), be16 port, be32 address, 8 zero bytes}. Only IPv4: the bsd service of a Switch has no IPv6
typedef struct { uint8_t len, family; uint16_t port; uint32_t addr; uint8_t zero[8]; } nx_sin;
static int sa_in(const void *g, unsigned n, struct sockaddr_in *h) {
    const nx_sin *a = g; memset(h, 0, sizeof *h);
    if (!a || n < 8) return sk_seterr(SK_EINVAL);
    if (a->family != 2 && a->family != 0) return sk_seterr(SK_EAFNOSUPPORT);
    h->sin_family = AF_INET; h->sin_port = a->port; h->sin_addr.s_addr = a->addr;
#ifdef __APPLE__
    h->sin_len = sizeof *h;
#endif
    return 0;
}
static void sa_out(const struct sockaddr_in *h, void *g, unsigned *gn) {
    if (!g || !gn) return;
    nx_sin a = {.len = sizeof a, .family = 2, .port = h->sin_port, .addr = h->sin_addr.s_addr};
    memcpy(g, &a, *gn < sizeof a ? *gn : sizeof a); *gn = sizeof a;
}

// Flags and options, FreeBSD → host
static int msg_flags(int f) {
#ifdef __linux__
    int o = f & 7;   // MSG_OOB, MSG_PEEK, MSG_DONTROUTE
    if (f & 0x40) o |= MSG_WAITALL;
    if (f & 0x80) o |= MSG_DONTWAIT;
    return o;
#else
    return f;
#endif
}
#ifdef __linux__
#define NOSIG MSG_NOSIGNAL   // a send on a closed stream must not kill the app with SIGPIPE (FreeBSD sets SO_NOSIGPIPE per socket)
#else
#define NOSIG 0
#endif
static int g_optlog;
static int sk_opt(int lv, int op, int *hl, int *ho) {   // 1: translated, 0: unknown (ignored)
    *hl = lv; *ho = op;
#ifdef __linux__
    if (lv == 0xffff) {
        static const int m[][2] = {{0x4, SO_REUSEADDR}, {0x8, SO_KEEPALIVE}, {0x10, SO_DONTROUTE}, {0x20, SO_BROADCAST}, {0x80, SO_LINGER}, {0x100, SO_OOBINLINE},
                                   {0x200, SO_REUSEPORT}, {0x1001, SO_SNDBUF}, {0x1002, SO_RCVBUF}, {0x1003, SO_SNDLOWAT}, {0x1004, SO_RCVLOWAT}, {0x1005, SO_SNDTIMEO},
                                   {0x1006, SO_RCVTIMEO}, {0x1007, SO_ERROR}, {0x1008, SO_TYPE}};
        *hl = SOL_SOCKET;
        for (unsigned i = 0; i < sizeof m / sizeof *m; i++) if (m[i][0] == op) { *ho = m[i][1]; return 1; }
        return 0;
    }
    if (lv == IPPROTO_IP) {
        static const int m[][2] = {{2, IP_HDRINCL}, {3, IP_TOS}, {4, IP_TTL}, {9, IP_MULTICAST_IF}, {10, IP_MULTICAST_TTL}, {11, IP_MULTICAST_LOOP}, {12, IP_ADD_MEMBERSHIP},
                                   {13, IP_DROP_MEMBERSHIP}};
        for (unsigned i = 0; i < sizeof m / sizeof *m; i++) if (m[i][0] == op) { *ho = m[i][1]; return 1; }
        return 0;
    }
    if (lv == IPPROTO_TCP) {
        static const int m[][2] = {{1, TCP_NODELAY}, {2, TCP_MAXSEG}, {0x100, TCP_KEEPIDLE}, {0x200, TCP_KEEPINTVL}, {0x400, TCP_KEEPCNT}};
        for (unsigned i = 0; i < sizeof m / sizeof *m; i++) if (m[i][0] == op) { *ho = m[i][1]; return 1; }
        return 0;
    }
    return lv == IPPROTO_UDP;
#else
    return lv != 0xffff || op != 0x800;   // SO_NOSIGPIPE is the host's own business
#endif
}

static int lan_socket_(int fam, int type, int proto) {
    if (fam != 2 && fam != 0) return sk_seterr(SK_EAFNOSUPPORT);
    int h = socket(AF_INET, type & 0xffff, proto);   // FreeBSD's SOCK_CLOEXEC 0x10000000, SOCK_NONBLOCK 0x20000000 in the high bits
    if (h < 0) return sk_err();
    if (type & 0x20000000) fcntl(h, F_SETFL, fcntl(h, F_GETFL) | O_NONBLOCK);
#ifdef __APPLE__
    int one = 1; setsockopt(h, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    int s = fd_new(h); if (s >= 0) LOG("[lan] socket %d: type %d protocol %d", s, type & 0xffff, proto);
    return s;
}
static int lan_close_(int s) {
    pthread_mutex_lock(&g_fdmu); int h = fd_host(s); if (h >= 0) g_fd[s] = 0; pthread_mutex_unlock(&g_fdmu);
    if (h < 0) return sk_seterr(SK_EBADF);
    SKR(close(h));
}
static int lan_bind_(int s, const void *a, unsigned n) {
    HFD(s); struct sockaddr_in sa; if (sa_in(a, n, &sa)) return -1;
    SKR(bind(h, (void *)&sa, sizeof sa));
}
static int lan_connect_(int s, const void *a, unsigned n) {
    HFD(s); struct sockaddr_in sa; if (sa_in(a, n, &sa)) return -1;
    SKR(connect(h, (void *)&sa, sizeof sa));
}
static int lan_listen_(int s, int n) { HFD(s); SKR(listen(h, n)); }
static int lan_accept_(int s, void *a, unsigned *an) {
    HFD(s); struct sockaddr_in sa; socklen_t sn = sizeof sa;
    int c = accept(h, (void *)&sa, &sn); if (c < 0) return sk_err();
    sa_out(&sa, a, an); return fd_new(c);
}
static int lan_name_(int s, void *a, unsigned *an, int peer) {
    HFD(s); struct sockaddr_in sa; socklen_t sn = sizeof sa; memset(&sa, 0, sizeof sa);
    if ((peer ? getpeername : getsockname)(h, (void *)&sa, &sn) < 0) return sk_err();
    sa_out(&sa, a, an); return 0;
}
static int lan_shutdown_(int s, int how) { HFD(s); SKR(shutdown(h, how)); }
static int lan_setsockopt_(int s, int lv, int op, const void *v, unsigned n) {
    HFD(s); int hl, ho;
    if (!sk_opt(lv, op, &hl, &ho)) { if (g_optlog++ < 32) LOG("[RED] [lan] SetSockOpt level %#x option %#x: not known, ignored", lv, op); return 0; }
    SKR(setsockopt(h, hl, ho, v, n));
}
static int lan_getsockopt_(int s, int lv, int op, void *v, unsigned *n) {
    HFD(s); int hl, ho;
    if (!sk_opt(lv, op, &hl, &ho)) { if (g_optlog++ < 32) LOG("[RED] [lan] GetSockOpt level %#x option %#x: not known, 0", lv, op); if (v && n && *n >= 4) { memset(v, 0, 4); *n = 4; } return 0; }
    socklen_t sn = n ? *n : 0;
    if (getsockopt(h, hl, ho, v, &sn) < 0) return sk_err();
    if (n) *n = sn;
    if (lv == 0xffff && op == 0x1007 && v && sn >= 4) *(int *)v = bsd_errno(*(int *)v);   // SO_ERROR
    return 0;
}
static int lan_fcntl_(int s, int cmd, int arg) {   // F_GETFL 3 / F_SETFL 4 with O_NONBLOCK = 0x800 (Linux's, as Horizon has it)
    HFD(s); int fl = fcntl(h, F_GETFL); if (fl < 0) return sk_err();
    if (cmd == 3) return 2 | (fl & O_NONBLOCK ? 0x800 : 0);   // O_RDWR
    if (cmd == 4) SKR(fcntl(h, F_SETFL, arg & 0x800 ? fl | O_NONBLOCK : fl & ~O_NONBLOCK));
    return 0;
}
static long lan_recvfrom_(int s, void *b, unsigned long n, int f, void *a, unsigned *an) {
    HFD(s); struct sockaddr_in sa; socklen_t sn = sizeof sa; memset(&sa, 0, sizeof sa);
    long r = recvfrom(h, b, n, msg_flags(f), (void *)&sa, &sn); if (r < 0) return sk_err();
    if (a && an) sa_out(&sa, a, an);
    return r;
}
static long lan_sendto_(int s, const void *b, unsigned long n, int f, const void *a, unsigned an) {
    HFD(s);
    if (!a) SKR(send(h, b, n, msg_flags(f) | NOSIG));
    struct sockaddr_in sa; if (sa_in(a, an, &sa)) return -1;
    SKR(sendto(h, b, n, msg_flags(f) | NOSIG, (void *)&sa, sizeof sa));
}
// Poll: pollfd is {i32 fd, i16 events, i16 revents} on both sides. POLLIN 1, POLLPRI 2, POLLOUT 4, POLLERR 8, POLLHUP 0x10, POLLNVAL 0x20 and POLLRDNORM 0x40 agree; FreeBSD's
// POLLWRNORM is POLLOUT (Linux 0x100), its POLLRDBAND 0x80 / POLLWRBAND 0x100 are not used by sockets here
static int lan_poll_(void *fds, uint64_t n, int ms) {
    sk_pollfd *p = fds; if (n > 64) return sk_seterr(SK_EINVAL);
    struct pollfd hp[64];
    for (uint64_t i = 0; i < n; i++) {
        hp[i].fd = p[i].fd < 0 ? -1 : fd_host(p[i].fd); hp[i].revents = 0;
        hp[i].events = (p[i].events & 7) | (p[i].events & 0x40 ? POLLIN : 0);
        if (p[i].fd >= 0 && hp[i].fd < 0) hp[i].fd = -2;   // not a descriptor of ours → POLLNVAL below
    }
    int bad = 0;
    for (uint64_t i = 0; i < n; i++) if (hp[i].fd == -2) { hp[i].fd = -1; bad = 1; }
    int r = poll(hp, n, bad ? 0 : ms); if (r < 0) return sk_err();
    r = 0;
    for (uint64_t i = 0; i < n; i++) {
        int16_t rv = hp[i].revents & 0x3f;
        if (p[i].fd >= 0 && fd_host(p[i].fd) < 0) rv = 0x20;
        if (rv & POLLIN && p[i].events & 0x40) rv |= 0x40;
        p[i].revents = rv; r += rv != 0;
    }
    return r;
}
static int lan_select_(int nfds, uint64_t *rs, uint64_t *ws, uint64_t *es, const int64_t *tv) {
    struct pollfd hp[64]; int map[64], k = 0;
    if (nfds > SK_MAX) nfds = SK_MAX;
    for (int fd = 0; fd < nfds; fd++) {
        int ev = (rs && rs[fd >> 6] >> (fd & 63) & 1 ? POLLIN : 0) | (ws && ws[fd >> 6] >> (fd & 63) & 1 ? POLLOUT : 0) | (es && es[fd >> 6] >> (fd & 63) & 1 ? POLLPRI : 0);
        if (!ev) continue;
        if (fd_host(fd) < 0 || k == 64) return sk_seterr(SK_EBADF);
        hp[k] = (struct pollfd){fd_host(fd), (short)ev, 0}; map[k++] = fd;
    }
    int r = poll(hp, k, tv ? (int)(tv[0] * 1000 + tv[1] / 1000) : -1); if (r < 0) return sk_err();
    for (int i = 0; i < k; i++) { int fd = map[i]; uint64_t b = 1ull << (fd & 63); if (rs) rs[fd >> 6] &= ~b; if (ws) ws[fd >> 6] &= ~b; if (es) es[fd >> 6] &= ~b; }
    r = 0;
    for (int i = 0; i < k; i++) {
        int fd = map[i]; uint64_t b = 1ull << (fd & 63); short rv = hp[i].revents;
        if (rs && rv & (POLLIN | POLLHUP | POLLERR) && hp[i].events & POLLIN) { rs[fd >> 6] |= b; r++; }
        if (ws && rv & (POLLOUT | POLLERR) && hp[i].events & POLLOUT) { ws[fd >> 6] |= b; r++; }
        if (es && rv & POLLPRI) { es[fd >> 6] |= b; r++; }
    }
    return r;
}

static const char *sa_str(const void *g, unsigned n) {   // trace text of a guest sockaddr_in
    static __thread char b[32]; const nx_sin *a = g; if (!a || n < 8) return "-";
    uint32_t ip = a->addr; snprintf(b, sizeof b, "%u.%u.%u.%u:%u", ip & 255, ip >> 8 & 255, ip >> 16 & 255, ip >> 24, (unsigned)ntohs(a->port)); return b;
}
static int lan_socket(int fam, int type, int proto) { int r = lan_socket_(fam, type, proto); TR(r, "Socket(%d, %#x, %d)", fam, type, proto); return r; }
static int lan_close(int s) { int r = lan_close_(s); TR(r, "Close(%d)", s); return r; }
static int lan_bind(int s, const void *a, unsigned n) { int r = lan_bind_(s, a, n); TR(r, "Bind(%d, %s)", s, sa_str(a, n)); return r; }
static int lan_connect(int s, const void *a, unsigned n) { int r = lan_connect_(s, a, n); TR(r, "Connect(%d, %s)", s, sa_str(a, n)); return r; }
static int lan_listen(int s, int n) { int r = lan_listen_(s, n); TR(r, "Listen(%d, %d)", s, n); return r; }
static int lan_accept(int s, void *a, unsigned *an) { int r = lan_accept_(s, a, an); TR(r, "Accept(%d)", s); return r; }
static int lan_name(int s, void *a, unsigned *an, int peer) { int r = lan_name_(s, a, an, peer); TR(r, "%s(%d) = %s", peer ? "GetPeerName" : "GetSockName", s, a && an ? sa_str(a, *an) : "-"); return r; }
static int lan_shutdown(int s, int how) { int r = lan_shutdown_(s, how); TR(r, "Shutdown(%d, %d)", s, how); return r; }
static int lan_setsockopt(int s, int lv, int op, const void *v, unsigned n) { int r = lan_setsockopt_(s, lv, op, v, n); TR(r, "SetSockOpt(%d, %#x, %#x, %u bytes: %d)", s, lv, op, n, v && n >= 4 ? *(const int *)v : -1); return r; }
static int lan_getsockopt(int s, int lv, int op, void *v, unsigned *n) { int r = lan_getsockopt_(s, lv, op, v, n); TR(r, "GetSockOpt(%d, %#x, %#x)", s, lv, op); return r; }
static int lan_fcntl(int s, int cmd, int arg) { int r = lan_fcntl_(s, cmd, arg); TR(r, "Fcntl(%d, %d, %#x)", s, cmd, arg); return r; }
static long lan_recvfrom(int s, void *b, unsigned long n, int f, void *a, unsigned *an) { long r = lan_recvfrom_(s, b, n, f, a, an); TR(r, "RecvFrom(%d, %lu, %#x)", s, n, f); return r; }
static long lan_sendto(int s, const void *b, unsigned long n, int f, const void *a, unsigned an) { long r = lan_sendto_(s, b, n, f, a, an); TR(r, "SendTo(%d, %lu, %#x, %s)", s, n, f, a ? sa_str(a, an) : "-"); return r; }
static int lan_poll(void *fds, uint64_t n, int ms) { int r = lan_poll_(fds, n, ms); TR(r, "Poll(%llu fds, %d ms)", (unsigned long long)n, ms); return r; }
static int lan_select(int nfds, uint64_t *rs, uint64_t *ws, uint64_t *es, const int64_t *tv) { int r = lan_select_(nfds, rs, ws, es, tv); TR(r, "Select(%d)", nfds); return r; }

// ---------- the entry points: offline unless the LAN is on ----------
#define LAN dm_lan_on()
#define WHY "bsd socket service: offline (-1, ENETDOWN) or the host's sockets with local play over the LAN (DM_LAN)"
HLE_FORCE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", "connects to the bsd socket service; creates no session", Result, h_sk_Initialize, const void *cfg) { (void)cfg; return RES_OK; }
HLE_FORCE("_ZN2nn6socket10InitializeEPvmmi", "connects to the bsd socket service; creates no session", Result, h_sk_Init4, void *mem, uint64_t sz, uint64_t alloc, int conc) {
    (void)mem; (void)sz; (void)alloc; (void)conc; return RES_OK;
}
HLE_FORCE("_ZN2nn6socket8FinalizeEv", "pairs with Initialize", Result, h_sk_Fini, void) { return RES_OK; }
// Socket: the enum (Family, Type, Protocol) and the plain int overloads have the same arguments
HLE_FORCE("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", WHY, int, h_sk_Socket, int a, int b, int c) { return LAN ? lan_socket(a, b, c) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket6SocketEiii", WHY, int, h_sk_Socket3i, int a, int b, int c) { return LAN ? lan_socket(a, b, c) : sk_fail(); }
HLE_FORCE("nnsocketSocket", WHY, int, h_nsk_Socket, int a, int b, int c) { return LAN ? lan_socket(a, b, c) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", WHY, int, h_sk_Bind, int s, const void *a, unsigned n) { return LAN ? lan_bind(s, a, n) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket4BindEiPK8sockaddrj", WHY, int, h_sk_Bind2, int s, const void *a, unsigned n) { return LAN ? lan_bind(s, a, n) : sk_fail(); }
HLE_FORCE("nnsocketBind", WHY, int, h_nsk_Bind, int s, const void *a, unsigned n) { return LAN ? lan_bind(s, a, n) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", WHY, int, h_sk_Connect, int s, const void *a, unsigned n) { return LAN ? lan_connect(s, a, n) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket7ConnectEiPK8sockaddrj", WHY, int, h_sk_Connect2, int s, const void *a, unsigned n) { return LAN ? lan_connect(s, a, n) : sk_fail(); }
HLE_FORCE("nnsocketConnect", WHY, int, h_nsk_Connect, int s, const void *a, unsigned n) { return LAN ? lan_connect(s, a, n) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket6ListenEii", WHY, int, h_sk_Listen, int s, int n) { return LAN ? lan_listen(s, n) : sk_fail(); }
HLE_FORCE("nnsocketListen", WHY, int, h_nsk_Listen, int s, int n) { return LAN ? lan_listen(s, n) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", WHY, int, h_sk_Accept, int s, void *a, unsigned *n) { return LAN ? lan_accept(s, a, n) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket6AcceptEiP8sockaddrPj", WHY, int, h_sk_Accept2, int s, void *a, unsigned *n) { return LAN ? lan_accept(s, a, n) : sk_fail(); }
HLE_FORCE("nnsocketAccept", WHY, int, h_nsk_Accept, int s, void *a, unsigned *n) { return LAN ? lan_accept(s, a, n) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket5CloseEi", WHY, int, h_sk_Close, int s) { return LAN ? lan_close(s) : sk_fail(); }
HLE_FORCE("nnsocketClose", WHY, int, h_nsk_Close, int s) { return LAN ? lan_close(s) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket8ShutdownEii", WHY, int, h_sk_Shutdown, int s, int how) { return LAN ? lan_shutdown(s, how) : sk_fail(); }
HLE_FORCE("nnsocketShutdown", WHY, int, h_nsk_Shutdown, int s, int how) { return LAN ? lan_shutdown(s, how) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket11GetPeerNameEiPNS0_8SockAddrEPj", WHY, int, h_sk_GetPeerName, int s, void *a, unsigned *n) { return LAN ? lan_name(s, a, n, 1) : sk_fail(); }
HLE_FORCE("nnsocketGetPeerName", WHY, int, h_nsk_GetPeerName, int s, void *a, unsigned *n) { return LAN ? lan_name(s, a, n, 1) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj", WHY, int, h_sk_GetSockName, int s, void *a, unsigned *n) { return LAN ? lan_name(s, a, n, 0) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket11GetSockNameEiP8sockaddrPj", WHY, int, h_sk_GetSockName2, int s, void *a, unsigned *n) { return LAN ? lan_name(s, a, n, 0) : sk_fail(); }
HLE_FORCE("nnsocketGetSockName", WHY, int, h_nsk_GetSockName, int s, void *a, unsigned *n) { return LAN ? lan_name(s, a, n, 0) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", WHY, int, h_sk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return LAN ? lan_getsockopt(s, l, o, v, n) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket10GetSockOptEiiiPvPj", WHY, int, h_sk_GetSockOpt2, int s, int l, int o, void *v, unsigned *n) { return LAN ? lan_getsockopt(s, l, o, v, n) : sk_fail(); }
HLE_FORCE("nnsocketGetSockOpt", WHY, int, h_nsk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return LAN ? lan_getsockopt(s, l, o, v, n) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", WHY, int, h_sk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return LAN ? lan_setsockopt(s, l, o, v, n) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket10SetSockOptEiiiPKvj", WHY, int, h_sk_SetSockOpt2, int s, int l, int o, const void *v, unsigned n) { return LAN ? lan_setsockopt(s, l, o, v, n) : sk_fail(); }
HLE_FORCE("nnsocketSetSockOpt", WHY, int, h_nsk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return LAN ? lan_setsockopt(s, l, o, v, n) : sk_fail(); }
// Fcntl is variadic: on the Switch its third argument is in w2 like a fixed one
HLE_FORCE("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", WHY, int, h_sk_Fcntl, int s, int c, int v) { return LAN ? lan_fcntl(s, c, v) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket5FcntlEiiz", WHY, int, h_sk_Fcntl2, int s, int c, int v) { return LAN ? lan_fcntl(s, c, v) : sk_fail(); }
HLE_FORCE("nnsocketFcntl", WHY, int, h_nsk_Fcntl, int s, int c, int v) { return LAN ? lan_fcntl(s, c, v) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket4PollEPNS0_6PollFdEmi", "offline: waits out the timeout when there is nothing to wait on, else fails; the host's poll with the LAN on", int, h_sk_Poll,
          void *f, uint64_t n, int t) { return LAN ? lan_poll(f, n, t) : off_poll(f, n, t); }
HLE_FORCE("_ZN2nn6socket4PollEP6pollfdmi", "offline: waits out the timeout when there is nothing to wait on, else fails; the host's poll with the LAN on", int, h_sk_Poll2,
          void *f, uint64_t n, int t) { return LAN ? lan_poll(f, n, t) : off_poll(f, n, t); }
HLE_FORCE("nnsocketPoll", "offline: waits out the timeout when there is nothing to wait on, else fails; the host's poll with the LAN on", int, h_nsk_Poll,
          void *f, uint64_t n, int t) { return LAN ? lan_poll(f, n, t) : off_poll(f, n, t); }
HLE_FORCE("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", "offline: waits out the timeout when there is nothing to wait on, else fails; the host's poll with the LAN on", int,
          h_sk_Select, int n, void *r, void *w, void *e, void *t) { return LAN ? lan_select(n, r, w, e, t) : off_select(n, r, w, e, t); }
// SDK 4's Select with plain fd_set / timeval (Dark Souls Remastered)
HLE_FORCE("_ZN2nn6socket6SelectEiP6fd_setS2_S2_P7timeval", "offline: waits out the timeout when there is nothing to wait on, else fails; the host's poll with the LAN on", int,
          h_sk_Select2, int n, void *r, void *w, void *e, void *t) { return LAN ? lan_select(n, r, w, e, t) : off_select(n, r, w, e, t); }
HLE_FORCE("_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", WHY, long, h_sk_Recv, int s, void *b, unsigned long n, int f) { return LAN ? lan_recvfrom(s, b, n, f, NULL, NULL) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket4RecvEiPvmi", WHY, long, h_sk_Recv2, int s, void *b, unsigned long n, int f) { return LAN ? lan_recvfrom(s, b, n, f, NULL, NULL) : sk_fail(); }
HLE_FORCE("nnsocketRecv", WHY, long, h_nsk_Recv, int s, void *b, unsigned long n, int f) { return LAN ? lan_recvfrom(s, b, n, f, NULL, NULL) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", WHY, long, h_sk_RecvFrom, int s, void *b, unsigned long n, int f, void *a, unsigned *al) {
    return LAN ? lan_recvfrom(s, b, n, f, a, al) : sk_fail();
}
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", WHY, long, h_sk_RecvFrom2, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { return LAN ? lan_recvfrom(s, b, n, f, a, al) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", WHY, long, h_sk_Send, int s, const void *b, unsigned long n, int f) { return LAN ? lan_sendto(s, b, n, f, NULL, 0) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket4SendEiPKvmi", WHY, long, h_sk_Send2, int s, const void *b, unsigned long n, int f) { return LAN ? lan_sendto(s, b, n, f, NULL, 0) : sk_fail(); }
HLE_FORCE("nnsocketSend", WHY, long, h_nsk_Send, int s, const void *b, unsigned long n, int f) { return LAN ? lan_sendto(s, b, n, f, NULL, 0) : sk_fail(); }
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", WHY, long, h_sk_SendTo, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) {
    return LAN ? lan_sendto(s, b, n, f, a, al) : sk_fail();
}
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", WHY, long, h_sk_SendTo2, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { return LAN ? lan_sendto(s, b, n, f, a, al) : sk_fail(); }
HLE_FORCE("nnsocketRecvMsg", "bsd socket service (cut)", int, h_nsk_RecvMsg, void) { return sk_fail(); }
HLE_FORCE("nnsocketSendMsg", "bsd socket service (cut)", int, h_nsk_SendMsg, void) { return sk_fail(); }
HLE_FORCE("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", "bsd socket service (cut)", int, h_sk_EventFd, unsigned long v, int f) { (void)v; (void)f; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket11EventFdReadEiPm", "bsd socket service (cut)", int, h_sk_EventFdRead, int s, unsigned long *v) { (void)s; (void)v; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket12EventFdWriteEim", "bsd socket service (cut)", int, h_sk_EventFdWrite, int s, unsigned long v) { (void)s; (void)v; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket17ResolverGetOptionEPNS0_14ResolverOptionENS0_17ResolverOptionKeyE", "resolver (cut)", int, h_sk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket17ResolverSetOptionERKNS0_14ResolverOptionE", "resolver (cut)", int, h_sk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE("nnsocketResolverGetOption", "resolver (cut)", int, h_nsk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
HLE_FORCE("nnsocketResolverSetOption", "resolver (cut)", int, h_nsk_ResSet, const void *o) { (void)o; return sk_fail(); }
// Name resolution: cut in both modes (local play needs none)
HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", "name resolution (cut): EAI_FAIL", int, h_sk_Gai, const char *h, const char *s, const void *hint, void **res) {
    (void)h; (void)s; (void)hint; if (res) *res = NULL; return SK_EAI_FAIL;
}
HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", "name resolution (cut): EAI_FAIL", int, h_sk_Gai2, const char *h, const char *s, const void *hint, void **res) {
    (void)h; (void)s; (void)hint; if (res) *res = NULL; return SK_EAI_FAIL;
}
HLE_FORCE("nnsocketGetAddrInfo", "name resolution (cut): EAI_FAIL", int, h_nsk_Gai, const char *h, const char *s, const void *hint, void **res) { (void)h; (void)s; (void)hint; if (res) *res = NULL; return SK_EAI_FAIL; }
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", "pairs with GetAddrInfo (cut)", void, h_sk_FreeAi, void *ai) { (void)ai; }
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", "pairs with GetAddrInfo (cut)", void, h_sk_FreeAi2, void *ai) { (void)ai; }
HLE_FORCE("nnsocketFreeAddrInfo", "pairs with GetAddrInfo (cut)", void, h_nsk_FreeAi, void *ai) { (void)ai; }
HLE_FORCE("_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", "resolver error text", const char *, h_sk_GaiStr, int e) { (void)e; return "network unavailable"; }
HLE_FORCE("nnsocketGAIStrError", "resolver error text", const char *, h_nsk_GaiStr, int e) { (void)e; return "network unavailable"; }
HLE_FORCE("_ZN2nn6socket16GetHostEntByNameEPKcPKNS0_14ResolverOptionEm", "name resolution (cut): NULL", void *, h_sk_GetHostEnt, const char *n, const void *o, unsigned long c) {
    (void)n; (void)o; (void)c; t_skerr = SK_ENETDOWN; return NULL;
}
HLE_FORCE("nnsocketGetHostByNameWithOptions", "name resolution (cut): NULL", void *, h_nsk_GetHost, const char *n, const void *o, unsigned long c) { (void)n; (void)o; (void)c; t_skerr = SK_ENETDOWN; return NULL; }
// The error number of this thread's last call (Linux numbering)
HLE_FORCE("_ZN2nn6socket12GetLastErrorEv", "socket error code kept by the host", int, h_sk_GetLastError, void) { return t_skerr; }
HLE_FORCE("_ZN2nn6socket12GetLastErrnoEv", "socket error code kept by the host", int, h_sk_Errno, void) { return t_skerr; }
HLE_FORCE("nnsocketGetLastErrno", "socket error code kept by the host", int, h_nsk_GetErrno, void) { return t_skerr; }
HLE_FORCE("nnsocketSetLastErrno", "socket error code kept by the host", void, h_nsk_SetErrno, int e) { t_skerr = e; }
