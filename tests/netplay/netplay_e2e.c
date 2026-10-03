#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
/*
 * Headless N-seat netplay, N PROCESSES (never one: each process owns its own
 * nes_netplay singleton and, for the hub, its own libjuice agents).
 *
 *   netplay_e2e lan N     the NES_NET_* env contract: NES_NETPLAY=1 NES_NET_SLOT
 *                         NES_NET_SLOTS NES_NET_BIND NES_NET_PEER over real UDP
 *                         on loopback (host with no peer = accept / LAN hub)
 *   netplay_e2e icehub N  host relay over ICE: every process runs the real
 *                         waiting-room module (rnet_host_ice) over real
 *                         libjuice agents on loopback; the only stand-in is the
 *                         lobby, replaced by a socketpair that carries the
 *                         `signal` payloads. The connected agents are then
 *                         handed to nes_netplay exactly as the launcher's
 *                         capture does (stash_ice_hub_agents / _guest_agent)
 *                         and the match runs with nes_netplay_start
 *                         (transport ICE_HUB: start_ice_hub_adopt /
 *                         adopt_ice_agent).
 *
 *   netplay_e2e lobby N <recomp-net-server binary>
 *                         the whole production path against a REAL lobby
 *                         server spawned on loopback: nes_lobby_* (the adapter
 *                         over the shared client) create/join/ready/start, the
 *                         shared client's waiting-room ICE agents, then
 *                         nes_netplay_capture_lobby_launch (plan + take_hub /
 *                         take_guest_agent BEFORE clear) and nes_netplay_start.
 *                         Needs RNET_HOST_ICE_BIND=127.0.0.1 style loopback
 *                         (set here). Registered only when the server binary
 *                         path is given (-DNES_E2E_LOBBY_SERVER=...).
 *
 * Each seat holds a distinct button byte for its port. After the input delay
 * every process must see every seat's byte on that seat's controller port,
 * with no state or input desync, for TICKS ticks. When N exceeds the build's
 * enforced seat ceiling the test instead requires the refusal (exit 0 iff
 * nes_netplay_start returned -5 "seat_limit"); nothing is faked.
 *
 * Environment: NES_E2E_BASE_PORT (default derived from the pid).
 */
#include "nes_netplay.h"
#include "nes_lobby_client.h"
#include "recomp_net/recomp_net.h"
#include "recomp_net/host_ice.h"
#include "recomp_net/lobby_client.h"
#include "platform/rnet_platform.h"

#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define TICKS 90
#define SETTLE 12
#define MAXN 4

static uint8_t btn_for(int port) { return (uint8_t)(0x10 + port * 0x11); }

/* ---- the match loop, as main_runner's netplay block runs it ---- */
static int run_match(int seat)
{
    int tick = 0, bad = 0, ports = nes_netplay_slot_count();
    /* `seat` is this peer's controller PORT. */
    uint32_t a = 0, b = 0, c = 0;
    while (tick < TICKS) {
        int guard = 0;
        if (nes_netplay_needs_local_sample()) nes_netplay_stage_local(btn_for(seat));
        while (!nes_netplay_poll_admit()) {
            if (nes_netplay_state_desync(&a, &b, &c) || nes_netplay_input_desync(&a, &b, &c)) {
                fprintf(stderr, "seat %d: desync at tick %u\n", seat, a); return 1;
            }
            if (nes_netplay_peer_disconnected(4000)) { fprintf(stderr, "seat %d: peer lost\n", seat); return 1; }
            nes_netplay_wait_recv(2);
            if (++guard > 20000) { fprintf(stderr, "seat %d: admit stalled at tick %d\n", seat, tick); return 1; }
        }
        if (tick >= SETTLE) {
            int p;
            for (p = 0; p < ports; ++p)
                if (nes_netplay_published_buttons(p) != btn_for(p)) {
                    if (!bad++)
                        fprintf(stderr, "seat %d tick %d: port %d = %02x want %02x\n", seat, tick, p,
                                nes_netplay_published_buttons(p), btn_for(p));
                }
        }
        nes_netplay_finish_frame();
        tick++;
    }
    usleep(400 * 1000); /* peers still need our last tick: stay up briefly */
    return bad ? 1 : 0;
}

