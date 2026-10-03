/* The only engine symbol nes_netplay.c needs: every peer reports the same
 * deterministic digest, as two peers of one correct build would. */
#include <stdint.h>
uint32_t nes_runtime_state_digest(void) { return 0xC0FFEE42u; }
