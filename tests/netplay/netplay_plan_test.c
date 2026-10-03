/* Pure tests: seat plan + the enforced seat ceiling. */
#include "nes_netplay.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, what) do { if (c) printf("PASS: %s\n", what); else { printf("FAIL: %s\n", what); fails++; } } while (0)

int main(void)
{
    NesSeatPlan p;
    const int cap = nes_netplay_max_seats();
    int two[] = {0, 1}, three[] = {0, 1, 2}, hostmid[] = {0, 1, 3}, sparse[] = {1, 3}, five[] = {0, 1, 2, 3, 4};
    int swapped[] = {1, 0};

    CHECK(cap == NES_SEAT_CAP_EXPECT, "ceiling equals NESRECOMP_INPUT_SEATS for this build");
    CHECK(nes_netplay_plan_seats(two, 2, 0, 1, NES_SEAT_PLAN_HOST_FIRST, &p) == 0 &&
          p.slot_count == 2 && p.local_slot == 1 && p.port_of_slot[0] == 0 && p.port_of_slot[1] == 1,
          "2 seats, host first: slot == seat == port");
    /* Host holds lobby seat 1: it is still session slot 0, driving port 1. */
    CHECK(nes_netplay_plan_seats(swapped, 2, 1, 1, NES_SEAT_PLAN_HOST_FIRST, &p) == 0 &&
          p.local_slot == 0 && p.port_of_slot[0] == 1 && p.port_of_slot[1] == 0,
          "host on seat 1 becomes slot 0 and keeps controller port 1");
    CHECK(nes_netplay_plan_seats(swapped, 2, 1, 0, NES_SEAT_PLAN_HOST_FIRST, &p) == 0 &&
          p.local_slot == 1 && p.port_of_slot[1] == 0, "guest on seat 0 is slot 1, port 0");
    CHECK(nes_netplay_plan_seats(two, 2, 0, 1, NES_SEAT_PLAN_DENSE, &p) == 0 && p.occupied_mask == 3u,
          "dense plan, 2 seats");
    if (cap >= 3) {
        CHECK(nes_netplay_plan_seats(three, 3, 0, 2, NES_SEAT_PLAN_HOST_FIRST, &p) == 0 &&
              p.slot_count == 3 && p.local_slot == 2 && p.port_of_slot[2] == 2, "3 seats accepted under a >=3 ceiling");
    } else {
        CHECK(nes_netplay_plan_seats(three, 3, 0, 2, NES_SEAT_PLAN_HOST_FIRST, &p) == -1 &&
              strstr(nes_netplay_last_error(), "seat_limit"), "3 seats REFUSED under the 2-seat ceiling");
    }
    if (cap >= 4) {
        CHECK(nes_netplay_plan_seats(hostmid, 3, 0, 3, NES_SEAT_PLAN_HOST_FIRST, &p) == 0 &&
              p.slot_count == 3 && p.local_slot == 2 && p.port_of_slot[2] == 3,
              "sparse seats {0,1,3}: dense session slots, port = lobby seat");
        CHECK(nes_netplay_plan_seats(sparse, 2, 1, 3, NES_SEAT_PLAN_IDENTITY, &p) == 0 &&
              p.slot_count == 4 && p.occupied_mask == 0xAu && p.local_slot == 3,
              "relay identity plan: slot == seat, sparse occupied mask");
    } else {
        CHECK(nes_netplay_plan_seats(sparse, 2, 1, 3, NES_SEAT_PLAN_IDENTITY, &p) == -1 &&
              strstr(nes_netplay_last_error(), "seat_limit"), "a seat at/above the ceiling is refused");
    }
    CHECK(nes_netplay_plan_seats(five, 5, 0, 0, NES_SEAT_PLAN_HOST_FIRST, &p) == -1, "5 seats refused in every build");
    CHECK(nes_netplay_plan_seats(two, 1, 0, 0, NES_SEAT_PLAN_HOST_FIRST, &p) == -1, "one seat is not a match");
    CHECK(nes_netplay_plan_seats(two, 2, 0, 7, NES_SEAT_PLAN_HOST_FIRST, &p) == -1, "peer not in the seat table refused");

    /* nes_netplay_start enforces the same ceiling independently of the plan. */
    {
        NesNetplayConfig c;
        nes_netplay_config_defaults(&c);
        c.enabled = 1; c.slot_count = cap + 1; c.transport = NES_NET_TRANSPORT_LAN;
        CHECK(nes_netplay_start(&c) == -5 && !nes_netplay_active() &&
              strstr(nes_netplay_last_error(), "seat_limit"), "nes_netplay_start refuses cap+1 seats");
        c.slot_count = 2; c.transport = NES_NET_TRANSPORT_ICE_HUB; c.is_host = 1; c.local_slot = 0;
        CHECK(nes_netplay_start(&c) == -3 && !nes_netplay_active(),
              "ICE hub start without captured agents fails loudly, never falls back");
    }
    return fails ? 1 : 0;
}
