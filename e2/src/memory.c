#include "e2/memory.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

void e2_memory_init(e2_memory *mem) {
    memset(mem, 0, sizeof(*mem));
}

int e2_memory_clone(const e2_memory *src, e2_memory *dst) {
    e2_memory_init(dst);
    for (unsigned i = 0; i < src->count; i++) {
        const e2_region *r = &src->regions[i];
        uint8_t *buf = malloc(r->size);
        if (buf == NULL) {
            e2_memory_destroy(dst);
            return -1;
        }
        memcpy(buf, r->host, r->size);
        e2_region *d = &dst->regions[dst->count++];
        d->base = r->base;
        d->size = r->size;
        d->host = buf;
        d->prot = r->prot;
    }
    return 0;
}

void e2_memory_destroy(e2_memory *mem) {
    for (unsigned i = 0; i < mem->count; i++) {
        free(mem->regions[i].host);
    }
    memset(mem, 0, sizeof(*mem));
}

static size_t page_round(size_t size) {
    return (size + E2_PAGE_SIZE - 1) & ~(size_t)(E2_PAGE_SIZE - 1);
}

/// Region fully containing [addr, addr+len), with at least `prot` granted.
static const e2_region *find(const e2_memory *mem, uint64_t addr, size_t len) {
    // Same last-hit cache as region_at, for the same reason: this is the lookup every
    // instruction fetch and every data access goes through.
    e2_memory *mutable_mem = (e2_memory *)(uintptr_t)mem;
    const unsigned cached = mutable_mem->cache_index;
    if (cached < mem->count) {
        const e2_region *r = &mem->regions[cached];
        if (addr >= r->base && addr - r->base < r->size) {
            const size_t offset = (size_t)(addr - r->base);
            if (len <= r->size - offset) {
                return r;
            }
            return NULL;
        }
    }
    for (unsigned i = 0; i < mem->count; i++) {
        const e2_region *r = &mem->regions[i];
        if (addr < r->base || addr - r->base >= r->size) {
            continue;
        }
        const size_t offset = (size_t)(addr - r->base);
        if (len > r->size - offset) {
            return NULL;
        }
        mutable_mem->cache_index = i;
        return r;
    }
    return NULL;
}

/// Region containing `addr` regardless of length, or NULL.
// Tried and measured, so it does not have to be tried again: a 64-slot direct-mapped hint table
// keyed on the address, on the theory that CPython's startup walking a hundred-odd regions makes
// the linear scan below expensive. Three runs before and three after gave 2.98-3.01 s and
// 2.99-3.11 s - no change. Startup runs at about 76 ns per instruction against the benchmark's 46,
// and that is the guest's own code and data being cold, which a lookup hint cannot touch.
static const e2_region *region_at(const e2_memory *mem, uint64_t addr) {
    // Try the previously resolved region first. The cast only writes a cache hint, so the
    // const contract holds; a hint that no longer matches simply fails the containment
    // test and the scan below corrects it.
    e2_memory *mutable_mem = (e2_memory *)(uintptr_t)mem;
    const unsigned cached = mutable_mem->cache_index;
    if (cached < mem->count) {
        const e2_region *r = &mem->regions[cached];
        if (addr >= r->base && addr - r->base < r->size) {
            return r;
        }
    }
    for (unsigned i = 0; i < mem->count; i++) {
        const e2_region *r = &mem->regions[i];
        if (addr >= r->base && addr - r->base < r->size) {
            mutable_mem->cache_index = i;
            return r;
        }
    }
    return NULL;
}

const e2_region *e2_memory_find(const e2_memory *mem, uint64_t addr) {
    return region_at(mem, addr);
}

int e2_memory_map_at(e2_memory *mem, uint64_t base, size_t size, int prot) {
    if (size == 0 || mem->count >= E2_MAX_REGIONS) {
        return -1;
    }
    if ((base & (E2_PAGE_SIZE - 1)) != 0) {
        return -1;
    }
    const size_t rounded = page_round(size);
    if (region_at(mem, base) != NULL || region_at(mem, base + rounded - 1) != NULL) {
        return -1;
    }
    for (unsigned i = 0; i < mem->count; i++) {
        const e2_region *r = &mem->regions[i];
        const uint64_t r_end = r->base + r->size;
        if (base < r_end && r->base < base + rounded) {
            return -1;
        }
    }
    void *host = calloc(1, rounded);
    if (host == NULL) {
        return -1;
    }
    e2_region *r = &mem->regions[mem->count++];
    r->base = base;
    r->size = rounded;
    r->prot = prot;
    r->host = (uint8_t *)host;
    return 0;
}

