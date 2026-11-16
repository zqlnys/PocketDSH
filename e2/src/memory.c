#include "e2/memory.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

void e2_memory_init(e2_memory *mem) {
    memset(mem, 0, sizeof(*mem));
}

void e2_memory_destroy(e2_memory *mem) {
    for (unsigned i = 0; i < mem->count; i++) {
        free(mem->regions[i].host);
        mem->regions[i].host = NULL;
    }
    mem->count = 0;
}

static size_t page_round(size_t size) {
    if (size == 0) {
        return E2_PAGE_SIZE;
    }
    return (size + E2_PAGE_SIZE - 1) & (size_t)E2_PAGE_MASK;
}

static const e2_region *find(const e2_memory *mem, uint64_t addr, size_t len) {
    for (unsigned i = 0; i < mem->count; i++) {
        const e2_region *r = &mem->regions[i];
        if (addr >= r->base && addr < r->base + r->size) {
            // The whole requested range has to fit in the same region; guests here
            // never straddle mappings, and refusing is safer than half-copying.
            if (len <= r->size && (addr - r->base) <= r->size - len) {
                return r;
            }
            return NULL;
        }
    }
    return NULL;
}

const e2_region *e2_memory_find(const e2_memory *mem, uint64_t addr) {
    return find(mem, addr, 1);
}

int e2_memory_map_at(e2_memory *mem, uint64_t base, size_t size, int prot) {
    if (mem->count >= E2_MAX_REGIONS) {
        return -1;
    }
    if ((base & ~E2_PAGE_MASK) != 0) {
        return -2;  // not page aligned
    }
    size_t rounded = page_round(size);
    if (base + rounded < base) {
        return -3;  // overflow
    }
    // Reject overlap.
    for (unsigned i = 0; i < mem->count; i++) {
        const e2_region *r = &mem->regions[i];
        if (base < r->base + r->size && r->base < base + rounded) {
            return -4;
        }
    }
    uint8_t *host = calloc(1, rounded);
    if (host == NULL) {
        return -5;
    }
    e2_region *slot = &mem->regions[mem->count++];
    slot->base = base;
    slot->size = rounded;
    slot->host = host;
    slot->prot = prot;
    return 0;
}

int e2_memory_map_any(e2_memory *mem, size_t size, int prot, uint64_t *base_out) {
    // Keep guest addresses low and deterministic so a bug is reproducible. The
    // loader will ask for explicit addresses once ELF loading lands.
    uint64_t base = 0x10000;
    for (;;) {
        bool free_range = true;
        for (unsigned i = 0; i < mem->count; i++) {
            const e2_region *r = &mem->regions[i];
            if (base < r->base + r->size && r->base < base + page_round(size)) {
                base = (r->base + r->size + E2_PAGE_SIZE - 1) & E2_PAGE_MASK;
                free_range = false;
                break;
            }
        }
        if (free_range) {
            break;
        }
    }
    int err = e2_memory_map_at(mem, base, size, prot);
    if (err != 0) {
        return err;
    }
    if (base_out != NULL) {
        *base_out = base;
    }
    return 0;
}

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

int e2_memory_write(e2_memory *mem, uint64_t addr, const void *src, size_t len) {
    uint8_t *dst = host_ptr_for(mem, addr, len, E2_PROT_WRITE);
    if (dst == NULL) {
        return -1;
    }
    memcpy(dst, src, len);
    return 0;
}

int e2_memory_read(const e2_memory *mem, uint64_t addr, void *dst, size_t len) {
    const uint8_t *src = host_ptr_for(mem, addr, len, E2_PROT_READ);
    if (src == NULL) {
        return -1;
    }
    memcpy(dst, src, len);
    return 0;
}

size_t e2_memory_total(const e2_memory *mem) {
    size_t total = 0;
    for (unsigned i = 0; i < mem->count; i++) {
        total += mem->regions[i].size;
    }
    return total;
}
