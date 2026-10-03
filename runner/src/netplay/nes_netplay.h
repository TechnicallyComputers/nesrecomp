#ifndef NES_NETPLAY_H
#define NES_NETPLAY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NES_NETPLAY_PAD_BYTES 5
/* Array bound for session slots (recomp-net RNET_MAX_SLOTS). The ENFORCED
 * ceiling is nes_netplay_max_seats(). */
#define NES_NETPLAY_MAX_SLOTS 8

/* Transport selection (NesNetplayConfig.transport). */
enum {
    NES_NET_TRANSPORT_AUTO = 0,
    NES_NET_TRANSPORT_ICE = 1,       /* peer-to-peer ICE, exactly 2 seats */
    NES_NET_TRANSPORT_LAN = 2,       /* UDP: direct, or the lobby server's relay (any seat count) */
    NES_NET_TRANSPORT_HOST_PORT = 4, /* host relay on the host's advertised UDP port: 2 seats accept, 3+ LAN hub */
    NES_NET_TRANSPORT_ICE_HUB = 5    /* host relay over the waiting room's connected ICE agents */
};

typedef struct NesNetplayConfig {
    int enabled, local_slot, input_player, input_delay;
    uint32_t session_id;
    char bind_hostport[64], peer_hostport[64];
    int transport;
    /* Seats in the session (0 = 2). Must not exceed nes_netplay_max_seats(). */
    int slot_count;
    /* Bit i = session slot i occupied (0 = all of [0, slot_count)). */
    uint32_t occupied_mask;
    /* 1 = this peer is the lobby host (slot 0 in hub transports). */
    int is_host;
    /* Session slot -> emulated controller port (0-based; port p is $4016/P1,
     * $4017/P2, then logical seats 3/4 via nes_input_seat). Valid when
     * ports_valid; otherwise slot == port. */
    int ports_valid;
    int port_of_slot[NES_NETPLAY_MAX_SLOTS];
} NesNetplayConfig;

/* ---- seat ceiling (explicit, enforced) ---------------------------------- */
/* Seats this build can drive: NESRECOMP_INPUT_SEATS (2 unless the title opts
 * in, max 4), further limited by NESRECOMP_GAME_PLAYERS. See nes_net_seats.h.
 * nes_netplay_start refuses anything above it; the lobby clamps to it. */
int nes_netplay_max_seats(void);
/* Why the last start / capture / plan was refused ("" when none). */
const char *nes_netplay_last_error(void);

/* ---- session-slot plan (pure; unit-tested) ------------------------------- */
typedef struct NesSeatPlan {
    int      slot_count;
    uint32_t occupied_mask;
    int      local_slot;                         /* this peer's session slot */
    int      port_of_slot[NES_NETPLAY_MAX_SLOTS];/* controller port per slot */
    int      seat_of_slot[NES_NETPLAY_MAX_SLOTS];/* lobby seat per slot (-1 none) */
} NesSeatPlan;
enum {
    NES_SEAT_PLAN_IDENTITY = 0,   /* session slot == lobby seat (server relay addresses seats) */
    NES_SEAT_PLAN_HOST_FIRST = 1, /* host = slot 0, the rest in seat order (hub transports) */
    NES_SEAT_PLAN_DENSE = 2       /* dense, in seat order (peer-to-peer ICE, 2 seats) */
};
/* seats[0..n): occupied lobby seats; mode as above.
 * Controller port is always the LOBBY seat, so the lobby's P1/P2 labels hold.
 * Returns 0, or -1 with nes_netplay_last_error() set: fewer than 2 seats,
 * more than the ceiling, a seat at/above the ceiling, host/self not seated. */
int nes_netplay_plan_seats(const int *seats, int n, int host_seat, int my_seat,
                           int mode, NesSeatPlan *out);

void nes_netplay_config_defaults(NesNetplayConfig*);
/* NES_NETPLAY, NES_NET_SLOT, NES_NET_SLOTS (seat count), NES_NET_INPUT_PLAYER,
 * NES_NET_INPUT_DELAY, NES_NET_SESSION, NES_NET_BIND, NES_NET_PEER,
 * NES_NET_TRANSPORT=ice|lan. */
void nes_netplay_apply_env(NesNetplayConfig*);
void nes_netplay_set_pending_config(const NesNetplayConfig*);
int nes_netplay_take_pending_config(NesNetplayConfig*);

/* ---- lobby launch capture (launcher bridge) ------------------------------
 * validate: pure check that the lobby's launch can run in this build (seat
 * ceiling, no spectators); call from fill_launch. capture: call from
 * clear_launch_pending BEFORE rnet_lobby_clear_launch_pending(): builds the
 * config from the lobby's launch and, for the ICE hub, takes ownership of the
 * connected agents. take_captured hands the config to the runner. */
int nes_netplay_validate_lobby_launch(void);
int nes_netplay_capture_lobby_launch(void);
int nes_netplay_take_captured(NesNetplayConfig*);
/* Destroy agents captured but never adopted; clear the captured config. */
void nes_netplay_discard_handover(void);
/* Hand already-COMPLETED agents (RNetIceAgent*) to the next start with
 * transport ICE_HUB. Host: session slots + agents; guest: the one host agent.
 * 0, or -1 (no ICE in this build / bad args; caller keeps the agents). */
int nes_netplay_stash_ice_hub_agents(const int *session_slots, void *const *agents, int n);
int nes_netplay_stash_ice_guest_agent(void *agent);

int nes_netplay_start(const NesNetplayConfig*);
void nes_netplay_shutdown(void);
int nes_netplay_active(void);
int nes_netplay_is_running(void);
int nes_netplay_local_slot(void);
int nes_netplay_slot_count(void);
int nes_netplay_input_player(void);
uint32_t nes_netplay_sim_tick(void);
void nes_netplay_stage_local(uint8_t);
int nes_netplay_needs_local_sample(void);
int nes_netplay_poll_admit(void);
void nes_netplay_finish_frame(void);
/* Published buttons for emulated controller PORT p (0-based). */
uint8_t nes_netplay_published_buttons(int);
void nes_netplay_wait_recv(int);
int nes_netplay_input_desync(uint32_t*,uint32_t*,uint32_t*);
int nes_netplay_state_desync(uint32_t*,uint32_t*,uint32_t*);
/* Any peer silent for ms (default 1500): the match ends. */
int nes_netplay_peer_disconnected(uint32_t);
#ifdef __cplusplus
}
#endif
#endif
