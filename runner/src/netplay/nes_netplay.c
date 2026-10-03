#include "nes_netplay.h"
#include "nes_net_seats.h"
#include "nes_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_net/recomp_net.h"
#if defined(NES_HAS_LOBBY_CLIENT)
#include "nes_lobby_client.h"
#include "recomp_net/lobby_client.h"
#include "recomp_net/host_ice.h"
#endif

typedef struct {
    RNetSession *session;
    uint8_t staged_buttons, published[NES_NETPLAY_MAX_SLOTS];
    uint32_t staged_digest, latched_tick, desync_tick, mm_a, mm_b;
    int staged_valid, active, local_slot, input_player, needs_advance;
    int latched, use_ice, state_desync, slot_count;
    int port_of_slot[NES_NETPLAY_MAX_SLOTS];
} NesNetplayState;
static NesNetplayState g_np;
static NesNetplayConfig g_pending;
static int g_pending_valid;
static char g_err[96];

static void set_err(const char *e) { snprintf(g_err, sizeof(g_err), "%s", e ? e : ""); if (e && *e) fprintf(stderr, "nes_netplay: %s\n", e); }
const char *nes_netplay_last_error(void) { return g_err; }
int nes_netplay_max_seats(void) { return nes_net_seat_cap(); }

void nes_netplay_set_pending_config(const NesNetplayConfig *c) {
    if (!c) { g_pending_valid=0; memset(&g_pending,0,sizeof(g_pending)); return; }
    g_pending=*c; g_pending_valid=1;
}
int nes_netplay_take_pending_config(NesNetplayConfig *c) {
    if (!c||!g_pending_valid)return 0;*c=g_pending;g_pending_valid=0;return 1;
}

void nes_netplay_config_defaults(NesNetplayConfig *c) {
    if (!c) return; memset(c,0,sizeof(*c)); c->input_delay=2; c->session_id=1; c->slot_count=2;
    strcpy(c->bind_hostport,"0.0.0.0:7777");
}
static unsigned env_u(const char *n,unsigned d) {
    const char *v=getenv(n); return v&&*v?(unsigned)strtoul(v,0,10):d;
}
void nes_netplay_apply_env(NesNetplayConfig *c) {
    const char *v; if(!c)return;
    v=getenv("NES_NETPLAY"); if(v&&*v)c->enabled=atoi(v)!=0;
    c->local_slot=(int)env_u("NES_NET_SLOT",(unsigned)c->local_slot);
    c->slot_count=(int)env_u("NES_NET_SLOTS",(unsigned)(c->slot_count>0?c->slot_count:2));
    c->input_player=(int)env_u("NES_NET_INPUT_PLAYER",(unsigned)c->input_player);
    c->input_delay=(int)env_u("NES_NET_INPUT_DELAY",(unsigned)c->input_delay);
    c->session_id=env_u("NES_NET_SESSION",c->session_id);
    v=getenv("NES_NET_BIND"); if(v&&*v)snprintf(c->bind_hostport,sizeof(c->bind_hostport),"%s",v);
    v=getenv("NES_NET_PEER"); if(v&&*v)snprintf(c->peer_hostport,sizeof(c->peer_hostport),"%s",v);
    v=getenv("NES_NET_TRANSPORT");
    if(v&&(!strcmp(v,"ice")||!strcmp(v,"ICE")))c->transport=NES_NET_TRANSPORT_ICE;
    else if(v&&(!strcmp(v,"lan")||!strcmp(v,"LAN")))c->transport=NES_NET_TRANSPORT_LAN;
}

