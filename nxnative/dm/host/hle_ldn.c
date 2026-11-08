// Local play over a LAN (DM_LAN=1): nifm and nn::ldn as Atmosphère's ldn_mitm makes them, so the game plays "local wireless" with Switches that run ldn_mitm, and with other ports.
// Offline (the default) nifm reports no network and ldn fails to initialize: the game takes its offline branch, as before.
#include "hle.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

void dm_guest_sleep_ns(int64_t ns);   // hle_thread.c
// DM_LAN_TRACE=1 (as hle_socket.c): one line per nifm / ldn query with its result
static int ldn_trace(void) { static int t = -1; if (t < 0) { const char *e = getenv("DM_LAN_TRACE"); t = e && *e == '1'; } return t; }
#define LT(...) do { if (ldn_trace()) LOG("[lan] " __VA_ARGS__); } while (0)

int dm_lan_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("DM_LAN"); on = e && *e == '1'; if (on) LOG("[lan] local play over the LAN is on (DM_LAN=1)"); } return on; }

// The device's own IPv4 address and subnet mask (network byte order) on the LAN: Wi-Fi (wlan*, Darwin en0) first, else the first other interface that is up, not loopback and
// can broadcast. 0 when there is none
int dm_lan_addr(uint32_t *ip, uint32_t *mask) {
    struct ifaddrs *l, *best = NULL; *ip = *mask = 0;
    if (getifaddrs(&l)) return 0;
    for (struct ifaddrs *i = l; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || i->ifa_flags & IFF_LOOPBACK || !i->ifa_netmask) continue;
        int wifi = !strncmp(i->ifa_name, "wlan", 4) || !strcmp(i->ifa_name, "en0");
        if (wifi) { best = i; break; }
        if (!best && i->ifa_flags & IFF_BROADCAST) best = i;
    }
    if (best) { *ip = ((struct sockaddr_in *)best->ifa_addr)->sin_addr.s_addr; *mask = ((struct sockaddr_in *)best->ifa_netmask)->sin_addr.s_addr; }
    freeifaddrs(l);
    return *ip != 0;
}

// ---------- nifm: offline, or with the LAN on, a local network mode request is granted (the LAN is the local network) ----------
// An internet request stays refused in both modes: online play is out of scope, and ldn needs only the local mode. IP queries give the device's LAN address with the LAN on
static _Atomic int g_nifm_local, g_nifm_req;
static int nifm_up(void) { return dm_lan_on() && g_nifm_local && g_nifm_req; }
HLE_FORCE("_ZN2nn4nifm10InitializeEv", "connects to the nifm network management service; creates no session", Result, h_nifm_Initialize, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4nifm19SetLocalNetworkModeEb", "local network mode: remembered (granted with the LAN on)", void, h_nifm_LocalMode, unsigned on) {
    g_nifm_local = on & 1; if (dm_lan_on()) LOG("[lan] nifm local network mode %s", g_nifm_local ? "on" : "off");
}
HLE_FORCE("_ZN2nn4nifm20SubmitNetworkRequestEv", "network request: granted for the local network mode with the LAN on, else not submitted", void, h_nifm_Submit, void) { g_nifm_req = 1; }
HLE_FORCE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", "network request: granted for the local network mode with the LAN on, else not submitted (returns at once)", void, h_nifm_SubmitWait, void) {
    g_nifm_req = 1; if (dm_lan_on()) LOG("[lan] nifm request (%s): %s", g_nifm_local ? "local" : "internet", nifm_up() ? "granted" : "refused");
}
HLE_FORCE("_ZN2nn4nifm20CancelNetworkRequestEv", "network request cancelled", void, h_nifm_Cancel, void) { g_nifm_req = 0; }
HLE_FORCE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", "the request is never on hold", int, h_nifm_OnHold, void) { return 0; }
HLE_FORCE("_ZN2nn4nifm18IsNetworkAvailableEv", "network available: only a granted local network request with the LAN on", int, h_nifm_Available, void) { static int last = -1; if (last != nifm_up()) LT("nifm IsNetworkAvailable -> %d", nifm_up()); last = nifm_up(); return nifm_up(); }
HLE_FORCE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", "reports failure, shows no system network screen", Result, h_nifm_HandleErr, void) { return nifm_up() ? RES_OK : RES_FAIL; }
static Result nifm_ipcfg(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) {
    uint32_t *p[5] = {a, b, c, d, e}; for (int i = 0; i < 5; i++) if (p[i]) *p[i] = 0;
    uint32_t ip, mask; if (!dm_lan_on() || !dm_lan_addr(&ip, &mask)) return RES_FAIL;
    LT("nifm IP configuration: %08x / %08x", ntohl(ip), ntohl(mask));
    if (a) *a = ip; if (b) *b = mask; if (c) *c = (ip & mask) | htonl(1);   // the gateway is not used by local play: the subnet's first address
    return RES_OK;
}
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", "IP configuration: the LAN address with the LAN on, else none", Result, h_nifm_IpCfg,
          uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) { return nifm_ipcfg(a, b, c, d, e); }
