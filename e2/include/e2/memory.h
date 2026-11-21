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
/**
 * Mappings a guest may hold at once.
 *
 * 64 was far too few: Python fills it during startup, after which `mmap`
 * returns ENOMEM and the allocator hands out addresses it never mapped. The
 * symptom appears much later as a wild pointer, which is why this looked like a
 * decoder problem for so long.
 */
// A fixed table, so the ceiling is a hard failure rather than a slowdown: reaching it makes
// the guest's mmap return ENOMEM, which CPython reports as MemoryError somewhere unrelated to
// memory being short. 1024 left CPython a factor of five of headroom on a desktop; a device
// loading more shared objects may not be so lucky, and the extra entries cost 128 KB.
#define E2_MAX_REGIONS 4096

/// Where position-independent images are placed. Fixed so a failing layout
/// reproduces exactly instead of moving with each run.
#define E2_PIE_BASE 0x40000000ULL

typedef struct {
    uint64_t base;   ///< guest base address, page aligned
    size_t size;     ///< byte length, page aligned
    uint8_t *host;   ///< host buffer backing this region
    int prot;        ///< E2_PROT_* bits
} e2_region;

typedef struct {
    e2_region regions[E2_MAX_REGIONS];
    unsigned count;
    // Cache of the region the last lookup resolved. Instruction fetch reads through
    // region_at on every step, and a linear scan over every mapping - CPython creates about
    // seventy - dominated the interpreter's own cost. Holding the last hit makes the common
    // case one comparison, because execution stays inside one region for millions of steps.
    // It is a pure cache: stale values are detected by the containment test and fall back.
    unsigned cache_index;
} e2_memory;

#define E2_PROT_READ 1
#define E2_PROT_WRITE 2
#define E2_PROT_EXEC 4

/// Fast path for the interpreter's own accesses: a fetch and most loads and stores land in
/// the region the previous one did, so this is one containment test and a memcpy.
///
/// Returns 0 on success and fills `dst`; returns non-zero when the access is not a simple
/// in-region one, and the caller must fall back to e2_memory_read. Profiling put a fifth of a
/// real run inside the two nested calls this replaces.
static inline int e2_memory_read_fast(const e2_memory *mem, uint64_t addr, void *dst, size_t len) {
    const unsigned cached = ((const e2_memory *)mem)->cache_index;
    if (cached < mem->count) {
        const e2_region *r = &mem->regions[cached];
        if ((r->prot & E2_PROT_READ) != 0 && addr >= r->base && addr - r->base < r->size) {
            const size_t offset = (size_t)(addr - r->base);
            if (len <= r->size - offset) {
                __builtin_memcpy(dst, r->host + offset, len);
                return 0;
            }
        }
    }
    return 1;
}

void e2_memory_init(e2_memory *mem);
void e2_memory_destroy(e2_memory *mem);

/// Deep-copies every region of `src` into `dst`, which must not hold live
/// regions. This is the whole of fork()'s address-space work. Returns 0 on
/// success, negative on allocation failure (in which case `dst` is left empty).
int e2_memory_clone(const e2_memory *src, e2_memory *dst);

/// Reserves `size` bytes at a host-chosen guest address, rounded up to page size.
/// Returns 0 on success, negative on failure.
int e2_memory_map_any(e2_memory *mem, size_t size, int prot, uint64_t *base_out);

/// Reserves `size` bytes at exactly `base`. Fails if the range overlaps an
/// existing region.
int e2_memory_map_at(e2_memory *mem, uint64_t base, size_t size, int prot);

/// Changes the protection of an already mapped range.
///
/// The loader needs this: a segment can be R+X in its final form while its bytes
/// still have to be written during load, so it is mapped writable first and
/// narrowed afterwards, exactly like a real loader does.
int e2_memory_protect(e2_memory *mem, uint64_t addr, size_t size, int prot);

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