/* ---- session-slot plan ---------------------------------------------------- */
int nes_netplay_plan_seats(const int *seats,int n,int host_seat,int my_seat,int mode,NesSeatPlan *o) {
    int cap=nes_net_seat_cap(),i,j,s,host_ok=0,my_ok=0,sorted[NES_NETPLAY_MAX_SLOTS];
    if(!seats||!o){set_err("plan: bad arguments");return -1;}
    memset(o,0,sizeof(*o));
    for(i=0;i<NES_NETPLAY_MAX_SLOTS;i++){o->port_of_slot[i]=-1;o->seat_of_slot[i]=-1;}
    if(n<2){set_err("plan: a match needs at least two seats");return -1;}
    if(n>cap){set_err("seat_limit: more players than this build can drive");return -1;}
    for(i=0;i<n;i++){
        s=seats[i];
        if(s<0||s>=cap){set_err("seat_limit: a seat is beyond this build's controller ports");return -1;}
        for(j=0;j<i;j++)if(seats[j]==s){set_err("plan: duplicate seat");return -1;}
        sorted[i]=s;
        if(s==host_seat)host_ok=1;
        if(s==my_seat)my_ok=1;
    }
    if(!my_ok){set_err("plan: this peer is not in the seat table");return -1;}
    if(mode==NES_SEAT_PLAN_HOST_FIRST&&!host_ok){set_err("plan: the host is not in the seat table");return -1;}
    for(i=1;i<n;i++){s=sorted[i];for(j=i-1;j>=0&&sorted[j]>s;j--)sorted[j+1]=sorted[j];sorted[j+1]=s;}
    if(mode==NES_SEAT_PLAN_IDENTITY){
        for(i=0;i<n;i++){o->seat_of_slot[sorted[i]]=sorted[i];o->port_of_slot[sorted[i]]=sorted[i];
                         o->occupied_mask|=1u<<sorted[i];}
        o->slot_count=sorted[n-1]+1;o->local_slot=my_seat;
    }else{
        int k=0;
        if(mode==NES_SEAT_PLAN_HOST_FIRST){o->seat_of_slot[0]=host_seat;k=1;}
        for(i=0;i<n;i++){
            if(mode==NES_SEAT_PLAN_HOST_FIRST&&sorted[i]==host_seat)continue;
            o->seat_of_slot[k++]=sorted[i];
        }
        for(i=0;i<n;i++){
            o->port_of_slot[i]=o->seat_of_slot[i];o->occupied_mask|=1u<<i;
            if(o->seat_of_slot[i]==my_seat)o->local_slot=i;
        }
        o->slot_count=n;
    }
    return 0;
}

/* ---- ICE handover stash ----------------------------------------------------
 * Agents the lobby's waiting room connected, owned here from capture until the
 * session adopts them (or they are discarded). Deliberately outside g_np:
 * nes_netplay_start() resets g_np and must not drop them. */
typedef struct {
    int n;                                  /* host: seats; guest: 1 */
    int guest;                              /* 1 = guest agent */
    int slot[NES_NETPLAY_MAX_SLOTS];
    void *agent[NES_NETPLAY_MAX_SLOTS];
} IceStash;
static IceStash g_stash;
static NesNetplayConfig g_captured;
static int g_captured_valid;

static void destroy_agent(void *a) {
#if defined(RNET_ENABLE_ICE)
    if(a)rnet_host_ice_destroy_agent((RNetIceAgent*)a);
#else
    (void)a;
#endif
}
void nes_netplay_discard_handover(void) {
    int i;
    for(i=0;i<g_stash.n;i++)destroy_agent(g_stash.agent[i]);
    memset(&g_stash,0,sizeof(g_stash));
    memset(&g_captured,0,sizeof(g_captured));g_captured_valid=0;
}
int nes_netplay_stash_ice_hub_agents(const int *slots,void *const *agents,int n) {
    int i,j;
#if defined(RNET_ENABLE_ICE)
    if(!slots||!agents||n<1||n>NES_NETPLAY_MAX_SLOTS-1)return -1;
    for(i=0;i<n;i++){
        if(!agents[i]||slots[i]<1||slots[i]>=NES_NETPLAY_MAX_SLOTS)return -1;
        for(j=0;j<i;j++)if(slots[j]==slots[i]||agents[j]==agents[i])return -1;
    }
    for(i=0;i<g_stash.n;i++)destroy_agent(g_stash.agent[i]);
    memset(&g_stash,0,sizeof(g_stash));
    for(i=0;i<n;i++){g_stash.slot[i]=slots[i];g_stash.agent[i]=agents[i];}
    g_stash.n=n;return 0;
#else
    (void)slots;(void)agents;(void)n;(void)i;(void)j;return -1;
#endif
}
int nes_netplay_stash_ice_guest_agent(void *agent) {
    int i;
#if defined(RNET_ENABLE_ICE)
    if(!agent)return -1;
    for(i=0;i<g_stash.n;i++)destroy_agent(g_stash.agent[i]);
    memset(&g_stash,0,sizeof(g_stash));
    g_stash.n=1;g_stash.guest=1;g_stash.agent[0]=agent;return 0;
#else
    (void)agent;(void)i;return -1;
#endif
}

