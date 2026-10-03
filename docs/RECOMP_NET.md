# recomp-net integration

NESRecomp exposes optional delay-synchronised netplay through `recomp-net`
for as many seats as the title's runtime can drive (see "Seat ceiling"). It is deliberately title opt-in: existing games do not build or
link networking code until their CMake target enables it.

## Title integration

After including `runner/runner.cmake` and creating the executable target:

```cmake
set(NESRECOMP_NET_ICE ON CACHE BOOL "" FORCE) # optional WAN/ICE support
target_compile_definitions(MyGame PRIVATE
    NESRECOMP_GAME_PLAYERS=2      # launcher label; also caps netplay seats
    # NESRECOMP_INPUT_SEATS=4     # opt in to logical seats 3/4 (see below)
    NESRECOMP_GAME_VERSION="my-game-net-v1")
nesrecomp_enable_recomp_net(MyGame)
```

The game version is the multiplayer compatibility pin. Change it whenever a
build can no longer remain deterministic with the previous release.

When the target also uses recomp-ui, the launcher automatically gains the
Netplay page and the complete host/join/room flow. The bridge combines the
WebSocket matchmaking service with recomp-net's local lobby registry, exposes
local interfaces and STUN-discovered external IPv4 addresses, and hands LAN or
ICE session details to the runner.

## Seat ceiling (explicit and enforced)

Netplay carries exactly as many seats as the runtime can drive, and no more:

```text
cap = NESRECOMP_INPUT_SEATS            (2 unless the title opts in; 2..4)
cap = min(cap, NESRECOMP_GAME_PLAYERS) when the title declares it
```

(`runner/src/netplay/nes_net_seats.h`, `nes_netplay_max_seats()`.) The NES
controller model here is two hardware streams, `$4016` and `$4017`; a title
that compiles with `NESRECOMP_INPUT_SEATS=3|4` additionally receives logical
seats 3 and 4 through `nes_input_seat()` (`runner/include/logical_input.h`) and
its recompiled code decides what they mean. There is no Four Score serial
multiplexer, so a title that has not opted in has **two seats and nothing in the
netcode will invent a third**.

The ceiling is enforced in three independent places: the lobby is configured
with it (`create` clamps `max_slots`), `nes_netplay_plan_seats` and
`nes_netplay_validate_lobby_launch` refuse a launch whose seats exceed it or
whose lobby seat has no controller port, and `nes_netplay_start` refuses
`slot_count > cap` with `-5` / `seat_limit:` in `nes_netplay_last_error()` and
returns to the lobby. Spectators (and a spectating host) are refused the same
way.

## Runtime contract

The runner admits exactly one simulation tick at each outermost VBlank. It
samples one local controller, waits until recomp-net publishes every seat's
delayed input, and then writes those bytes to the emulated `$4016` / `$4017`
controller streams (and `g_logical_input` for seats 3/4). Nested VBlank
callbacks reuse the published inputs and do not advance the network timeline.

Published inputs are indexed by **controller port = lobby seat**, so the lobby's
P1/P2 labels hold even when the session slot order differs (a hub host is always
session slot 0 whatever seat it holds).

Each input packet also carries a canonical CRC32 of CPU, RAM/SRAM, PPU, mapper,
APU, timing, and controller-shift state; every seat must agree. A mismatch,
input-stream mismatch, any peer disconnect, or Escape ends the match and returns
the launcher to the room. Quick save/load is disabled while a netplay session is
active.

Match settings are host-owned. The NES integration synchronises input delay and
the title's widescreen settings (carried through recomp-net's caps codec). It is
delay sync only: no rollback, no spectators.

## Transports

The lobby is recomp-net's shared `rnet_lobby_*` client behind the thin
`nes_lobby_*` adapter (`runner/src/lobby/nes_lobby_client.c`); there is no
engine-local lobby protocol or WebSocket code. A launch is carried by, in order:

1. **Host relay over ICE** (`transport_ice_hub`, the default when the build has
   ICE): the waiting room connected one libjuice agent per guest; the match runs
   over those same agents, the host fanning out to the guests. Works for every
   seat count up to the ceiling. The launcher bridge captures the agents in
   `clear_launch_pending` (before the lobby launch is cleared): host
   `rnet_lobby_ice_take_hub` + `rnet_session_start_ice_hub_adopt`, guest
   `rnet_lobby_ice_take_guest_agent` + `rnet_session_adopt_ice_agent`. A seat
   that is not connected refuses the launch (`ice_not_connected`).
2. Host relay over the host's advertised UDP port (legacy `transport_host`):
   host accepts (2 seats) or runs the LAN hub (3+), guests dial it.
3. The lobby server's UDP relay (`force_input_relay`): everyone dials it.
4. Peer-to-peer ICE, exactly two seats.
5. LAN / direct UDP (`NES_NET_TRANSPORT=lan`), any seat count.

## Headless LAN bring-up

The launcher is the normal path. For automated two-process testing, both peers
can instead use environment variables:

```text
NES_NETPLAY=1
NES_NET_SLOT=0..N-1
NES_NET_SLOTS=2..cap          # seat count (default 2)
NES_NET_SESSION=4242
NES_NET_BIND=127.0.0.1:17771
NES_NET_PEER=127.0.0.1:17772
NES_NET_TRANSPORT=lan|ice
NES_NET_INPUT_DELAY=2
NES_NET_INPUT_PLAYER=0|1
```

Use inverse bind/peer ports for the second local process. With more than two
seats the host (slot 0, no `NES_NET_PEER`) is the LAN hub and every other seat
dials it. `NES_NET_TRANSPORT=ice` is peer-to-peer and therefore two seats only;
the ICE host relay needs a lobby (see tests below). `NES_NET_TRANSPORT`
defaults to automatic selection: lobby-backed public peers use ICE and private
or direct endpoints use LAN.

## Dependencies

`lib/recomp-net` is pinned as a submodule (the host-relay-over-ICE API,
`rnet_session_start_ice_hub_adopt` / `rnet_lobby_ice_take_*`, is required). ICE
builds fetch libjuice when it is not installed locally (`-DRNET_LIBJUICE_ROOT`
points at a source tree for offline builds). On Windows, the NESRecomp build
supplies a narrow native mutex/error-code compatibility shim for the pinned
recomp-net sources; it has not been re-verified against the newer recomp-net.

## Tests

`tests/netplay` builds through `runner/recomp_net.cmake` exactly as a title
does, once per seat ceiling (2 and 4):

```sh
cmake -S tests/netplay -B build/netplay-tests -DRNET_LIBJUICE_ROOT=<libjuice> \
      [-DNES_E2E_LOBBY_SERVER=<recomp-net-server binary>]
cmake --build build/netplay-tests -j
ctest --test-dir build/netplay-tests --output-on-failure
```

They cover the seat plan and ceiling, N-process LAN over the `NES_NET_*`
environment, N-process ICE hub adoption over real libjuice agents (loopback; the
waiting-room signalling is a socketpair), and, with a server binary, the whole
production path against a real lobby server on loopback. Builds whose ceiling is
below N must refuse N seats; that is asserted, not skipped.