// SDK 10's functions of the same name take socket::InAddr / in_addr, exactly the other way round from the generation above (Super Mario 3D World + Bowser's Fury)
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEPNS_6socket6InAddrES3_S3_S3_S3_", "IP configuration: the LAN address with the LAN on, else none (SDK 10 overload)", Result, h_nifm_IpCfg10,
          uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) { return nifm_ipcfg(a, b, c, d, e); }
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", "IP address: the LAN address with the LAN on, else none", Result, h_nifm_GetIp, uint32_t *a) { return nifm_ipcfg(a, 0, 0, 0, 0); }
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", "IP address: the LAN address with the LAN on, else none (SDK 10 overload)", Result, h_nifm_GetIp10, uint32_t *a) {
    return nifm_ipcfg(a, 0, 0, 0, 0);
}

// ---------- ldn: the local-wireless network as ldn_mitm makes it on the LAN (github.com/spacemeowx2/ldn_mitm, lan_discovery.cpp / lan_protocol.cpp; Ryujinx's LdnMitm is the same) ----------
// Every player keeps a UDP socket on port 11452. A station scans by broadcasting an empty Scan packet to the subnet's broadcast address; each access point (the host of a network)
// answers with its NetworkInfo (ScanResp) and the station keeps the answers of one second. The station then connects over TCP to the host's port 11452 and sends its NodeInfo (Connect);
// the host gives it the next free node (1–7) and sends the whole NetworkInfo (SyncNetwork) to every station whenever a node joins or leaves or the advertise data changes. The game's own
// traffic (Pia) then goes over the LAN with the players' real addresses, which the NetworkInfo's nodes carry. Packets: a 12-byte header (magic 0x11451400, type, compressed flag,
// length, length before compression), then the body, its zero runs compressed (a 0 byte followed by the number of further zeros). Node addresses are in host byte order (as
// nn::ldn's Ipv4Address); a node's MAC address is 02:00 followed by that address's four bytes in memory order, the network's BSSID is its host's.
// The game calls nn::ldn (the SDK's client functions) and the host answers them here, so the structures are the SDK's (the same as the service's: NetworkInfo 0x480 bytes).
enum { LDN_PORT = 11452, LDN_NODES = 8, LDN_ADV_MAX = 384, LDN_SCAN_MAX = 16 };
enum { ST_NONE, ST_INIT, ST_AP, ST_AP_CREATED, ST_STA, ST_STA_CONNECTED, ST_ERROR };   // nn::ldn::State
enum { PK_SCAN, PK_SCAN_RESP, PK_CONNECT, PK_SYNC };
#define LDN_MAGIC 0x11451400u
#define R_LDN_STATE 0x40CBu                              // nn::ldn (module 203) description 32: the call is not valid in this state
#define R_MITM(d) (0xFDu | (uint32_t)(d) << 9)           // ldn_mitm's own results (module 253), returned where it returns them
#pragma pack(push, 1)
typedef struct { uint32_t magic; uint8_t type, compressed; uint16_t length, decompress_length; uint8_t reserved[2]; } lan_hdr;
#pragma pack(pop)
typedef struct { uint8_t raw[6]; } ldn_mac;
typedef struct { uint8_t length; char raw[33]; } ldn_ssid;
typedef struct { ldn_mac bssid; ldn_ssid ssid; int16_t channel; int8_t link_level; uint8_t network_type; uint32_t unk; } ldn_common;
typedef struct { uint32_t ipv4; ldn_mac mac; int8_t node_id, connected; char user_name[33]; uint8_t unk1; int16_t lc_version; uint8_t unk2[16]; } ldn_node;
typedef struct { uint8_t random[16]; uint16_t security_mode; uint8_t accept_policy, unk1[3], node_count_max, node_count; ldn_node nodes[LDN_NODES]; uint16_t unk2, adv_size;
                 uint8_t adv[LDN_ADV_MAX]; uint8_t unk3[148]; } ldn_ldn;
typedef struct { uint64_t lc_id; uint8_t unk1[2]; uint16_t scene_id; uint8_t unk2[4]; } ldn_intent;
typedef struct { uint64_t high, low; } ldn_session;
typedef struct { ldn_intent intent; ldn_session session; } ldn_netid;
typedef struct { ldn_netid id; ldn_common common; ldn_ldn ldn; } ldn_info;
typedef struct { uint16_t security_mode, passphrase_size; uint8_t passphrase[64]; } ldn_secconf;
typedef struct { char user_name[33]; uint8_t unk[15]; } ldn_userconf;
typedef struct { ldn_intent intent; uint16_t channel; uint8_t node_count_max, unk1; uint16_t lc_version; uint8_t unk2[10]; } ldn_netconf;
typedef struct { uint8_t random[16]; ldn_session session; } ldn_secparam;
typedef struct { ldn_netid id; uint32_t network_type; ldn_mac bssid; ldn_ssid ssid; uint8_t unk[16]; uint32_t flag; } ldn_filter;
_Static_assert(sizeof(lan_hdr) == 12 && sizeof(ldn_node) == 0x40 && sizeof(ldn_common) == 0x30 && sizeof(ldn_ldn) == 0x430, "ldn layouts");
_Static_assert(sizeof(ldn_info) == 0x480 && sizeof(ldn_filter) == 0x60 && sizeof(ldn_netconf) == 0x20 && sizeof(ldn_userconf) == 0x30 && sizeof(ldn_secconf) == 0x44, "ldn layouts");

