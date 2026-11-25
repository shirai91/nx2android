// What the tests that link hle_nvn.c and hle_bigstack.c without the host need of it: weak definitions, so a test that defines its own keeps it
#include <stdint.h>
__attribute__((weak)) uint64_t g_t0_ns;   // process start time (host main.c), read by hle_nvn's per-frame timing
__attribute__((weak)) int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }   // no guest address space (hle_bigstack.c)
__attribute__((weak)) void dm_vi_frame_queued(uint64_t frame, uint64_t at, uint64_t grid, uint64_t tick) { (void)frame; (void)at; (void)grid; (void)tick; }   // no vi layer frame history (hle_vi.c), told of each present by hle_nvn
__attribute__((weak)) void dm_vi_frame_ticket(uint64_t frame, uint64_t ticket) { (void)frame; (void)ticket; }   // no vi layer frame history (hle_vi.c), told of each present's ticket by hle_nvn
