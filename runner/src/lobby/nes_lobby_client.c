#include "nes_lobby_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(NES_HAS_LOBBY_CLIENT)

const char *nes_lobby_default_url(void)
{
    return "ws://netplay.technicallycomputers.ca:8765";
}
void nes_lobby_init(void) {}
const char *nes_lobby_host_player_id(void) { return ""; }
int  nes_lobby_try_fill_launch(NesLobbyJoinInfo *out) { (void)out; return 0; }
int  nes_lobby_connect(const char *ws_url) { (void)ws_url; return -1; }
void nes_lobby_disconnect(void) {}
int  nes_lobby_connected(void) { return 0; }
void nes_lobby_set_display_name(const char *name) { (void)name; }
const char *nes_lobby_display_name(void) { return ""; }
const char *nes_lobby_player_id(void) { return ""; }
void nes_lobby_pump(void) {}
void nes_lobby_request_list(void) {}
int  nes_lobby_list_count(void) { return 0; }
int  nes_lobby_list_get(int index, NesLobbyRow *out) { (void)index; (void)out; return 0; }
void nes_lobby_set_game_identity(const char *a, const char *b) { (void)a; (void)b; }
const char *nes_lobby_game_version(void) { return NES_GAME_VERSION; }
int  nes_lobby_create(const char *a, const char *b, const char *c, const char *d,
                       const char *e, const NesLobbyMatchCaps *f)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return -1; }
int  nes_lobby_join(const char *a, const char *b, const char *c)
{ (void)a; (void)b; (void)c; return -1; }
int  nes_lobby_leave(void) { return -1; }
int  nes_lobby_in_lobby(void) { return 0; }
int  nes_lobby_is_host(void) { return 0; }
const NesLobbyJoinInfo *nes_lobby_join_info(void)
{
    static NesLobbyJoinInfo z;
    return &z;
}
const NesLobbyMatchCaps *nes_lobby_match_caps(void)
{
    static NesLobbyMatchCaps z;
    return &z;
}
int  nes_lobby_set_match_caps(const NesLobbyMatchCaps *c) { (void)c; return -1; }
int  nes_lobby_member_count(void) { return 0; }
int  nes_lobby_member_get(int index, NesLobbyMember *out) { (void)index; (void)out; return 0; }
int  nes_lobby_local_ready(void) { return 0; }
int  nes_lobby_all_ready(void) { return 0; }
int  nes_lobby_set_ready(int ready) { (void)ready; return -1; }
int  nes_lobby_request_start(const NesLobbyMatchCaps *c) { (void)c; return -1; }
int  nes_lobby_launch_pending(void) { return 0; }
void nes_lobby_clear_launch_pending(void) {}
int  nes_lobby_send_signal(int type, int flag, const char *text)
{
    (void)type;
    (void)flag;
    (void)text;
    return -1;
}
int  nes_lobby_poll_signal(int *type, int *flag, char *text, size_t text_cap)
{
    (void)type;
    (void)flag;
    (void)text;
    (void)text_cap;
    return 0;
}

#else /* NES_HAS_LOBBY_CLIENT */

/*
 * Adapter over recomp-net's shared lobby client (recomp_net/lobby_client.h).
 *
 * This file used to be a 1,282-line fork of the pre-lift SNES client with its
 * own WebSocket/SHA-1 copies, a hard-coded "max_slots":2, and none of the host
 * relay, hub handover, spectator or addressed-signal protocol. Everything
 * here now delegates; what remains is the NES-specific part: the title's
 * match_caps keys (widescreen settings) carried through the shared client's
 * caps codec, the seat ceiling, and the nes_lobby_* spellings the launcher
 * bridge and runner already use.
 */
#include "nes_net_seats.h"
#include "recomp_net/lobby_client.h"
#include "recomp_net/host_ice.h"

#include <stdio.h>
#include <string.h>

#define NES_LOBBY_LEGACY_URL "ws://netplay.technicallycomputers.ca:8765"

/* NES caps keys, stored in RNetLobbyMatchCaps.ext. */
typedef struct NesCapsExt {
    unsigned char widescreen, widescreen_hud, ignore_aspect;
    int           ws_extra;
} NesCapsExt;

