#include "e2/loader.h"

#include <string.h>

#include "e2/memory.h"

// ELF constants from the System V gABI plus the AArch64 psABI supplement. Parsed
// by hand rather than via <elf.h> so the layout is explicit and testable on any
// host.

#define EI_NIDENT 16
#define EI_CLASS 4
#define EI_DATA 5

#define ELFCLASS64 2
#define ELFDATA2LSB 1

#define ET_EXEC 2
#define ET_DYN 3

#define EM_AARCH64 183

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3

#define PF_X 1
#define PF_W 2
#define PF_R 4

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} e2_elf64_ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} e2_elf64_phdr;

_Static_assert(sizeof(e2_elf64_ehdr) == 64, "ELF64 header size");
_Static_assert(sizeof(e2_elf64_phdr) == 56, "ELF64 program header size");

const char *e2_load_result_name(e2_load_result result) {
    switch (result) {
        case E2_LOAD_OK: return "ok";
        case E2_LOAD_BAD_MAGIC: return "bad magic";
        case E2_LOAD_UNSUPPORTED: return "unsupported";
        case E2_LOAD_NEEDS_INTERP: return "needs interpreter";
        case E2_LOAD_NO_MEMORY: return "out of memory";
        case E2_LOAD_TRUNCATED: return "truncated";
    }
    return "unknown";
}

static int prot_from_flags(uint32_t flags) {
    int prot = 0;
    if (flags & PF_R) prot |= E2_PROT_READ;
    if (flags & PF_W) prot |= E2_PROT_WRITE;
    if (flags & PF_X) prot |= E2_PROT_EXEC;
    return prot;
}

e2_load_result e2_load_elf_full(e2_machine *machine, const uint8_t *image, size_t size,
                                bool allow_interpreter, e2_loaded_image *out) {
    if (image == NULL || size < sizeof(e2_elf64_ehdr)) {
        return E2_LOAD_TRUNCATED;
    }
    const e2_elf64_ehdr *eh = (const e2_elf64_ehdr *)image;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0) {
        return E2_LOAD_BAD_MAGIC;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        return E2_LOAD_UNSUPPORTED;
    }
    if (eh->e_machine != EM_AARCH64) {
        return E2_LOAD_UNSUPPORTED;
    }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        return E2_LOAD_UNSUPPORTED;
    }
    if (eh->e_phentsize != sizeof(e2_elf64_phdr)) {
        return E2_LOAD_UNSUPPORTED;
    }
    if (eh->e_phoff > size || (uint64_t)eh->e_phnum * eh->e_phentsize > size - eh->e_phoff) {
        return E2_LOAD_TRUNCATED;
    }

    const e2_elf64_phdr *ph = (const e2_elf64_phdr *)(image + eh->e_phoff);

    bool has_interp = false;
    uint64_t dynamic_vaddr = 0;
    uint64_t dynamic_size = 0;
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP) {
            has_interp = true;
        } else if (ph[i].p_type == PT_DYNAMIC) {
            dynamic_vaddr = ph[i].p_vaddr;
            dynamic_size = ph[i].p_memsz;
        }
    }
    if (has_interp && !allow_interpreter) {
        return E2_LOAD_NEEDS_INTERP;
    }

    // Position-independent images are placed at a fixed base: reproducible
    // failures matter more than ASLR for an emulator under test.
    uint64_t bias = 0;
    if (eh->e_type == ET_DYN) {
        uint64_t lowest = UINT64_MAX;
        for (unsigned i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type == PT_LOAD && ph[i].p_memsz > 0 && ph[i].p_vaddr < lowest) {
                lowest = ph[i].p_vaddr;
            }
        }
        if (lowest == UINT64_MAX) {
            return E2_LOAD_UNSUPPORTED;
        }
        bias = E2_PIE_BASE - (lowest & ~(uint64_t)(E2_PAGE_SIZE - 1));
    }

    for (unsigned i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        const uint64_t vaddr = ph[i].p_vaddr + bias;
        const uint64_t memsz = ph[i].p_memsz;
        const uint64_t filesz = ph[i].p_filesz;
        if (memsz == 0) {
            continue;
        }
        if (ph[i].p_offset > size || filesz > size - ph[i].p_offset) {
            return E2_LOAD_TRUNCATED;
        }

        // p_vaddr is only congruent to p_offset modulo the page size, not page
        // aligned. Map whole pages around the segment and let the data land at
        // its real address inside them.
        const uint64_t page_mask = (uint64_t)(E2_PAGE_SIZE - 1);
        const uint64_t map_base = vaddr & ~page_mask;
        const uint64_t map_end = (vaddr + memsz + page_mask) & ~page_mask;

        const int final_prot = prot_from_flags(ph[i].p_flags);
        const bool fresh = e2_memory_find(&machine->mem, map_base) == NULL;
        if (fresh) {
            // Map writable for the load, narrow to the segment's real protection
            // once the bytes are in. Text segments are R+X and would otherwise
            // reject their own contents.
            if (e2_memory_map_at(&machine->mem, map_base, (size_t)(map_end - map_base),
                                 final_prot | E2_PROT_WRITE) != 0) {
                return E2_LOAD_NO_MEMORY;
            }
        } else if (e2_memory_find(&machine->mem, map_end - 1) == NULL) {
            // Partially overlapping layouts are more than this loader handles;
            // refusing beats mapping twice and corrupting the first segment.
            return E2_LOAD_NO_MEMORY;
        }

        if (filesz > 0) {
            if (e2_memory_write(&machine->mem, vaddr, image + ph[i].p_offset,
                                (size_t)filesz) != 0) {
                return E2_LOAD_NO_MEMORY;
            }
        }
        if (fresh && e2_memory_protect(&machine->mem, map_base,
                                       (size_t)(map_end - map_base), final_prot) != 0) {
            return E2_LOAD_NO_MEMORY;
        }
    }

    machine->cpu.pc = eh->e_entry + bias;
    // SP is a separate field and was set up by e2_machine_init; writing it through
    // the general register array would run past X30.

    if (out != NULL) {
        out->load_bias = bias;
        out->entry = eh->e_entry + bias;
        out->dynamic_addr = dynamic_vaddr ? dynamic_vaddr + bias : 0;
        out->dynamic_size = dynamic_size;
        out->phdr_addr = eh->e_phoff + bias;
        out->phnum = eh->e_phnum;
        out->phentsize = eh->e_phentsize;
    }
    return E2_LOAD_OK;
}

e2_load_result e2_load_elf(e2_machine *machine, const uint8_t *image, size_t size) {
    return e2_load_elf_full(machine, image, size, false, NULL);
}
