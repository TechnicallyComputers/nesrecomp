#ifndef NES_NET_SEATS_H
#define NES_NET_SEATS_H
/*
 * The enforced netplay seat ceiling for this build.
 *
 * Netplay can carry exactly as many seats as the runtime can DRIVE. The NES
 * controller model here is two hardware streams ($4016/$4017) plus, for titles
 * that opt in with NESRECOMP_INPUT_SEATS=3|4, logical seats 3/4 that the game
 * reads through nes_input_seat() (runner/include/logical_input.h). There is no
 * Four Score serial multiplexer: a title that does not opt in has two seats,
 * and nothing in the netcode may invent a third.
 *
 *   cap = NESRECOMP_INPUT_SEATS            (2 unless the title opts in; <= 4)
 *   cap = min(cap, NESRECOMP_GAME_PLAYERS) when the title declares it
 *
 * This is the single source of the number: the lobby is configured with it
 * (create() clamps), nes_netplay_start() refuses a launch above it, and the
 * published-input fan-out is bounded by it. NES_NET_SEATS_MAX is the array
 * bound only.
 */
#include "logical_input.h"

#define NES_NET_SEATS_MAX 4

static inline int nes_net_seat_cap(void)
{
    int cap = NESRECOMP_INPUT_SEATS;
#ifdef NESRECOMP_GAME_PLAYERS
    if (NESRECOMP_GAME_PLAYERS < cap) cap = NESRECOMP_GAME_PLAYERS;
#endif
    if (cap > NES_NET_SEATS_MAX) cap = NES_NET_SEATS_MAX;
    return cap < 2 ? 2 : cap;
}
#endif