static int caps_write_extra(const RNetLobbyMatchCaps *c, char *out, size_t cap, void *ctx)
{
    NesCapsExt e;
    int n;
    (void)ctx;
    memcpy(&e, c->ext.bytes, sizeof(e));
    n = snprintf(out, cap,
                 ",\"widescreen\":%s,\"widescreen_hud\":%s,\"ignore_aspect\":%s,"
                 "\"ws_extra\":%d",
                 e.widescreen ? "true" : "false",
                 e.widescreen_hud ? "true" : "false",
                 e.ignore_aspect ? "true" : "false", e.ws_extra);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

static void caps_parse_extra(const char *json, RNetLobbyMatchCaps *c, void *ctx)
{
    NesCapsExt e;
    (void)ctx;
    memset(&e, 0, sizeof(e));
    e.widescreen = (unsigned char)rnet_lobby_json_get_bool(json, "widescreen", 0);
    e.widescreen_hud = (unsigned char)rnet_lobby_json_get_bool(json, "widescreen_hud", 1);
    e.ignore_aspect = (unsigned char)rnet_lobby_json_get_bool(json, "ignore_aspect", 0);
    e.ws_extra = rnet_lobby_json_get_int(json, "ws_extra", 0);
    if (e.ws_extra < 0) e.ws_extra = 0;
    memcpy(c->ext.bytes, &e, sizeof(e));
}

static int s_inited;
static NesLobbyJoinInfo s_join;
static NesLobbyMatchCaps s_caps;

void nes_lobby_init(void)
{
    RNetLobbyConfig cfg;
    RNetLobbyCapsCodec codec;
    if (s_inited) return;
    s_inited = 1;
    memset(&cfg, 0, sizeof(cfg));
    cfg.game_version = NES_GAME_VERSION;
    cfg.platform = "nes";
    cfg.max_players = nes_net_seat_cap();
    cfg.legacy_env_prefix = "NES_NET_";
    rnet_lobby_configure(&cfg);
    memset(&codec, 0, sizeof(codec));
    codec.write_extra = caps_write_extra;
    codec.parse_extra = caps_parse_extra;
    rnet_lobby_set_caps_codec(&codec);
    /* The NES session has no spectator seat: do not open a gallery. */
    rnet_lobby_set_allow_spectators(0);
}

const char *nes_lobby_default_url(void)
{
    const char *e;
    nes_lobby_init();
    e = getenv("RNET_LOBBY_URL");
    if ((!e || !*e)) e = getenv("NES_NET_LOBBY_URL");
    return e && *e ? rnet_lobby_default_url() : NES_LOBBY_LEGACY_URL;
}
int  nes_lobby_connect(const char *ws_url)
{
    nes_lobby_init();
    return rnet_lobby_connect(ws_url && *ws_url ? ws_url : nes_lobby_default_url());
}
void nes_lobby_disconnect(void) { rnet_lobby_disconnect(); }
int  nes_lobby_connected(void) { return rnet_lobby_connected(); }
void nes_lobby_set_display_name(const char *n) { nes_lobby_init(); rnet_lobby_set_display_name(n); }
const char *nes_lobby_display_name(void) { return rnet_lobby_display_name(); }
const char *nes_lobby_player_id(void) { return rnet_lobby_player_id(); }
const char *nes_lobby_host_player_id(void) { return rnet_lobby_host_player_id(); }
void nes_lobby_pump(void) { rnet_lobby_pump(); }
void nes_lobby_set_game_identity(const char *g, const char *v)
{
    nes_lobby_init();
    rnet_lobby_set_game_identity(g, v);
}
const char *nes_lobby_game_version(void) { nes_lobby_init(); return rnet_lobby_game_version(); }
void nes_lobby_request_list(void) { rnet_lobby_request_list(); }
int  nes_lobby_list_count(void) { return rnet_lobby_list_count(); }
int  nes_lobby_list_get(int i, NesLobbyRow *out)
{
    RNetLobbyRow r;
    if (!out || !rnet_lobby_list_get(i, &r)) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->lobby_id, sizeof(out->lobby_id), "%s", r.lobby_id);
    snprintf(out->name, sizeof(out->name), "%s", r.name);
    snprintf(out->game_name, sizeof(out->game_name), "%s", r.game_name);
    snprintf(out->game_version, sizeof(out->game_version), "%s", r.game_version);
    out->player_count = r.player_count;
    out->max_slots = r.max_slots;
    out->has_password = r.has_password;
    return 1;
}

/* NES caps -> shared caps. Delay-sync only (rollback off); the host relay
 * preference is the lobby client's (default on, falls back to the server). */
static void caps_to_shared(const NesLobbyMatchCaps *c, RNetLobbyMatchCaps *o)
{
    NesCapsExt e;
    memset(o, 0, sizeof(*o));
    memset(&e, 0, sizeof(e));
    o->valid = 1;
    o->input_delay = c->input_delay < 2 ? 2 : c->input_delay > 16 ? 16 : c->input_delay;
    o->relay_host = rnet_lobby_relay_host_pref();
    /* The host's own copy of the caps drives its waiting-room agents; the wire
     * form is derived from the same pref + build, so say it here too. */
    o->relay_via_ice = o->relay_host && rnet_lobby_relay_via_ice() && rnet_host_ice_available();
    o->rollback = 0;
    e.widescreen = (unsigned char)(c->widescreen != 0);
    e.widescreen_hud = (unsigned char)(c->widescreen_hud != 0);
    e.ignore_aspect = (unsigned char)(c->ignore_aspect != 0);
    e.ws_extra = c->ws_extra;
    memcpy(o->ext.bytes, &e, sizeof(e));
}
static void caps_from_shared(const RNetLobbyMatchCaps *r, NesLobbyMatchCaps *o)
{
    NesCapsExt e;
    memset(o, 0, sizeof(*o));
    if (!r || !r->valid) return;
    memcpy(&e, r->ext.bytes, sizeof(e));
    o->valid = 1;
    o->widescreen = e.widescreen;
    o->widescreen_hud = e.widescreen_hud;
    o->ignore_aspect = e.ignore_aspect;
    o->ws_extra = e.ws_extra;
    o->input_delay = r->input_delay;
}