// One TCP or UDP endpoint with its reassembly buffer (TCP delivers packets in pieces)
typedef struct { int fd; uint8_t buf[2048]; int n; } lan_sock;
typedef struct { lan_sock s; int status; } lan_station;   // status: 0 free, 1 accepted (waiting for its Connect), 2 connected
static pthread_mutex_t g_lmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_lthr; static _Atomic int g_lrun;
static _Atomic int g_state; static int g_reason, g_inited;
static void *g_ev;                                         // the game's state change event (AttachStateChangeEvent)
static lan_sock g_udp = {.fd = -1}, g_tcp = {.fd = -1};    // g_tcp: the host's listening socket, or a station's connection to its host
static lan_station g_sta[LDN_NODES - 1];                   // the host's stations: node i + 1
static ldn_info g_info;                                    // the network: the host's own, a station's as last synced
static ldn_info g_scan[LDN_SCAN_MAX]; static int g_nscan;
static uint8_t g_last_conn[LDN_NODES];
static uint32_t g_ip, g_mask;                              // this device, network byte order

static void ev_signal(void) { if (g_ev) hle_signal_system_event(g_ev); }
static void set_state(int s) { if (g_state != s) LOG("[lan] ldn state %d -> %d", (int)g_state, s); g_state = s; ev_signal(); }
static void sock_close(lan_sock *s) { if (s->fd >= 0) close(s->fd); s->fd = -1; s->n = 0; }