/* ---- LAN ---- */
static int child_lan(int seat, int n, int base)
{
    char buf[64];
    NesNetplayConfig c;
    nes_netplay_config_defaults(&c);
    setenv("NES_NETPLAY", "1", 1);
    snprintf(buf, sizeof(buf), "%d", seat); setenv("NES_NET_SLOT", buf, 1);
    snprintf(buf, sizeof(buf), "%d", n); setenv("NES_NET_SLOTS", buf, 1);
    snprintf(buf, sizeof(buf), "127.0.0.1:%d", base + seat); setenv("NES_NET_BIND", buf, 1);
    if (seat) { snprintf(buf, sizeof(buf), "127.0.0.1:%d", base); setenv("NES_NET_PEER", buf, 1); }
    setenv("NES_NET_TRANSPORT", "lan", 1);
    setenv("NES_NET_SESSION", "4242", 1);
    nes_netplay_apply_env(&c);
    if (nes_netplay_start(&c) != 0) { fprintf(stderr, "seat %d: start failed: %s\n", seat, nes_netplay_last_error()); return 1; }
    return run_match(seat);
}

/* ---- ICE hub: the lobby is a socketpair ---- */
typedef struct Wire { int type, flag; char text[2048]; } Wire;
enum { W_GO = -1, W_DONE = -2 };
static int g_fd[MAXN];     /* host: fd per guest idx; guest: g_fd[0] to host */
static int g_me, g_n;
static char g_pid[MAXN][8];

static int wsend(int fd, int type, int flag, const char *text)
{
    Wire w;
    memset(&w, 0, sizeof(w));
    w.type = type; w.flag = flag;
    if (text) snprintf(w.text, sizeof(w.text), "%s", text);
    return write(fd, &w, sizeof(w)) == (ssize_t)sizeof(w) ? 0 : -1;
}
static int sig_out(const char *to, int type, int flag, const char *text, void *ctx)
{
    int i;
    (void)ctx;
    if (g_me == 0) {
        for (i = 1; i < g_n; ++i) if (!strcmp(to, g_pid[i])) return wsend(g_fd[i], type, flag, text);
        return -1;
    }
    return strcmp(to, g_pid[0]) ? -1 : wsend(g_fd[0], type, flag, text);
}
static int json_out(const char *j, void *ctx) { (void)j; (void)ctx; return 0; }

static void view_update(RNetHostIce *h)
{
    RNetHostIceView v;
    RNetHostIcePeer peers[MAXN];
    int i;
    memset(&v, 0, sizeof(v));
    v.active = 1; v.is_host = g_me == 0; v.local_slot = g_me;
    if (g_me == 0) {
        for (i = 1; i < g_n; ++i) { peers[i - 1].player_id = g_pid[i]; peers[i - 1].slot = i; }
        v.peer_count = g_n - 1;
    } else { peers[0].player_id = g_pid[0]; peers[0].slot = 0; v.peer_count = 1; }
    v.peers = peers;
    v.stun_host = "";              /* loopback host candidates only */
    v.bind_address = "127.0.0.1";
    v.send_signal = sig_out; v.send_json = json_out;
    rnet_host_ice_update(h, &v);
}

/* 1 = a message was read (and applied/ returned type via *ctl). */
static int wpoll(int fd, int from_idx, RNetHostIce *h, int *ctl)
{
    Wire w;
    ssize_t r = read(fd, &w, sizeof(w));
    if (r != (ssize_t)sizeof(w)) return 0;
    if (w.type < 0) { *ctl = w.type; return 1; }
    (void)rnet_host_ice_push_signal(h, g_pid[from_idx], from_idx, w.type, w.flag, w.text);
    return 1;
}

