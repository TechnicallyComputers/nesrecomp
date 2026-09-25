# Rollback netplay in nesrecomp — what is built, what is measured, what is not

> **Added 2026-09-25, branch `feat/rollback-netplay`.** The episode driver is
> recomp-net's (`include/recomp_net/rb_driver.h`); nesrecomp binds it. The
> doctrine is `recomp-ai-rules/NETPLAY.md`. Target title: Super Mario Bros.,
> 2-4 player simultaneous co-op. **What has been run: 2-4 processes on one
> Linux machine over UDP loopback (directly, through recomp-net's LAN hub, and
> through a local recomp-net-server's lobby and input relay), headless (SDL
> dummy video/audio). Nothing here has been played across the internet,
> between two machines, on Windows/macOS, or by a person holding a
> controller.** Screenshots in the evidence were read by the agent; the
> gameplay verdict belongs to Alex.

## 1. The pieces

| where | what |
|---|---|
| `lib/recomp-net` 03ee1b1 | the driver, LAN transport + hub, lobby client, link simulator (RetroPortingToolKit `feat/rb-sparse-seats-and-ws-backlog` = main a9d20e2 + sparse-seat seal fix + WS backlog) |
| `lib/retcomm-rbengine` 2a03e73 | the snapshot ring and the monotonic clock |
| `runner/src/savestate.c` | ONE in-memory serializer; the V7 file is exactly its byte stream |
| `runner/src/rollback/nes_rb_state.c` | the rollback snapshot (V7 image + logical-input trailer) and the per-tick digest over the same bytes |
| `runner/src/rollback/nes_rb_probe.c` | `NES_RB_PROBE`, the determinism probe |
| `runner/src/rollback/nes_session_config.c` | the session configuration seal |
| `runner/src/main_runner.c` | the tick gate (top of the outermost frame callback), row application before the NMI, replay presentation suppression, the per-tick continuation restart |
| `runner/src/netplay/nes_netplay_rb.c` | `RNetRbHost` binding, INCREMENTAL replay |
| `runner/src/netplay/nes_netplay.c` | session: seats, transport, identity, SRAM barrier + sandbox, refusals, soft return |
| `runner/src/netplay/nes_host_lobby.c` | adapter over recomp-ui `recomp_netplay_host` (+ the headless room) |
| `tools/rb_loopback.sh`, `rb_sweep.sh`, `rb_lobby.sh` | the multi-process harness |

Removed: `nes_launcher_netplay.c` (no longer compiled against recomp-ui: its
`cb_create` lacked `max_slots`) and `lobby/nes_lobby_client.c` + a private
WebSocket copy (1.6k lines), both superseded by the shared backend.

## 2. The shape

**The tick.** A NES game's RESET routine never returns: generated code runs it
forever on the host C stack, and `nes_vblank_callback` (the NMI + one frame) is
called nested inside guest code whenever the CPU-cycle budget crosses a frame.
One rollback tick = one OUTERMOST callback (nested spin-wait callbacks are part
of it). The gate runs at the very top of that callback, before the NMI:
`finish_frame` for the tick that just ran, the local pad staged, `poll_admit`
until the driver admits a live or a replayed tick. The published rows are
applied right before the NMI -- after every local input source, so while a
session is active they are the only input any seat sees (NETPLAY.md §2).

**Snapshot and digest over one domain.** A snapshot keyed T is taken at that
point (the state before T): the V7 save-state image (CPU, WRAM, SRAM, CHR,
OAM, palette, nametables, PPU registers/latches, mapper, the runtime timing
blob, the APU blob, controller ports and shift registers, render/zapper
sidecars, frame count, the interrupted guest continuation, every mod record --
SMB's `smb.coop` actors included) plus a trailer with the logical input seats.
The digest is a 64-bit hash over the SAME serialized bytes, partitioned
`cpu_wram` / `ppu` / `apu_io_mods`; the image is cached until guest code runs,
so the snapshot and the digest of a tick cannot disagree.

**Replay model: INCREMENTAL, with the continuation restarted every tick.**
The driver hands out one replayed tick per outer callback. A baseline load
happens at the top of a callback; the rest of that callback replays the load
tick; `finish_frame_callback` then discards the stale native stack
(`longjmp` to `run_guest_execution`) and resumes the guest at the
continuation the snapshot recorded -- `(resume PC, charged)`, run through
`nes_interp_resume`, which hands back to generated code at the first covered
target. Under rollback (netplay or the probe) that restart happens at the end
of EVERY outer callback, not only after a load, so a live tick and a replayed
tick run the identical host path by construction.

Why the restart every tick (measured, not assumed): with restarts only after
a load, the probe diverged on 40 of 40 windows at `runtime_blob` nes_cycles /
ops_count +-1 and the APU accumulators. SMB's idle loop is `JMP $8057`, which
the recompiler charges as 3+2 cycles in `func_RESET`'s body (loop-back goto
with a transfer charge) but 3 then 2 per iteration in `func_8057_b0` (the
idle-spin shape) -- and the interpreter charges 3. A resumed timeline
therefore landed the next VBlank one CPU cycle off the live one. Restarting
every tick makes the continuation a function of guest state only; with it the
probe runs 0 divergences (§5). The codegen inconsistency itself (a JMP is 3
cycles on hardware) is a real timing defect; fixing it changes netplay-off
output for every title and is filed as an open decision (§6), not fixed here.