// Zero-run compression as lan_protocol.cpp has it: fails (the body goes uncompressed) when the result is not smaller than the input
static int lan_compress(const uint8_t *in, int n, uint8_t *out, int cap) {
    int i = 0, o = 0;
    while (i < n && o < cap) {
        uint8_t c = in[i++];
        if (c) { out[o++] = c; continue; }
        int run = 0; while (i < n && !in[i] && run < 255) { run++; i++; }
        out[o++] = 0; if (o == cap) return -1;
        out[o++] = (uint8_t)run;
    }
    return i == n ? o : -1;
}
static int lan_decompress(const uint8_t *in, int n, uint8_t *out, int cap) {
    int i = 0, o = 0;
    while (i < n && o < cap) {
        uint8_t c = in[i++]; out[o++] = c;
        if (c) continue;
        if (i == n) return -1;
        for (int k = in[i++]; k > 0 && o < cap; k--) out[o++] = 0;
    }
    return i == n ? o : -1;
}
static int lan_send(lan_sock *s, int type, const void *body, int n, const struct sockaddr_in *to) {
    uint8_t pk[sizeof(lan_hdr) + sizeof(ldn_info)]; lan_hdr h = {LDN_MAGIC, (uint8_t)type, 0, (uint16_t)n, 0, {0}};
    if (s->fd < 0 || n > (int)sizeof(ldn_info)) return -1;
    int c = n ? lan_compress(body, n, pk + sizeof h, n) : -1;
    if (c >= 0) { h.compressed = 1; h.decompress_length = (uint16_t)n; h.length = (uint16_t)c; } else if (n) memcpy(pk + sizeof h, body, n);
    memcpy(pk, &h, sizeof h);
    int len = (int)sizeof h + h.length;
    return to ? (int)sendto(s->fd, pk, len, 0, (const void *)to, sizeof *to) : (int)send(s->fd, pk, len, MSG_NOSIGNAL);
}
// Read what is waiting; for each whole packet call fn(type, body, length, sender). Returns -1 when the connection closed or failed
typedef void (*lan_fn)(lan_sock *s, int type, const uint8_t *b, int n, const struct sockaddr_in *from);
static int lan_recv(lan_sock *s, int dgram, lan_fn fn) {
    struct sockaddr_in from; socklen_t fl = sizeof from; memset(&from, 0, sizeof from);
    ssize_t r = dgram ? recvfrom(s->fd, s->buf, sizeof s->buf, MSG_DONTWAIT, (void *)&from, &fl) : recv(s->fd, s->buf + s->n, sizeof s->buf - s->n, MSG_DONTWAIT);
    if (r == 0 && !dgram) return -1;
    if (r < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    s->n = dgram ? (int)r : s->n + (int)r;
    while (s->n >= (int)sizeof(lan_hdr)) {
        lan_hdr h; memcpy(&h, s->buf, sizeof h);
        if (h.magic != LDN_MAGIC || sizeof h + h.length > sizeof s->buf) { s->n = 0; break; }   // not ours: dropped (lan_protocol.cpp does the same)
        int tot = (int)sizeof h + h.length; if (s->n < tot) break;
        uint8_t body[sizeof(ldn_info) + 64]; int bn = h.length;
        if (h.compressed) bn = lan_decompress(s->buf + sizeof h, h.length, body, sizeof body);
        else if (bn <= (int)sizeof body) memcpy(body, s->buf + sizeof h, bn); else bn = -1;
        if (bn >= 0 && (!h.compressed || bn == h.decompress_length)) fn(s, h.type, body, bn, &from);
        memmove(s->buf, s->buf + tot, s->n - tot); s->n -= tot;
        if (dgram) break;
    }
    if (dgram) s->n = 0;
    return 0;
}

static void fake_mac(ldn_mac *m) { uint32_t ip = ntohl(g_ip); m->raw[0] = 2; m->raw[1] = 0; memcpy(m->raw + 2, &ip, 4); }
static void own_node(ldn_node *n, const ldn_userconf *u, int ver) {
    memset(n, 0, sizeof *n); n->ipv4 = ntohl(g_ip); fake_mac(&n->mac); n->connected = 1; n->lc_version = (int16_t)ver;
    memcpy(n->user_name, u->user_name, 32); n->user_name[32] = 0;
}
static void node_changes(void) {   // the event fires when a node joined or left (lan_discovery.cpp isNodeStateChanged)
    int ch = 0;
    for (int i = 0; i < LDN_NODES; i++) if ((uint8_t)g_info.ldn.nodes[i].connected != g_last_conn[i]) { g_last_conn[i] = g_info.ldn.nodes[i].connected; ch = 1; }
    if (ch) { LOG("[lan] ldn nodes: %d of %d connected", g_info.ldn.node_count, g_info.ldn.node_count_max); ev_signal(); }
}
static void host_sync(void) {   // the host: node slots from the stations, then the NetworkInfo to every connected station
    int n = 0;
    for (int i = 0; i < LDN_NODES - 1; i++) { ldn_node *nd = &g_info.ldn.nodes[i + 1]; nd->node_id = (int8_t)(i + 1); nd->connected = g_sta[i].status == 2; n += nd->connected; }
    g_info.ldn.node_count = (uint8_t)(n + 1);
    for (int i = 0; i < LDN_NODES - 1; i++) if (g_sta[i].status == 2 && lan_send(&g_sta[i].s, PK_SYNC, &g_info, sizeof g_info, NULL) < 0) LOG("[RED] [lan] SyncNetwork to node %d failed", i + 1);
    node_changes();
}
static void on_udp(lan_sock *s, int type, const uint8_t *b, int n, const struct sockaddr_in *from) {
    if (type == PK_SCAN && g_state == ST_AP_CREATED) lan_send(s, PK_SCAN_RESP, &g_info, sizeof g_info, from);
    else if (type == PK_SCAN_RESP && n == (int)sizeof(ldn_info)) {
        const ldn_info *in = (const void *)b; int k = 0;
        while (k < g_nscan && memcmp(&g_scan[k].common.bssid, &in->common.bssid, 6)) k++;
        if (k < LDN_SCAN_MAX) { g_scan[k] = *in; if (k == g_nscan) g_nscan++; }
    }
}
static lan_station *g_cur_sta;
static void on_station(lan_sock *s, int type, const uint8_t *b, int n, const struct sockaddr_in *from) {
    (void)s; (void)from;
    if (type != PK_CONNECT || n != (int)sizeof(ldn_node)) { LOG("[RED] [lan] node packet type %d, %d bytes: not a Connect", type, n); return; }
    int id = (int)(g_cur_sta - g_sta) + 1; memcpy(&g_info.ldn.nodes[id], b, sizeof(ldn_node));
    struct in_addr a = {htonl(g_info.ldn.nodes[id].ipv4)}; LOG("[lan] node %d joined: %s (%.32s)", id, inet_ntoa(a), g_info.ldn.nodes[id].user_name);
    g_cur_sta->status = 2; host_sync();
}
static void on_host(lan_sock *s, int type, const uint8_t *b, int n, const struct sockaddr_in *from) {
    (void)s; (void)from;
    if (type != PK_SYNC || n != (int)sizeof(ldn_info)) { LOG("[RED] [lan] host packet type %d, %d bytes: not a SyncNetwork", type, n); return; }
    memcpy(&g_info, b, sizeof g_info);
    if (g_state == ST_STA) set_state(ST_STA_CONNECTED);
    node_changes();
}
static void *lan_worker(void *a) {   // ldn_mitm's worker: polls the UDP socket, the TCP socket and the stations
    (void)a; pthread_setname_np(pthread_self(), "ldn-lan");
    while (g_lrun) {
        struct pollfd p[2 + LDN_NODES - 1]; int np = 0;
        pthread_mutex_lock(&g_lmu);
        p[np++] = (struct pollfd){g_udp.fd, POLLIN, 0}; p[np++] = (struct pollfd){g_tcp.fd, POLLIN, 0};
        for (int i = 0; i < LDN_NODES - 1; i++) p[np++] = (struct pollfd){g_sta[i].status ? g_sta[i].s.fd : -1, POLLIN, 0};
        pthread_mutex_unlock(&g_lmu);
        if (poll(p, np, 100) <= 0) continue;
        pthread_mutex_lock(&g_lmu);
        if (p[0].revents && g_udp.fd == p[0].fd) lan_recv(&g_udp, 1, on_udp);
        if (p[1].revents && g_tcp.fd == p[1].fd) {
            if (g_state == ST_AP_CREATED) {   // a station connects
                int c = accept(g_tcp.fd, NULL, NULL), k = 0;
                while (c >= 0 && k < LDN_NODES - 1 && g_sta[k].status) k++;
                if (c >= 0 && k < LDN_NODES - 1 && k < g_info.ldn.node_count_max - 1) { fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK); g_sta[k].s = (lan_sock){.fd = c}; g_sta[k].status = 1; }
                else if (c >= 0) { LOG("[lan] a station was refused: the network is full"); close(c); }
            } else if (g_state == ST_STA || g_state == ST_STA_CONNECTED) {
                if (p[1].revents & (POLLERR | POLLHUP) || lan_recv(&g_tcp, 0, on_host) < 0) {   // the host left
                    LOG("[lan] the host closed the connection"); sock_close(&g_tcp);
                    if (g_state == ST_STA_CONNECTED) { g_reason = 6; set_state(ST_STA); }   // SignalLost
                }
            }
        }
        for (int i = 0; i < LDN_NODES - 1; i++) if (g_sta[i].status && p[2 + i].revents && g_sta[i].s.fd == p[2 + i].fd) {
            g_cur_sta = &g_sta[i];
            if (p[2 + i].revents & (POLLERR | POLLHUP) || lan_recv(&g_sta[i].s, 0, on_station) < 0) {
                LOG("[lan] node %d left", i + 1); sock_close(&g_sta[i].s); g_sta[i].status = 0; host_sync();
            }
        }
        pthread_mutex_unlock(&g_lmu);
    }
    return NULL;
}
static void stations_reset(void) { for (int i = 0; i < LDN_NODES - 1; i++) { sock_close(&g_sta[i].s); g_sta[i].status = 0; } }
static int lan_listen_on(int type) {   // a socket of this type bound to port 11452 on every address
    int fd = socket(AF_INET, type, 0), one = 1; if (fd < 0) return -1;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(LDN_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    if (bind(fd, (void *)&a, sizeof a) || (type == SOCK_STREAM && listen(fd, 10))) {
        LOG("[RED] [lan] port %d (%s) cannot be bound: %s", LDN_PORT, type == SOCK_STREAM ? "TCP" : "UDP", strerror(errno)); close(fd); return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

#define LDN_WHY "local wireless: cut, or over the LAN as ldn_mitm with DM_LAN (hle_ldn.c)"
#define LOCKED(x) do { pthread_mutex_lock(&g_lmu); Result r_ = (x); pthread_mutex_unlock(&g_lmu); return r_; } while (0)
static Result ldn_init(void) {
    if (!dm_lan_on()) return RES_FAIL;
    if (g_inited) return RES_OK;
    if (!dm_lan_addr(&g_ip, &g_mask)) { LOG("[RED] [lan] ldn: the device has no LAN address (Wi-Fi off?): local play is unavailable"); return RES_FAIL; }
    g_udp.fd = lan_listen_on(SOCK_DGRAM); if (g_udp.fd < 0) return RES_FAIL;
    memset(&g_info, 0, sizeof g_info); stations_reset(); g_reason = 0;
    g_lrun = 1; if (pthread_create(&g_lthr, NULL, lan_worker, NULL)) { g_lrun = 0; sock_close(&g_udp); return RES_FAIL; }
    g_inited = 1; struct in_addr a = {g_ip}; LOG("[lan] ldn initialized on %s, port %d", inet_ntoa(a), LDN_PORT);
    set_state(ST_INIT); return RES_OK;
}
HLE_FORCE("_ZN2nn3ldn10InitializeEv", LDN_WHY, Result, h_ldn_Init, void) { LOCKED(ldn_init()); }
HLE_FORCE("_ZN2nn3ldn8FinalizeEv", LDN_WHY, void, h_ldn_Fini, void) {
    pthread_mutex_lock(&g_lmu); int run = g_inited; g_lrun = 0; pthread_mutex_unlock(&g_lmu);
    if (!run) return;
    pthread_join(g_lthr, NULL);
    pthread_mutex_lock(&g_lmu); sock_close(&g_udp); sock_close(&g_tcp); stations_reset(); g_inited = 0; set_state(ST_NONE); pthread_mutex_unlock(&g_lmu);
}
HLE_FORCE("_ZN2nn3ldn8GetStateEv", LDN_WHY, int, h_ldn_State, void) { static int last = -1; if (last != g_state) LT("ldn GetState -> %d", (int)g_state); last = g_state; return g_state; }
HLE_FORCE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", "local wireless state event: bound, fires on state and node changes with DM_LAN", void, h_ldn_Attach, void *ev) {
    hle_rebind_system_event(ev, 1); g_ev = ev; LT("ldn AttachStateChangeEvent(%p)", ev);
}
static Result ldn_reopen(int to) {   // Open/CloseAccessPoint, Open/CloseStation: drop the network, go to the new state
    if (g_state == ST_NONE) return R_LDN_STATE;
    if (to == ST_AP || to == ST_STA) g_reason = 0;
    sock_close(&g_tcp); stations_reset(); set_state(to); return RES_OK;
}
HLE_FORCE("_ZN2nn3ldn15OpenAccessPointEv", LDN_WHY, Result, h_ldn_OpenAp, void) { LOCKED(ldn_reopen(ST_AP)); }
HLE_FORCE("_ZN2nn3ldn16CloseAccessPointEv", LDN_WHY, Result, h_ldn_CloseAp, void) { LOCKED(ldn_reopen(ST_INIT)); }
HLE_FORCE("_ZN2nn3ldn11OpenStationEv", LDN_WHY, Result, h_ldn_OpenSta, void) { LOCKED(ldn_reopen(ST_STA)); }
HLE_FORCE("_ZN2nn3ldn12CloseStationEv", LDN_WHY, Result, h_ldn_CloseSta, void) { LOCKED(ldn_reopen(ST_INIT)); }
static Result ldn_disc(void) { if (g_state != ST_STA && g_state != ST_STA_CONNECTED) return R_LDN_STATE; sock_close(&g_tcp); set_state(ST_STA); return RES_OK; }
HLE_FORCE("_ZN2nn3ldn10DisconnectEv", LDN_WHY, Result, h_ldn_Disc, void) { LOCKED(ldn_disc()); }
static Result ldn_destroy(void) { if (g_state != ST_AP && g_state != ST_AP_CREATED) return R_LDN_STATE; sock_close(&g_tcp); stations_reset(); set_state(ST_AP); return RES_OK; }
HLE_FORCE("_ZN2nn3ldn14DestroyNetworkEv", LDN_WHY, Result, h_ldn_Destroy, void) { LOCKED(ldn_destroy()); }

// Scan: broadcast, then one second of answers, filtered as lan_discovery.cpp scan() does
static Result ldn_scan(ldn_info *out, int *count, int max, const ldn_filter *f) {
    if (count) *count = 0;
    if (g_state == ST_NONE) return R_LDN_STATE;
    pthread_mutex_lock(&g_lmu); g_nscan = 0;
    struct sockaddr_in b = {.sin_family = AF_INET, .sin_port = htons(LDN_PORT), .sin_addr.s_addr = g_ip | ~g_mask};
    int r = lan_send(&g_udp, PK_SCAN, NULL, 0, &b);
    pthread_mutex_unlock(&g_lmu);
    if (r < 0) { LOG("[RED] [lan] scan broadcast failed: %s", strerror(errno)); return R_MITM(20); }
    dm_guest_sleep_ns(1000000000ll);
    pthread_mutex_lock(&g_lmu);
    int k = 0;
    for (int i = 0; i < g_nscan && k < max; i++) {
        const ldn_info *in = &g_scan[i]; int ok = 1;
        if (f->flag & 1) ok &= f->id.intent.lc_id == in->id.intent.lc_id;
        if (f->flag & 2) ok &= !memcmp(&f->id.session, &in->id.session, sizeof f->id.session);
        if (f->flag & 4) ok &= f->network_type == in->common.network_type;
        if (f->flag & 16) ok &= f->ssid.length == in->common.ssid.length && !memcmp(f->ssid.raw, in->common.ssid.raw, f->ssid.length);
        if (f->flag & 32) ok &= f->id.intent.scene_id == in->id.intent.scene_id;
        if (ok) out[k++] = *in;
    }
    LOG("[lan] scan: %d network(s) answered, %d match", g_nscan, k);
    pthread_mutex_unlock(&g_lmu);
    if (count) *count = k;
    return RES_OK;
}
HLE_FORCE_BIG("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", LDN_WHY, Result, h_ldn_Scan, void *out, int *n, int max, const void *f, int ch) {
    (void)ch; if (!dm_lan_on()) { if (n) *n = 0; return RES_FAIL; } return ldn_scan(out, n, max, f);
}
HLE_FORCE_BIG("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", LDN_WHY, Result, h_ldn_ScanP, void *out, int *n, int max, const void *f, int ch) {
    (void)ch; if (!dm_lan_on()) { if (n) *n = 0; return RES_FAIL; } return ldn_scan(out, n, max, f);
}

// CreateNetwork: the TCP listener, then this device as node 0 (lan_discovery.cpp createNetwork); a private network keeps the session ID it is given
static Result ldn_create(const ldn_netconf *nc, const ldn_secconf *sc, const ldn_userconf *uc, const ldn_session *sid) {
    if (g_state != ST_AP) return R_LDN_STATE;
    sock_close(&g_tcp); g_tcp.fd = lan_listen_on(SOCK_STREAM); if (g_tcp.fd < 0) return R_MITM(7);
    uint16_t adv = g_info.ldn.adv_size; uint8_t advd[LDN_ADV_MAX]; memcpy(advd, g_info.ldn.adv, sizeof advd);   // SetAdvertiseData may come first
    memset(&g_info, 0, sizeof g_info); g_info.ldn.adv_size = adv; memcpy(g_info.ldn.adv, advd, sizeof advd);
    fake_mac(&g_info.common.bssid); g_info.common.link_level = 3; g_info.common.network_type = 2;
    g_info.common.ssid.length = 32; memcpy(g_info.common.ssid.raw, "12345678123456781234567812345678", 32);
    g_info.common.channel = nc->channel ? (int16_t)nc->channel : 6;
    for (int i = 0; i < LDN_NODES; i++) g_info.ldn.nodes[i].node_id = (int8_t)i;
    g_info.ldn.node_count_max = nc->node_count_max;
    // As the ldn service does (and Ryujinx's): security mode 0 (any) becomes Retail (1), and the security parameter gets 16 random bytes, from which the game's
    // session keys come (ldn_mitm leaves them zero)
    g_info.ldn.security_mode = sc->security_mode ? sc->security_mode : 1; arc4random_buf(g_info.ldn.random, sizeof g_info.ldn.random);
    LT("ldn CreateNetwork: security mode %d, passphrase %d bytes, channel %d, version %d, user '%.32s'", sc->security_mode, sc->passphrase_size, nc->channel, nc->lc_version, uc->user_name);
    if (sid) g_info.id.session = *sid; else arc4random_buf(&g_info.id.session, sizeof g_info.id.session);
    g_info.id.intent = nc->intent;
    own_node(&g_info.ldn.nodes[0], uc, nc->lc_version);
    memset(g_last_conn, 0, sizeof g_last_conn);
    set_state(ST_AP_CREATED); host_sync();
    LOG("[lan] network created: local communication ID %016llx scene %u, %d nodes at most", (unsigned long long)nc->intent.lc_id, nc->intent.scene_id, nc->node_count_max);
    return RES_OK;
}
HLE_FORCE_BIG("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", LDN_WHY, Result, h_ldn_Create, const void *a, const void *b, const void *c) {
    if (!dm_lan_on()) return RES_FAIL; LOCKED(ldn_create(a, b, c, NULL));
}
HLE_FORCE_BIG("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE", LDN_WHY,
          Result, h_ldn_CreateP, const void *a, const void *b, const void *c, const void *d, int n, const void *e) {
    (void)n; (void)e; if (!dm_lan_on()) return RES_FAIL; LOCKED(ldn_create(a, b, d, &((const ldn_secparam *)c)->session));
}
// Connect: TCP to node 0's address, our NodeInfo, then up to 3 seconds for the host's SyncNetwork (ldn_mitm waits one second and returns success either way)
static Result ldn_connect(const ldn_info *ni, const ldn_userconf *uc, int ver) {
    pthread_mutex_lock(&g_lmu);
    if (g_state != ST_STA) { pthread_mutex_unlock(&g_lmu); return R_LDN_STATE; }
    if (!ni->ldn.node_count) { pthread_mutex_unlock(&g_lmu); return R_MITM(30); }
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(LDN_PORT), .sin_addr.s_addr = htonl(ni->ldn.nodes[0].ipv4)};
    sock_close(&g_tcp); g_reason = 0;
    pthread_mutex_unlock(&g_lmu);
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return R_MITM(6);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int r = connect(fd, (void *)&a, sizeof a);
    if (r && errno == EINPROGRESS) { struct pollfd p = {fd, POLLOUT, 0}; int e = 0; socklen_t el = sizeof e; r = poll(&p, 1, 3000) == 1 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) && !e ? 0 : -1; }
    if (r) { LOG("[RED] [lan] connect to the host %s failed", inet_ntoa(a.sin_addr)); close(fd); return R_MITM(31); }
    ldn_node me; own_node(&me, uc, ver);
    pthread_mutex_lock(&g_lmu);
    g_tcp = (lan_sock){.fd = fd}; memset(g_last_conn, 0, sizeof g_last_conn);
    r = lan_send(&g_tcp, PK_CONNECT, &me, sizeof me, NULL);
    pthread_mutex_unlock(&g_lmu);
    if (r < 0) { LOG("[RED] [lan] Connect packet to the host failed"); return R_MITM(32); }
    LOG("[lan] connecting to the host %s", inet_ntoa(a.sin_addr));
    for (int i = 0; i < 30 && g_state != ST_STA_CONNECTED; i++) dm_guest_sleep_ns(100000000ll);
    if (g_state != ST_STA_CONNECTED) LOG("[RED] [lan] no SyncNetwork from the host within 3 s");
    return RES_OK;
}
HLE_FORCE_BIG("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", LDN_WHY, Result, h_ldn_Connect,
          const void *a, const void *b, const void *c, int v, int o) { (void)b; (void)o; return dm_lan_on() ? ldn_connect(a, c, v) : RES_FAIL; }