/* ---- lobby launch ----------------------------------------------------------- */
#if defined(NES_HAS_LOBBY_CLIENT)
static int lobby_plan(NesLobbyJoinInfo *j,NesSeatPlan *plan,int *mode_out) {
    int seats[NES_NETPLAY_MAX_SLOTS],n=0,host_seat=-1,my_seat=-1,i,mc,mode;
    const char *host_id=nes_lobby_host_player_id(),*self_id=nes_lobby_player_id();
    if(!nes_lobby_try_fill_launch(j)){set_err("no usable launch");return -1;}
    if(j->host_spectates||j->local_is_spectator){set_err("spectators are not supported by NESRecomp netplay");return -1;}
    mc=nes_lobby_member_count();
    for(i=0;i<mc&&n<NES_NETPLAY_MAX_SLOTS;i++){
        NesLobbyMember m;
        if(!nes_lobby_member_get(i,&m)||m.slot<0||m.slot>=NES_NETPLAY_MAX_SLOTS)continue;
        seats[n++]=m.slot;
        if(host_id&&*host_id&&!strcmp(m.player_id,host_id))host_seat=m.slot;
        if(self_id&&*self_id&&!strcmp(m.player_id,self_id))my_seat=m.slot;
    }
    if(my_seat<0)my_seat=j->local_slot;
    mode=(j->transport_ice_hub||j->transport_host)?NES_SEAT_PLAN_HOST_FIRST
        :j->force_input_relay?NES_SEAT_PLAN_IDENTITY:NES_SEAT_PLAN_DENSE;
    if(mode==NES_SEAT_PLAN_DENSE&&n!=2){set_err("peer-to-peer ICE carries exactly two seats; the room needs a host relay or the server relay");return -1;}
    if(mode_out)*mode_out=mode;
    return nes_netplay_plan_seats(seats,n,host_seat,my_seat,mode,plan);
}
#endif

int nes_netplay_validate_lobby_launch(void) {
#if defined(NES_HAS_LOBBY_CLIENT)
    NesLobbyJoinInfo j;NesSeatPlan p;
    return lobby_plan(&j,&p,0)==0;
#else
    return 0;
#endif
}