static int child_icehub(int seat, int n)
{
    RNetHostIce *h = rnet_host_ice_create();
    NesNetplayConfig c;
    int i, go = 0, done_sent = 0, dones = 0;
    uint64_t t0 = rnet_os_monotonic_ms();
    void *agents[MAXN]; int slots[MAXN];
    if (!h || !rnet_host_ice_available()) { fprintf(stderr, "ICE not available in this build\n"); return 1; }
    while (!go) {
        RNetHostIceStatus st;
        int ctl = 0;
        if (rnet_os_monotonic_ms() - t0 > 30000) { fprintf(stderr, "seat %d: waiting room timed out\n", seat); return 1; }
        view_update(h);
        if (seat == 0) {
            for (i = 1; i < n; ++i) while (wpoll(g_fd[i], i, h, &ctl)) if (ctl == W_DONE) dones++;
            rnet_host_ice_status(h, &st);
            if (st.completed == n - 1 && dones >= n - 1) {
                for (i = 1; i < n; ++i) wsend(g_fd[i], W_GO, 0, NULL);
                go = 1;
            }
        } else {
            while (wpoll(g_fd[0], 0, h, &ctl)) if (ctl == W_GO) go = 1;
            rnet_host_ice_status(h, &st);
            if (st.completed == 1 && !done_sent) { wsend(g_fd[0], W_DONE, 0, NULL); done_sent = 1; }
        }
        rnet_os_sleep_micros(2000);
    }
    nes_netplay_config_defaults(&c);
    c.enabled = 1; c.transport = NES_NET_TRANSPORT_ICE_HUB; c.slot_count = n; c.local_slot = seat;
    c.is_host = seat == 0; c.session_id = 4243; c.ports_valid = 1;
    for (i = 0; i < n; ++i) c.port_of_slot[i] = i;
    if (seat == 0) {
        for (i = 1; i < n; ++i) { agents[i - 1] = rnet_host_ice_take_completed(h, g_pid[i]); slots[i - 1] = i; }
        for (i = 0; i < n - 1; ++i) if (!agents[i]) { fprintf(stderr, "host: agent %d not COMPLETED\n", i + 1); return 1; }
        if (nes_netplay_stash_ice_hub_agents(slots, agents, n - 1) != 0) { fprintf(stderr, "host: stash refused\n"); return 1; }
    } else {
        void *a = rnet_host_ice_take_completed(h, g_pid[0]);
        if (!a || nes_netplay_stash_ice_guest_agent(a) != 0) { fprintf(stderr, "guest %d: no agent\n", seat); return 1; }
    }
    rnet_host_ice_destroy(&h);
    if (nes_netplay_start(&c) != 0) { fprintf(stderr, "seat %d: start failed: %s\n", seat, nes_netplay_last_error()); return 1; }
    return run_match(seat);
}

/* ---- lobby ---- */
static void lpump_ms(int ms)
{
    uint64_t until = rnet_os_monotonic_ms() + (uint64_t)ms;
    while (rnet_os_monotonic_ms() < until) { nes_lobby_pump(); rnet_os_sleep_micros(3000); }
}
static void ldiag(int me)
{
    static uint64_t last;
    RNetHostIceStatus st;
    if (!getenv("NES_E2E_DEBUG") || rnet_os_monotonic_ms() - last < 1500) return;
    last = rnet_os_monotonic_ms();
    memset(&st, 0, sizeof(st));
    rnet_lobby_host_ice_status(&st);
    fprintf(stderr, "seat %d: caps valid=%d relay_host=%d via_ice=%d in_lobby=%d members=%d launch=%d\n", me,
            rnet_lobby_match_caps()->valid, rnet_lobby_match_caps()->relay_host, rnet_lobby_match_caps()->relay_via_ice,
            rnet_lobby_in_lobby(), rnet_lobby_member_count(), rnet_lobby_launch_pending());
    fprintf(stderr, "seat %d: host_ice role=%d peers=%d completed=%d reports=%u last=%s rejected=%u\n", me, st.role,
            st.peer_count, st.completed, st.reports_sent, st.last_report, st.rejected);
}
#define LWAIT(cond, ms, what) do { uint64_t u_ = rnet_os_monotonic_ms() + (ms); \
    while (!(cond)) { ldiag(me); if (rnet_os_monotonic_ms() > u_) { fprintf(stderr, "seat %d: timeout: %s (err=%s)\n", me, what, \
        nes_lobby_join_info()->last_error); return 1; } nes_lobby_pump(); rnet_os_sleep_micros(3000); } } while (0)