// ConnectPrivate names the network by its session ID: a scan finds its host first (ldn_mitm does not implement it)
HLE_FORCE_BIG("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE", LDN_WHY,
          Result, h_ldn_ConnectP, const void *a, const void *b, const void *c, const void *d, int v, int o) {
    (void)b; (void)o; if (!dm_lan_on()) return RES_FAIL;
    static ldn_info found; static ldn_filter f; memset(&f, 0, sizeof f);
    f.id.intent = ((const ldn_netconf *)a)->intent; f.id.session = ((const ldn_secparam *)c)->session; f.flag = 1 | 2 | 32;
    int n = 0; Result r = ldn_scan(&found, &n, 1, &f);
    if (r || !n) { LOG("[RED] [lan] ConnectPrivate: the network was not found"); return r ? r : R_MITM(30); }
    return ldn_connect(&found, d, v);
}
static Result ldn_netinfo(ldn_info *o) { if (g_state != ST_AP_CREATED && g_state != ST_STA_CONNECTED) return R_LDN_STATE; *o = g_info; return RES_OK; }
HLE_FORCE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", LDN_WHY, Result, h_ldn_NetInfo, void *o) {
    if (!dm_lan_on()) return RES_FAIL;
    pthread_mutex_lock(&g_lmu); Result r = ldn_netinfo(o); pthread_mutex_unlock(&g_lmu); LT("ldn GetNetworkInfo -> %#x (state %d, %d nodes)", r, (int)g_state, g_info.ldn.node_count); return r;
}
HLE_FORCE("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", "the device's LAN address and mask, host byte order (ldn_mitm's GetIpv4Address)", Result, h_ldn_Ip,
          uint32_t *a, uint32_t *m) {
    uint32_t ip = 0, mask = 0; Result r = dm_lan_on() && dm_lan_addr(&ip, &mask) ? RES_OK : RES_FAIL;
    if (a) *a = ntohl(ip); if (m) *m = ntohl(mask); LT("ldn GetIpv4Address -> %#x (%08x / %08x)", r, ntohl(ip), ntohl(mask)); return r;
}
static Result ldn_secprm(ldn_secparam *o) { if (g_state != ST_AP_CREATED && g_state != ST_STA_CONNECTED) return R_LDN_STATE; memcpy(o->random, g_info.ldn.random, 16); o->session = g_info.id.session; return RES_OK; }
HLE_FORCE("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", LDN_WHY, Result, h_ldn_SecPrm, void *o) {
    if (!dm_lan_on()) return RES_FAIL;
    pthread_mutex_lock(&g_lmu); Result r = ldn_secprm(o); pthread_mutex_unlock(&g_lmu); LT("ldn GetSecurityParameter -> %#x", r); return r;
}
static Result ldn_netcfg(ldn_netconf *o) {
    if (g_state != ST_AP_CREATED && g_state != ST_STA_CONNECTED) return R_LDN_STATE;
    memset(o, 0, sizeof *o); o->intent = g_info.id.intent; o->channel = (uint16_t)g_info.common.channel; o->node_count_max = g_info.ldn.node_count_max;
    o->lc_version = (uint16_t)g_info.ldn.nodes[0].lc_version; return RES_OK;
}
HLE_FORCE("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", LDN_WHY, Result, h_ldn_NetCfg, void *o) {
    if (!dm_lan_on()) return RES_FAIL;
    pthread_mutex_lock(&g_lmu); Result r = ldn_netcfg(o); pthread_mutex_unlock(&g_lmu); LT("ldn GetNetworkConfig -> %#x", r); return r;
}
static Result ldn_adv(const void *d, uint64_t n) {
    if (n > LDN_ADV_MAX) return R_MITM(10);
    if (n && d) memcpy(g_info.ldn.adv, d, n);
    g_info.ldn.adv_size = (uint16_t)n;
    if (g_state == ST_AP_CREATED) host_sync();
    return RES_OK;
}
HLE_FORCE("_ZN2nn3ldn16SetAdvertiseDataEPKvm", LDN_WHY, Result, h_ldn_Adv, const void *d, uint64_t n) {
    if (!dm_lan_on()) return RES_FAIL;
    pthread_mutex_lock(&g_lmu); Result r = ldn_adv(d, n); pthread_mutex_unlock(&g_lmu); LT("ldn SetAdvertiseData(%llu bytes) -> %#x", (unsigned long long)n, r); return r;
}
HLE_FORCE("_ZN2nn3ldn19GetDisconnectReasonEv", "why the last network ended (0 none)", int, h_ldn_DiscReason, void) { LT("ldn GetDisconnectReason -> %d", g_reason); return g_reason; }
// Accept filters and policies are not carried by ldn_mitm's protocol: accepted and ignored, as it does
HLE_FORCE("_ZN2nn3ldn20AddAcceptFilterEntryENS0_10MacAddressE", LDN_WHY, Result, h_ldn_Filter, uint64_t mac) { LT("ldn AddAcceptFilterEntry(%012llx)", (unsigned long long)(mac & 0xffffffffffffull)); return dm_lan_on() ? RES_OK : RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", LDN_WHY, Result, h_ldn_Policy, int p) { LT("ldn SetStationAcceptPolicy(%d)", p); return dm_lan_on() ? RES_OK : RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", LDN_WHY, Result, h_ldn_Reject, uint32_t a) { LT("ldn Reject(%08x)", a); return dm_lan_on() ? RES_OK : RES_FAIL; }