int nes_netplay_capture_lobby_launch(void) {
#if defined(NES_HAS_LOBBY_CLIENT)
    NesLobbyJoinInfo j;NesSeatPlan p;NesNetplayConfig c;const NesLobbyMatchCaps *mc;int mode,i;
    nes_netplay_discard_handover();
    if(lobby_plan(&j,&p,&mode)!=0)return 0;
    nes_netplay_config_defaults(&c);
    c.enabled=1;c.session_id=j.session_id;c.local_slot=p.local_slot;c.slot_count=p.slot_count;
    c.occupied_mask=p.occupied_mask;c.is_host=nes_lobby_is_host();c.ports_valid=1;
    for(i=0;i<NES_NETPLAY_MAX_SLOTS;i++)c.port_of_slot[i]=p.port_of_slot[i];
    mc=nes_lobby_match_caps();c.input_delay=mc&&mc->valid?mc->input_delay:2;
    snprintf(c.bind_hostport,sizeof(c.bind_hostport),"%s",j.bind_hostport);
    snprintf(c.peer_hostport,sizeof(c.peer_hostport),"%s",j.peer_hostport);
    if(j.transport_ice_hub){
        c.transport=NES_NET_TRANSPORT_ICE_HUB;
#if defined(RNET_ENABLE_ICE)
        if(c.is_host){
            RNetLobbyIceSeat seat[NES_NETPLAY_MAX_SLOTS];int n,k,slots[NES_NETPLAY_MAX_SLOTS];void *ag[NES_NETPLAY_MAX_SLOTS];
            n=rnet_lobby_ice_take_hub(seat,NES_NETPLAY_MAX_SLOTS);
            if(n<1){set_err(rnet_lobby_ice_launch_error()[0]?rnet_lobby_ice_launch_error():"ice_hub: no connected agents to take");return 0;}
            for(i=0;i<n;i++){
                slots[i]=-1;ag[i]=seat[i].agent;
                for(k=1;k<p.slot_count;k++)if(p.seat_of_slot[k]==seat[i].lobby_slot)slots[i]=k;
            }
            for(i=0;i<n;i++)if(slots[i]<0){
                set_err("ice_hub: a connected agent is not in the seat table");
                for(k=0;k<n;k++)destroy_agent(ag[k]);return 0;
            }
            if(n!=p.slot_count-1||nes_netplay_stash_ice_hub_agents(slots,ag,n)!=0){
                set_err("ice_hub: agents do not cover the seated guests");
                for(k=0;k<n;k++)destroy_agent(ag[k]);return 0;
            }
        }else{
            RNetIceAgent *a=rnet_lobby_ice_take_guest_agent();
            if(!a||nes_netplay_stash_ice_guest_agent(a)!=0){
                set_err(rnet_lobby_ice_launch_error()[0]?rnet_lobby_ice_launch_error():"ice_hub: no connected agent to the host");
                if(a)destroy_agent(a);return 0;
            }
        }
#else
        set_err("ice_hub: this build has no ICE support");return 0;
#endif
    }else if(j.transport_host)c.transport=NES_NET_TRANSPORT_HOST_PORT;
    else if(j.force_input_relay)c.transport=NES_NET_TRANSPORT_LAN;
    else c.transport=NES_NET_TRANSPORT_ICE;
    g_captured=c;g_captured_valid=1;g_err[0]=0;
    return 1;
#else
    return 0;
#endif
}
int nes_netplay_take_captured(NesNetplayConfig *c) {
    if(!c||!g_captured_valid)return 0;*c=g_captured;g_captured_valid=0;return 1;
}