**Why not a fiber stack snapshot** (the Genesis track's choice): PRINCIPLES.md
"Control Flow Semantics" says host coroutine/fiber stacks are not save-state
data and a load must never depend on resuming an old host C stack. The restart
satisfies the doctrine's other half -- "a well-defined generated-code resume
boundary" -- and is measured exact; the fiber snapshot would be the forbidden
shape.

**The interpreter bridge, honestly** (PRINCIPLES.md "The interpreter may
bridge -- but only honestly"). The resume runs guest code in `interp.c` until
it re-enters generated code. For SMB that is exactly ONE instruction per tick:
every run logs `RB_BRIDGE restarts=N interp_instrs=N` (e.g. 8399 / 8399), 0
declines, 0 watchdog trips. Loud: counted per run and per entry PC (interp
hotspots, TCP `interpreter_stats`). Heals to static: the self-loop handoff
returns to native code on the first transfer. Not a miss bridge: no discovered
coverage is involved, so there is nothing to promote. Strict mode:
`NESRECOMP_INTERP_FALLBACK=off` disables the miss bridge, not the resume; a
build that must never interpret cannot roll back, and says so here rather than
pretending. A title whose main thread does real work when the NMI fires would
run that work interpreted until its next covered JSR -- measure `RB_BRIDGE`
before shipping such a title online.

**Presentation.** A replayed tick runs the same code as a live one (render,
sprite-0 prediction and zapper framebuffer are simulation inputs on some
titles) and skips only the SDL present, the wall-clock pacing and the audio
delivery; the APU output ring is rewound at resim end, so no tick is heard
twice. Turbo is refused online.

## 3. What a session settles

- **Seats**: 2-4, session slot == lobby seat == controller port (SEAT policy).
  Seats 0/1 are `$4016/$4017`, seats 2/3 are `g_logical_input` (SMB co-op
  Wario/Waluigi). More than two seats: seat 0 hubs the LAN session, or every
  seat dials the lobby server's UDP relay. Spectators on the relay.
- **Identity**: `game_version` = `git describe` + first 8 bytes of the exe's
  SHA-256; `content_fingerprint` = ROM SHA-256. Both reach the driver's IDENT.
- **Session configuration** (`nes_session_config.h`): keys a match may vary
  (engine: `widescreen`; SMB: `coop`). The host's offer comes from its
  offline selection, the lobby carries it in match caps (`nes_session`), every
  peer applies it before boot and never saves it, and the driver's mod-set
  handshake confirms every peer runs the identical text. A netplay launch
  commits no mods (`nes_mod_runtime_commit_netplay_c`).
- **SRAM**: host-authoritative, transferred before any guest code runs
  (`nes_netplay_boot_barrier`; RNET_STATE_OP_SRAM); guests persist only to
  `saves/netplay/`.
- **Refused online**: input scripts, recording, `--loadstate`, `NES_RB_PROBE`,
  `--smoke/--benchmark`, quick states, turbo, and the TCP verbs write_ram /
  restore_frame / save_state / load_state / set_input / press / set_turbo /
  pause / continue / step / run_to_frame (registered, answering with the
  alternative).
- **Leaving**: Escape, a peer gone, a refusal or a drained match returns to the
  waiting room with `last_error`; a rematch is a cold boot in the same process.

## 4. Harness

`tools/rb_loopback.sh` (2-4 seats), `tools/rb_sweep.sh` (pre-flight + 19
cells), `tools/rb_lobby.sh` (online through a local recomp-net-server, or
LAN; rounds = rematches; spectators; forced boot fork; offline Play after
rematch vs a fresh process). Every peer runs from its own copy of the
executable. See each script's header.

## 5. Capability matrix

MATRIX_PLACEHOLDER

## 6. Open defects and decisions

OPEN_PLACEHOLDER