int nes_lobby_create(const char *name, const char *game_name, const char *game_version,
                     const char *password, const char *host_bind,
                     const NesLobbyMatchCaps *caps)
{
    RNetLobbyMatchCaps rc;
    nes_lobby_init();
    if (caps && caps->valid) caps_to_shared(caps, &rc);
    return rnet_lobby_create(name, game_name, game_version, password, host_bind,
                             caps && caps->valid ? &rc : NULL, nes_net_seat_cap());
}
int nes_lobby_join(const char *id, const char *pw, const char *bind)
{
    nes_lobby_init();
    return rnet_lobby_join(id, pw, bind);
}
int nes_lobby_leave(void) { return rnet_lobby_leave(); }
int nes_lobby_in_lobby(void) { return rnet_lobby_in_lobby(); }
int nes_lobby_is_host(void) { return rnet_lobby_is_host(); }

static void join_from_shared(const RNetLobbyJoinInfo *j, NesLobbyJoinInfo *o)
{
    memset(o, 0, sizeof(*o));
    o->ok = j->ok;
    snprintf(o->lobby_id, sizeof(o->lobby_id), "%s", j->lobby_id);
    o->session_id = j->session_id;
    o->local_slot = j->local_slot;
    snprintf(o->host_endpoint, sizeof(o->host_endpoint), "%s", j->host_endpoint);
    snprintf(o->guest_endpoint, sizeof(o->guest_endpoint), "%s", j->guest_endpoint);
    snprintf(o->bind_hostport, sizeof(o->bind_hostport), "%s", j->bind_hostport);
    snprintf(o->peer_hostport, sizeof(o->peer_hostport), "%s", j->peer_hostport);
    o->player_count = j->player_count;
    o->max_slots = j->max_slots;
    o->transport_host = j->transport_host;
    o->transport_ice_hub = j->transport_ice_hub;
    o->force_input_relay = j->force_input_relay;
    o->host_spectates = j->host_spectates;
    o->local_is_spectator = j->local_is_spectator;
    snprintf(o->last_error, sizeof(o->last_error), "%s", j->last_error);
}
const NesLobbyJoinInfo *nes_lobby_join_info(void)
{
    const RNetLobbyJoinInfo *j = rnet_lobby_join_info();
    if (j) join_from_shared(j, &s_join); else memset(&s_join, 0, sizeof(s_join));
    return &s_join;
}
int nes_lobby_try_fill_launch(NesLobbyJoinInfo *out)
{
    RNetLobbyJoinInfo j;
    if (!out || !rnet_lobby_try_fill_launch(&j)) return 0;
    join_from_shared(&j, out);
    return 1;
}
const NesLobbyMatchCaps *nes_lobby_match_caps(void)
{
    caps_from_shared(rnet_lobby_match_caps(), &s_caps);
    return &s_caps;
}
int nes_lobby_set_match_caps(const NesLobbyMatchCaps *c)
{
    RNetLobbyMatchCaps rc;
    if (!c || !c->valid) return -1;
    caps_to_shared(c, &rc);
    return rnet_lobby_set_match_caps(&rc);
}
int nes_lobby_member_count(void) { return rnet_lobby_member_count(); }
int nes_lobby_member_get(int i, NesLobbyMember *out)
{
    RNetLobbyMember m;
    if (!out || !rnet_lobby_member_get(i, &m)) return 0;
    memset(out, 0, sizeof(*out));
    out->slot = m.slot;
    snprintf(out->player_id, sizeof(out->player_id), "%s", m.player_id);
    snprintf(out->display_name, sizeof(out->display_name), "%s", m.display_name);
    out->ready = m.ready;
    return 1;
}
int nes_lobby_local_ready(void) { return rnet_lobby_local_ready(); }
int nes_lobby_all_ready(void) { return rnet_lobby_all_ready(); }
int nes_lobby_set_ready(int r) { return rnet_lobby_set_ready(r); }
int nes_lobby_request_start(const NesLobbyMatchCaps *c)
{
    RNetLobbyMatchCaps rc;
    if (c && c->valid) caps_to_shared(c, &rc);
    return rnet_lobby_request_start(c && c->valid ? &rc : NULL);
}
int  nes_lobby_launch_pending(void) { return rnet_lobby_launch_pending(); }
void nes_lobby_clear_launch_pending(void) { rnet_lobby_clear_launch_pending(); }
int  nes_lobby_send_signal(int type, int flag, const char *text)
{
    return rnet_lobby_send_signal(type, flag, text);
}
int  nes_lobby_poll_signal(int *type, int *flag, char *text, size_t cap)
{
    return rnet_lobby_poll_signal(type, flag, text, cap);
}

#endif /* NES_HAS_LOBBY_CLIENT */