int e2_memory_map_any(e2_memory *mem, size_t size, int prot, uint64_t *base_out) {
    if (size == 0) {
        return -1;
    }
    const size_t rounded = page_round(size);
    // qemu-user keeps every guest address inside the region it reserves at guest_base, and
    // the loader it maps already sits at 0x400000810000. Allocating anonymous mappings from
    // 0x10000000 instead put libpython and the heap in a different part of the address space
    // from everything else, which makes a trace taken here incomparable with qemu's and can
    // change behaviour wherever a pointer is narrowed or compared against a limit.
    uint64_t candidate = 0x4000000000ULL;
    for (int attempt = 0; attempt < E2_MAX_REGIONS * 4; attempt++) {
        bool clash = false;
        uint64_t next = candidate;
        for (unsigned i = 0; i < mem->count; i++) {
            const e2_region *r = &mem->regions[i];
            const uint64_t r_end = r->base + r->size;
            if (candidate < r_end && r->base < candidate + rounded) {
                clash = true;
                if (r_end > next) {
                    next = r_end;
                }
            }
        }
        if (!clash) {
            if (e2_memory_map_at(mem, candidate, size, prot) != 0) {
                return -1;
            }
            if (base_out != NULL) {
                *base_out = candidate;
            }
            return 0;
        }
        candidate = (next + E2_PAGE_SIZE - 1) & ~(uint64_t)(E2_PAGE_SIZE - 1);
    }
    return -1;
}

int e2_memory_protect(e2_memory *mem, uint64_t addr, size_t size, int prot) {
    if (size == 0) {
        return 0;
    }
    // Apply the requested protection to every region the range touches. Treating
    // this as advisory hid a real class of bug: a stray write into .rodata
    // silently corrupted constant tables (string literals included), and the
    // symptom appeared far away as nonsense option parsing.
    size_t done = 0;
    while (done < size) {
        const uint64_t here = addr + done;
        const e2_region *r = region_at(mem, here);
        if (r == NULL) {
            return -1;
        }
        const size_t avail = (size_t)(r->base + r->size - here);
        e2_region *writable = (e2_region *)r;
        writable->prot = prot;
        done += avail < (size - done) ? avail : (size - done);
    }
    return 0;
}

/// Host pointer for exactly this access, or NULL when it crosses a boundary or
/// the protection does not allow it.
static uint8_t *host_ptr_for(const e2_memory *mem, uint64_t addr, size_t len, int prot) {
    const e2_region *r = find(mem, addr, len);
    if (r == NULL) {
        return NULL;
    }
    if ((r->prot & prot) != prot) {
        return NULL;
    }
    return r->host + (addr - r->base);
}

uint8_t *e2_memory_host_ptr(e2_memory *mem, uint64_t addr, size_t len, int prot) {
    return host_ptr_for(mem, addr, len, prot);
}

int e2_memory_read(const e2_memory *mem, uint64_t addr, void *dst, size_t len) {
    if (len == 0) {
        return 0;
    }
    uint8_t *out = (uint8_t *)dst;
    size_t done = 0;
    while (done < len) {
        const uint64_t here = addr + done;
        const uint8_t *src = host_ptr_for(mem, here, len - done, E2_PROT_READ);
        if (src != NULL) {
            memcpy(out + done, src, len - done);
            return 0;
        }
        // Adjacent regions are normal (musl maps each PT_LOAD separately) and a
        // memcpy does not know where one ends; copy this region's share and go on.
        const e2_region *r = region_at(mem, here);
        if (r == NULL || (r->prot & E2_PROT_READ) == 0) {
            return -1;
        }
        const size_t avail = (size_t)(r->base + r->size - here);
        if (avail == 0) {
            return -1;
        }
        const uint8_t *part = host_ptr_for(mem, here, avail, E2_PROT_READ);
        if (part == NULL) {
            return -1;
        }
        memcpy(out + done, part, avail);
        done += avail;
    }
    return 0;
}

int e2_memory_write(e2_memory *mem, uint64_t addr, const void *src, size_t len) {
    if (len == 0) {
        return 0;
    }
    const uint8_t *in = (const uint8_t *)src;
    size_t done = 0;
    while (done < len) {
        const uint64_t here = addr + done;
        uint8_t *dst = host_ptr_for(mem, here, len - done, E2_PROT_WRITE);
        if (dst != NULL) {
            memcpy(dst, in + done, len - done);
            return 0;
        }
        const e2_region *r = region_at(mem, here);
        if (r == NULL || (r->prot & E2_PROT_WRITE) == 0) {
            return -1;
        }
        const size_t avail = (size_t)(r->base + r->size - here);
        if (avail == 0) {
            return -1;
        }
        uint8_t *part = host_ptr_for(mem, here, avail, E2_PROT_WRITE);
        if (part == NULL) {
            return -1;
        }
        memcpy(part, in + done, avail);
        done += avail;
    }
    return 0;
}

size_t e2_memory_total(const e2_memory *mem) {
    size_t total = 0;
    for (unsigned i = 0; i < mem->count; i++) {
        total += mem->regions[i].size;
    }
    return total;
}
