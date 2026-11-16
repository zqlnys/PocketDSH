#ifndef E2_MEMORY_H
#define E2_MEMORY_H

#include <stddef.h>
#include <stdint.h>

/// Guest address space.
///
/// The guest sees a flat 48-bit address space backed by host memory. Regions are
/// allocated by the loader (ELF segments, stack) and freed by teardown; there is
/// no paging, because a guest process here is one address space owned by one host
/// thread.
#define E2_PAGE_SIZE 4096
#define E2_PAGE_MASK (~((uint64_t)E2_PAGE_SIZE - 1))
#define E2_MAX_REGIONS 64

typedef struct {
    uint64_t base;   ///< guest base address, page aligned
    size_t size;     ///< byte length, page aligned
    uint8_t *host;   ///< host buffer backing this region
    int prot;        ///< E2_PROT_* bits
} e2_region;

typedef struct {
    e2_region regions[E2_MAX_REGIONS];
    unsigned count;
} e2_memory;

#define E2_PROT_READ 1
#define E2_PROT_WRITE 2
#define E2_PROT_EXEC 4

void e2_memory_init(e2_memory *mem);
void e2_memory_destroy(e2_memory *mem);

/// Reserves `size` bytes at a host-chosen guest address, rounded up to page size.
/// Returns 0 on success, negative on failure.
int e2_memory_map_any(e2_memory *mem, size_t size, int prot, uint64_t *base_out);

/// Reserves `size` bytes at exactly `base`. Fails if the range overlaps an
/// existing region.
int e2_memory_map_at(e2_memory *mem, uint64_t base, size_t size, int prot);

/// Copies host bytes into a guest region. Returns 0 on success.
int e2_memory_write(e2_memory *mem, uint64_t addr, const void *src, size_t len);

/// Copies guest bytes out. Returns 0 on success.
int e2_memory_read(const e2_memory *mem, uint64_t addr, void *dst, size_t len);

/// Resolves a guest address to a host pointer for `len` bytes, respecting `prot`.
/// Returns NULL when the range is unmapped or lacks the requested permission.
uint8_t *e2_memory_host_ptr(e2_memory *mem, uint64_t addr, size_t len, int prot);

/// Returns the region containing `addr`, or NULL.
const e2_region *e2_memory_find(const e2_memory *mem, uint64_t addr);

/// Total bytes mapped, for diagnostics.
size_t e2_memory_total(const e2_memory *mem);

#endif