static int child_lobby(int me, int n, const char *url)
{
    NesLobbyMatchCaps caps;
    NesNetplayConfig c;
    char name[16];
    uint64_t t_start = 0;
    snprintf(name, sizeof(name), "e2e%d", me);
    nes_lobby_set_game_identity("NES Netplay E2E", "e2e-1");
    nes_lobby_set_display_name(name);
    if (nes_lobby_connect(url) != 0) { fprintf(stderr, "seat %d: connect failed\n", me); return 1; }
    LWAIT(nes_lobby_connected() && nes_lobby_player_id()[0], 8000, "welcome");
    memset(&caps, 0, sizeof(caps));
    caps.valid = 1; caps.input_delay = 2; caps.widescreen = 1; caps.widescreen_hud = 1; caps.ws_extra = 7;
    if (me == 0) {
        if (nes_lobby_create("e2e room", "NES Netplay E2E", "e2e-1", "", "127.0.0.1:0", &caps) != 0) return 1;
        LWAIT(nes_lobby_in_lobby() && nes_lobby_is_host(), 8000, "created");
        LWAIT(nes_lobby_member_count() >= n, 30000, "guests seated");
        nes_lobby_set_ready(1);
        LWAIT(nes_lobby_all_ready(), 20000, "all ready");
        while (!nes_lobby_launch_pending()) {
            if (rnet_os_monotonic_ms() - t_start > 700) { (void)nes_lobby_request_start(&caps); t_start = rnet_os_monotonic_ms(); }
            nes_lobby_pump(); rnet_os_sleep_micros(3000);
            ldiag(me);
        }
    } else {
        int joined = 0;
        uint64_t last = 0;
        while (!joined) {
            NesLobbyRow r;
            if (rnet_os_monotonic_ms() - last > 500) { nes_lobby_request_list(); last = rnet_os_monotonic_ms(); }
            nes_lobby_pump(); rnet_os_sleep_micros(3000);
            if (nes_lobby_list_count() > 0 && nes_lobby_list_get(0, &r) && r.max_slots >= n) {
                joined = nes_lobby_join(r.lobby_id, "", "127.0.0.1:0") == 0;
            }
        }
        LWAIT(nes_lobby_in_lobby(), 8000, "joined");
        nes_lobby_set_ready(1);
        LWAIT(nes_lobby_launch_pending(), 60000, "launch");
    }
    /* What the launcher bridge does: validate in fill_launch, capture in clear. */
    if (!nes_netplay_validate_lobby_launch()) { fprintf(stderr, "seat %d: validate refused: %s\n", me, nes_netplay_last_error()); return 1; }
    if (!nes_netplay_capture_lobby_launch()) { fprintf(stderr, "seat %d: capture failed: %s\n", me, nes_netplay_last_error()); return 1; }
    nes_lobby_clear_launch_pending();
    if (!nes_netplay_take_captured(&c)) return 1;
    if (nes_lobby_match_caps()->ws_extra != 7 || !nes_lobby_match_caps()->widescreen) {
        fprintf(stderr, "seat %d: NES match caps did not round-trip through the codec\n", me); return 1;
    }
    if (c.transport != NES_NET_TRANSPORT_ICE_HUB || c.slot_count != n) {
        fprintf(stderr, "seat %d: expected ICE hub / %d seats, got transport %d slots %d\n", me, n, c.transport, c.slot_count); return 1;
    }
    if (nes_netplay_start(&c) != 0) { fprintf(stderr, "seat %d: start failed: %s\n", me, nes_netplay_last_error()); return 1; }
    return run_match(c.port_of_slot[c.local_slot]);
}

static int free_port(void)
{
    struct sockaddr_in a;
    socklen_t l = sizeof(a);
    int fd = socket(AF_INET, SOCK_STREAM, 0), p;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(0x7f000001);
    bind(fd, (struct sockaddr *)&a, sizeof(a));
    getsockname(fd, (struct sockaddr *)&a, &l);
    p = ntohs(a.sin_port);
    close(fd);
    return p;
}

