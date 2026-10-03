cmake_minimum_required(VERSION 3.16)
project(NESRecompNetplayTests C)
set(CMAKE_C_STANDARD 11)

# Standalone native tests for runner/src/netplay + the nes_lobby_* adapter,
# built through the SAME runner/recomp_net.cmake a title uses
# (-DNESRECOMP_ENABLE_NET=ON -DNESRECOMP_NET_ICE=ON). Offline libjuice:
#   -DRNET_LIBJUICE_ROOT=<libjuice source tree>   (else FetchContent)
#
#   cmake -S tests/netplay -B build/netplay-tests -DRNET_LIBJUICE_ROOT=...
#   cmake --build build/netplay-tests -j
#   ctest --test-dir build/netplay-tests --output-on-failure
set(NESRECOMP_RUNNER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../runner")
get_filename_component(NESRECOMP_RUNNER_ROOT "${NESRECOMP_RUNNER_ROOT}" ABSOLUTE)
set(NESRECOMP_ENABLE_NET ON CACHE BOOL "" FORCE)
set(NESRECOMP_NET_ICE ON CACHE BOOL "Build with ICE (the hub tests need it)")
include("${NESRECOMP_RUNNER_ROOT}/recomp_net.cmake")

# One engine-side build per seat ceiling: 2 is every title that has not opted
# in; 4 is NESRECOMP_INPUT_SEATS=4 (logical seats 3/4).
foreach(SEATS 2 4)
    add_executable(netplay_plan_test_${SEATS} netplay_plan_test.c stubs.c ${NESRECOMP_RUNNER_SOURCES})
    add_executable(netplay_e2e_${SEATS} netplay_e2e.c stubs.c ${NESRECOMP_RUNNER_SOURCES})
    foreach(T netplay_plan_test_${SEATS} netplay_e2e_${SEATS})
        target_include_directories(${T} PRIVATE "${NESRECOMP_RUNNER_ROOT}/include" ${NESRECOMP_RUNNER_INCLUDE_DIRS}
            "${NESRECOMP_RECOMP_NET_ROOT}/src") # test-only: rnet_os_* clock/sleep
        target_link_libraries(${T} PRIVATE ${NESRECOMP_RUNNER_LIBRARIES})
        target_compile_definitions(${T} PRIVATE NESRECOMP_INPUT_SEATS=${SEATS} NES_SEAT_CAP_EXPECT=${SEATS})
        if(NOT WIN32)
            target_compile_definitions(${T} PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
        endif()
    endforeach()
endforeach()

enable_testing()
foreach(SEATS 2 4)
    add_test(NAME plan_${SEATS}seat_build COMMAND netplay_plan_test_${SEATS})
    # Headless LAN: N processes, real UDP on loopback, NES_NET_* env contract.
    foreach(N 2 3 4)
        add_test(NAME lan_${N}seats_${SEATS}seat_build COMMAND netplay_e2e_${SEATS} lan ${N})
    endforeach()
    if(NESRECOMP_NET_ICE)
        foreach(N 2 3 4)
            add_test(NAME icehub_${N}seats_${SEATS}seat_build COMMAND netplay_e2e_${SEATS} icehub ${N})
        endforeach()
    endif()
endforeach()

# Optional: the full production path against a real lobby server on loopback
# (e.g. recomp-net-server's target/debug/recomp-net-server).
set(NES_E2E_LOBBY_SERVER "" CACHE FILEPATH "recomp-net-server binary for the lobby e2e tests")
if(NES_E2E_LOBBY_SERVER AND NESRECOMP_NET_ICE)
    foreach(SEATS 2 4)
        foreach(N 2 3 4)
            add_test(NAME lobby_${N}seats_${SEATS}seat_build COMMAND netplay_e2e_${SEATS} lobby ${N} "${NES_E2E_LOBBY_SERVER}")
        endforeach()
    endforeach()
endif()