/* ---- session callbacks -------------------------------------------------------- */
static void encode(uint8_t b,uint32_t d,RNetInputSample *o,rnet_u32 t) {
    memset(o,0,sizeof(*o)); o->tick=t; o->size=NES_NETPLAY_PAD_BYTES; o->valid=1;
    o->bytes[0]=b; o->bytes[1]=(rnet_u8)d; o->bytes[2]=(rnet_u8)(d>>8);
    o->bytes[3]=(rnet_u8)(d>>16); o->bytes[4]=(rnet_u8)(d>>24);
}
static uint32_t digest(const RNetInputSample *i) {
    if(!i||!i->valid||i->size<5)return 0;
    return (uint32_t)i->bytes[1]|((uint32_t)i->bytes[2]<<8)|
           ((uint32_t)i->bytes[3]<<16)|((uint32_t)i->bytes[4]<<24);
}
static void sample(rnet_u32 t,RNetInputSample *o,void *ctx) {
    NesNetplayState *s=(NesNetplayState*)ctx;
    encode(s->staged_valid?s->staged_buttons:0,
           s->staged_valid?s->staged_digest:nes_runtime_state_digest(),o,t);
}
static void publish(rnet_u32 t,const RNetInputSample *p,int n,void *ctx) {
    NesNetplayState *s=(NesNetplayState*)ctx; int i,first=-1;
    uint32_t d,d0=0;
    memset(s->published,0,sizeof(s->published));
    if(!p||n<2)return;
    if(n>s->slot_count)n=s->slot_count;
    for(i=0;i<n&&i<NES_NETPLAY_MAX_SLOTS;i++){
        int port=s->port_of_slot[i];
        if(port<0||port>=NES_NETPLAY_MAX_SLOTS)continue;
        if(p[i].valid&&p[i].size)s->published[port]=p[i].bytes[0];
        if(p[i].valid&&p[i].size>=5){
            d=digest(&p[i]);
            if(first<0){first=i;d0=d;}
            else if(d!=d0&&!s->state_desync){
                /* Every occupied seat must carry the same state digest. */
                s->state_desync=1;s->desync_tick=t;s->mm_a=d0;s->mm_b=d;
            }
        }
    }
}
#if defined(NES_HAS_LOBBY_CLIENT) && defined(RNET_ENABLE_ICE)
static void on_signal(const RNetSignal *m,void *ctx) {
    (void)ctx;if(m)(void)nes_lobby_send_signal((int)m->type,(int)m->flag,m->text);
}
static void drain_signals(void) {
    int type,flag;char text[2048];
    /* Peer-to-peer ICE only. A hub / adopted session never takes signals: its
     * agents are frozen at COMPLETED and the lobby client owns type 130+. */
    if(g_np.use_ice!=1)return;
    while(g_np.session&&nes_lobby_poll_signal(&type,&flag,text,sizeof(text))){
        RNetSignal s;memset(&s,0,sizeof(s));
        if(type==(int)RNET_SIGNAL_LOCAL_SDP)type=(int)RNET_SIGNAL_REMOTE_SDP;
        else if(type==(int)RNET_SIGNAL_LOCAL_CANDIDATE)type=(int)RNET_SIGNAL_REMOTE_CANDIDATE;
        s.type=(RNetSignalType)type;s.flag=(rnet_u8)flag;s.peer_slot=0xFF; /* 1:1, unaddressed */
        snprintf(s.text,sizeof(s.text),"%s",text);
        rnet_session_push_signal(g_np.session,&s);
    }
}
#else
static void drain_signals(void){}
#endif
static int private_peer(const char *hp) {
    char h[64];const char *c;size_t n;unsigned a=0,b=0;
    if(!hp||!*hp)return 1;c=strrchr(hp,':');n=c?(size_t)(c-hp):strlen(hp);
    if(n>=sizeof(h))n=sizeof(h)-1;memcpy(h,hp,n);h[n]=0;
    if(!strcmp(h,"localhost"))return 1;if(sscanf(h,"%u.%u",&a,&b)<1)return 0;
    return a==127||a==10||(a==192&&b==168)||(a==172&&b>=16&&b<=31);
}
/* Resolve AUTO to a concrete transport (env / headless bring-up). */
static int resolve_transport(const NesNetplayConfig *c) {
    if(c->transport!=NES_NET_TRANSPORT_AUTO)return c->transport;
#if defined(RNET_ENABLE_ICE) && defined(NES_HAS_LOBBY_CLIENT)
    if(nes_lobby_connected()&&nes_lobby_in_lobby()&&!private_peer(c->peer_hostport)&&
       (c->slot_count<=2))return NES_NET_TRANSPORT_ICE;
#else
    (void)private_peer;
#endif
    return NES_NET_TRANSPORT_LAN;
}
static void fail_start(void) {
    if(g_np.session){rnet_session_destroy(g_np.session);g_np.session=0;}
}
int nes_netplay_start(const NesNetplayConfig *c) {
    RNetConfig rc;RNetHostVTable h;int tr,n,i,cap=nes_net_seat_cap();
    if(!c||!c->enabled)return -1;nes_netplay_shutdown();rnet_config_init_defaults(&rc);
    n=c->slot_count>0?c->slot_count:2;
    /* The enforced seat ceiling: refuse rather than run fewer or invent more. */
    if(n<2||n>cap||n>NES_NETPLAY_MAX_SLOTS){
        char m[96];snprintf(m,sizeof(m),"seat_limit: %d seats requested, this build drives 2..%d",n,cap);
        set_err(m);nes_netplay_discard_handover();return -5;
    }
    if(c->local_slot<0||c->local_slot>=n){set_err("local slot is outside the seat count");nes_netplay_discard_handover();return -5;}
    for(i=0;i<NES_NETPLAY_MAX_SLOTS;i++)g_np.port_of_slot[i]=-1;
    for(i=0;i<n;i++){
        int port=c->ports_valid?c->port_of_slot[i]:i;
        if(c->occupied_mask&&!(c->occupied_mask&(1u<<i))){g_np.port_of_slot[i]=-1;continue;}
        if(port<0||port>=cap){
            set_err("seat_limit: a seat has no controller port in this build");nes_netplay_discard_handover();return -5;
        }
        g_np.port_of_slot[i]=port;
    }
    tr=resolve_transport(c);
    if(tr==NES_NET_TRANSPORT_ICE&&n!=2){set_err("peer-to-peer ICE carries exactly two seats");nes_netplay_discard_handover();return -5;}
    rc.slot_count=(rnet_u8)n;rc.local_slot=(rnet_u8)c->local_slot;
    rc.occupied_mask=c->occupied_mask;
    rc.input_delay=(rnet_u8)(c->input_delay<0?0:c->input_delay>16?16:c->input_delay);
    rc.session_id=c->session_id?c->session_id:1;rc.protocol_magic=0x4e45534eu;
    memset(&h,0,sizeof(h));h.sample_local=sample;h.publish=publish;h.ctx=&g_np;
    g_np.slot_count=n;
#if defined(NES_HAS_LOBBY_CLIENT) && defined(RNET_ENABLE_ICE)
    if(tr==NES_NET_TRANSPORT_ICE)h.on_signal=on_signal;
#endif
    g_np.session=rnet_session_create(&rc,&h);if(!g_np.session)return -2;
    if(tr==NES_NET_TRANSPORT_ICE_HUB){
#if defined(RNET_ENABLE_ICE)
        int rcode=-1;
        if(!c->is_host&&c->local_slot!=0&&g_stash.guest&&g_stash.n==1)
            rcode=rnet_session_adopt_ice_agent(g_np.session,(RNetIceAgent*)g_stash.agent[0]);
        else if(c->is_host&&c->local_slot==0&&!g_stash.guest&&g_stash.n==n-1){
            RNetIceAdoptSeat seats[NES_NETPLAY_MAX_SLOTS];
            for(i=0;i<g_stash.n;i++){seats[i].slot=g_stash.slot[i];seats[i].agent=(RNetIceAgent*)g_stash.agent[i];}
            rcode=rnet_session_start_ice_hub_adopt(g_np.session,seats,g_stash.n);
        }
        if(rcode!=0){
            if(!g_err[0])set_err("ice_hub: no connected agents were captured for this launch");
            fail_start();nes_netplay_discard_handover();return -3;
        }
        /* Adopted: the session owns the agents now. */
        memset(&g_stash,0,sizeof(g_stash));
#else
        set_err("ice_hub: this build has no ICE support");fail_start();return -3;
#endif
    }else if(tr==NES_NET_TRANSPORT_ICE){
#if defined(RNET_ENABLE_ICE)
        RNetIceConfig ic;rnet_ice_config_init_defaults(&ic);ic.controlling=rc.local_slot==0;
        if(rnet_session_start_ice(g_np.session,&ic)!=0){fail_start();return -3;}
#else
        set_err("ICE requested but this build has no ICE support");fail_start();return -3;
#endif
    }else{
        /* Direct UDP / the server's relay: every peer binds and dials. The
         * host relay's advertised port (HOST_PORT) and a headless N-seat LAN
         * room: the host with no peer endpoint is the hub (3+) or accepts
         * the first packet (2). */
        int hub=(tr==NES_NET_TRANSPORT_HOST_PORT||tr==NES_NET_TRANSPORT_LAN)&&
                n>2&&c->local_slot==0&&!c->peer_hostport[0];
        int rcode=hub?rnet_session_start_lan_hub(g_np.session,c->bind_hostport)
                     :rnet_session_start_lan(g_np.session,c->bind_hostport,c->peer_hostport);
        if(rcode!=0){fail_start();return -4;}
    }
    g_np.active=1;g_np.local_slot=rc.local_slot;g_np.input_player=c->input_player==1;
    g_np.use_ice=tr==NES_NET_TRANSPORT_ICE?1:tr==NES_NET_TRANSPORT_ICE_HUB?2:0;
    fprintf(stderr,"nes_netplay: %s slot=%d/%d session=%u delay=%u bind=%s peer=%s\n",
            tr==NES_NET_TRANSPORT_ICE_HUB?"ice-hub":tr==NES_NET_TRANSPORT_ICE?"ice":
            tr==NES_NET_TRANSPORT_HOST_PORT?"host-port":"lan",
            g_np.local_slot,n,(unsigned)rc.session_id,(unsigned)rc.input_delay,c->bind_hostport,c->peer_hostport);
    return 0;
}
void nes_netplay_shutdown(void) {
    if(g_np.session){(void)rnet_session_send_bye(g_np.session);rnet_session_destroy(g_np.session);}
    memset(&g_np,0,sizeof(g_np));
}
int nes_netplay_active(void){return g_np.active&&g_np.session!=0;}
int nes_netplay_is_running(void){return nes_netplay_active()&&rnet_session_is_running(g_np.session);}
int nes_netplay_local_slot(void){return nes_netplay_active()?g_np.local_slot:-1;}
int nes_netplay_slot_count(void){return nes_netplay_active()?g_np.slot_count:0;}
int nes_netplay_input_player(void){return nes_netplay_active()?g_np.input_player:0;}
uint32_t nes_netplay_sim_tick(void){return nes_netplay_active()?rnet_session_sim_tick(g_np.session):0;}
void nes_netplay_stage_local(uint8_t b){
    uint32_t t;if(!nes_netplay_active())return;t=nes_netplay_is_running()?rnet_session_sim_tick(g_np.session):0;
    if(g_np.latched&&g_np.latched_tick==t)return;g_np.staged_buttons=b;g_np.staged_digest=nes_runtime_state_digest();
    g_np.staged_valid=1;g_np.latched=1;g_np.latched_tick=t;
}
int nes_netplay_needs_local_sample(void){
    uint32_t t;if(!nes_netplay_active())return 0;t=nes_netplay_is_running()?rnet_session_sim_tick(g_np.session):0;
    return !(g_np.latched&&g_np.latched_tick==t);
}
int nes_netplay_poll_admit(void){
    rnet_u32 t;if(!nes_netplay_active())return 1;
#if defined(NES_HAS_LOBBY_CLIENT)
    if(nes_lobby_connected())nes_lobby_pump();
#endif
    drain_signals();rnet_session_pump(g_np.session);if(!rnet_session_is_running(g_np.session))return 0;
    if(g_np.needs_advance)return !g_np.state_desync;t=rnet_session_sim_tick(g_np.session);
    if(!rnet_session_try_admit(g_np.session,t))return 0;g_np.needs_advance=1;return !g_np.state_desync;
}
void nes_netplay_finish_frame(void){
    if(!nes_netplay_active()||!g_np.needs_advance)return;rnet_session_advance(g_np.session);
    g_np.needs_advance=0;g_np.latched=0;g_np.staged_valid=0;
}
uint8_t nes_netplay_published_buttons(int p){return p>=0&&p<NES_NETPLAY_MAX_SLOTS?g_np.published[p]:0;}
void nes_netplay_wait_recv(int ms){if(nes_netplay_active())(void)rnet_session_wait_recv(g_np.session,ms);}
int nes_netplay_input_desync(uint32_t*t,uint32_t*l,uint32_t*r){return nes_netplay_active()&&rnet_session_input_desync(g_np.session,t,l,r);}
int nes_netplay_state_desync(uint32_t*t,uint32_t*a,uint32_t*b){
    if(!g_np.state_desync)return 0;if(t)*t=g_np.desync_tick;if(a)*a=g_np.mm_a;if(b)*b=g_np.mm_b;return 1;
}
int nes_netplay_peer_disconnected(uint32_t ms){
    if(!nes_netplay_active())return 0;
    if(!ms)ms=1500;
    if(g_np.slot_count>2)return rnet_session_disconnected_peers(g_np.session,ms)!=0;
    return rnet_session_peer_disconnected(g_np.session,ms);
}