static int main_lobby(int n, const char *server)
{
    char url[64], bind_addr[64], db[128], tmpl[] = "/tmp/nes_e2e_XXXXXX";
    int port = free_port(), i, fails = 0;
    pid_t srv, pids[MAXN];
    if (!mkdtemp(tmpl)) return 2;
    snprintf(db, sizeof(db), "sqlite:%s/s.db?mode=rwc", tmpl);
    snprintf(bind_addr, sizeof(bind_addr), "127.0.0.1:%d", port);
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d", port);
    srv = fork();
    if (srv == 0) {
        const char *lg = getenv("NES_E2E_SERVER_LOG");
        int dn = open(lg && *lg ? lg : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(dn, 1); dup2(dn, 2);
        setenv("BIND_ADDR", bind_addr, 1); setenv("DATABASE_URL", db, 1); setenv("REQUIRE_AUTH", "false", 1);
        setenv("PUBLIC_HOST", "127.0.0.1", 1);
        chdir(tmpl);
        execl(server, server, (char *)NULL);
        _exit(127);
    }
    for (i = 0; i < 100; ++i) { /* wait for the listener */
        struct sockaddr_in a;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_port = htons((unsigned short)port); a.sin_addr.s_addr = htonl(0x7f000001);
        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) { close(fd); break; }
        close(fd); usleep(100 * 1000);
    }
    setenv("RNET_HOST_ICE_BIND", "127.0.0.1", 1);
    setenv("RNET_HOST_ICE_NO_STUN", "1", 1);
    for (i = 0; i < n; ++i) {
        pids[i] = fork();
        if (pids[i] == 0) { int rc; alarm(120); rc = child_lobby(i, n, url); nes_netplay_shutdown(); nes_lobby_disconnect(); _exit(rc); }
        usleep(i == 0 ? 1200 * 1000 : 300 * 1000);
    }
    for (i = 0; i < n; ++i) {
        int st = 0;
        waitpid(pids[i], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { printf("FAIL: seat %d exited abnormally (status %d)\n", i, st); fails++; }
    }
    kill(srv, SIGTERM); waitpid(srv, NULL, 0);
    if (!fails) printf("PASS: lobby %d seats over a real lobby server + ICE hub, %d ticks\n", n, TICKS);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    int n, i, base, fails = 0, icehub;
    pid_t pids[MAXN];
    if (argc < 3) { fprintf(stderr, "usage: %s lan|icehub N\n", argv[0]); return 2; }
    icehub = !strcmp(argv[1], "icehub");
    n = atoi(argv[2]);
    if (n < 2 || n > MAXN) return 2;
    if (!strcmp(argv[1], "lobby") && n <= nes_netplay_max_seats()) return argc > 3 ? main_lobby(n, argv[3]) : 2;
    if (!strcmp(argv[1], "lobby")) icehub = 1;
    if (n > nes_netplay_max_seats()) {
        /* The enforced ceiling: this build cannot drive n seats. The only
         * acceptable outcome is the explicit refusal. */
        NesNetplayConfig c;
        nes_netplay_config_defaults(&c);
        c.enabled = 1; c.slot_count = n; c.transport = icehub ? NES_NET_TRANSPORT_ICE_HUB : NES_NET_TRANSPORT_LAN;
        c.is_host = 1;
        if (nes_netplay_start(&c) == -5 && strstr(nes_netplay_last_error(), "seat_limit")) {
            printf("PASS: %d seats refused by the %d-seat ceiling: %s\n", n, nes_netplay_max_seats(), nes_netplay_last_error());
            return 0;
        }
        printf("FAIL: %d seats were not refused under the %d-seat ceiling\n", n, nes_netplay_max_seats());
        return 1;
    }
    {
        const char *e = getenv("NES_E2E_BASE_PORT");
        base = e && *e ? atoi(e) : 20000 + (int)(getpid() % 2000) * 8;
    }
    g_n = n;
    for (i = 0; i < n; ++i) snprintf(g_pid[i], sizeof(g_pid[i]), "p%d", i);
    if (icehub) {
        int sv[MAXN][2];
        for (i = 1; i < n; ++i) {
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv[i]) != 0) { perror("socketpair"); return 2; }
        }
        for (i = 0; i < n; ++i) {
            pids[i] = fork();
            if (pids[i] == 0) {
                int j, rc;
                alarm(90);
                g_me = i;
                for (j = 1; j < n; ++j) {
                    if (i == 0) { close(sv[j][1]); g_fd[j] = sv[j][0]; fcntl(g_fd[j], F_SETFL, O_NONBLOCK); }
                    else if (j == i) { close(sv[j][0]); g_fd[0] = sv[j][1]; fcntl(g_fd[0], F_SETFL, O_NONBLOCK); }
                    else { close(sv[j][0]); close(sv[j][1]); }
                }
                rc = child_icehub(i, n);
                nes_netplay_shutdown();
                _exit(rc);
            }
        }
        for (i = 1; i < n; ++i) { close(sv[i][0]); close(sv[i][1]); }
    } else {
        for (i = 0; i < n; ++i) {
            pids[i] = fork();
            if (pids[i] == 0) { int rc; alarm(60); rc = child_lan(i, n, base); nes_netplay_shutdown(); _exit(rc); }
            if (i == 0) usleep(150 * 1000);
        }
    }
    for (i = 0; i < n; ++i) {
        int st = 0;
        waitpid(pids[i], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { printf("FAIL: seat %d exited abnormally (status %d)\n", i, st); fails++; }
    }
    if (!fails) printf("PASS: %s %d seats, %d ticks, every port saw every seat's input\n", argv[1], n, TICKS);
    return fails ? 1 : 0;
}
