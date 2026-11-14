// Write-protect dirty-page tracking for guest memory (nvn_dirty.c)
#pragma once
#include <stddef.h>
#include <stdint.h>

void nvn_dirty_add_range(const void *mem, size_t size);           // Register a memory pool as a tracked range (the first call registers the page-fault hook and the host pre-write hook)
void nvn_dirty_stat(int *live, int *peak, int *cap);                // number of registered memory pools, peak, tracking table capacity (for the memory readout line)
void nvn_dirty_remove_range(const void *mem);                     // Pool destruction: restore protected pages to writable and mark invalid
int nvn_dirty_tracked(const void *addr, size_t len);
void nvn_dirty_protect(const void *addr, size_t len);              // Set the covered pages read-only
void nvn_dirty_release(const void *addr, size_t len);              // Make the covered pages writable again and stamp them as written (a span no longer watched)
uint64_t nvn_dirty_released(void);                                 // Cumulative: pages unprotected by nvn_dirty_release
int nvn_dirty_runs(void);                                          // Runs of protected pages now (each a mapping of the process)
int nvn_dirty_defrag(int (*remap)(void *mem, size_t len));         // Map the most fragmented ranges again in one piece (remap: read-only, 0 = done, 1 = nothing mapped, -1 = failed after mapping a part); number of ranges done
int nvn_dirty_quiet(const void *mem, size_t len);                  // nvn_dirty_swap's answer before it touches a page: 0 = go ahead, 1 = untracked span, 2 = written by the host lately (try later), -1 = not whole host pages
int nvn_dirty_swap(void *mem, size_t len, int (*fill)(void *at, size_t n, size_t off, void *ctx), void *ctx, size_t *done);   // New pages under a span, no write lost, a slice at a time (fill: copy, map read-only, 0 = done); 1 = untracked span, 2 = written by the host lately (try later); done: bytes swapped
uint64_t nvn_dirty_now(void);                                      // Current generation: taken before upload, as the stamp of that content
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp); // Covered pages were written after the stamp -> 1
const void *nvn_dirty_span(const void *addr, size_t len);          // The tracked range holding the span (a handle to keep; NULL = untracked)
int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp);   // nvn_dirty_since through a kept handle (rechecked, scanned again only if it no longer holds the span)
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps);           // Cumulative: number of page faults triggered by game writes, number of pages unprotected before host writes
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns);   // Cumulative: this module's mprotect call count, pages covered, time spent
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total); // Currently live slots, slot table high-water mark, total number of registrations
