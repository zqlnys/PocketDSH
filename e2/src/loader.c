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

e2_load_result e2_load_elf_at(e2_machine *machine, const uint8_t *image, size_t size,
                              uint64_t preferred_base, e2_loaded_image *out) {
    // The default entry point runs the same code with the compiled-in base.
    extern e2_load_result e2_load_elf_full_base(e2_machine *, const uint8_t *, size_t,
                                                bool, e2_loaded_image *, uint64_t);
    return e2_load_elf_full_base(machine, image, size, true, out, preferred_base);
}

e2_load_result e2_load_elf_full(e2_machine *machine, const uint8_t *image, size_t size,
                                bool allow_interpreter, e2_loaded_image *out) {
    extern e2_load_result e2_load_elf_full_base(e2_machine *, const uint8_t *, size_t,
                                                bool, e2_loaded_image *, uint64_t);
    return e2_load_elf_full_base(machine, image, size, allow_interpreter, out, 0);
}

e2_load_result e2_load_elf_full_base(e2_machine *machine, const uint8_t *image, size_t size,
                                     bool allow_interpreter, e2_loaded_image *out,
                                     uint64_t preferred_base) {
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
        const uint64_t want = preferred_base ? preferred_base : E2_PIE_BASE;
        bias = want - (lowest & ~(uint64_t)(E2_PAGE_SIZE - 1));
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
        // The program headers live inside the first PT_LOAD, so their guest
        // address is that segment's vaddr plus the offset of e_phoff within it.
        // For ET_EXEC bias is zero and using e_phoff alone yields a file offset
        // (0x40), which every later computation then treats as a pointer.
        uint64_t phdr_addr = eh->e_phoff + bias;
        for (unsigned i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type != PT_LOAD) {
                continue;
            }
            if (eh->e_phoff >= ph[i].p_offset &&
                eh->e_phoff < ph[i].p_offset + ph[i].p_filesz) {
                phdr_addr = ph[i].p_vaddr + bias + (eh->e_phoff - ph[i].p_offset);
                break;
            }
        }
        out->phdr_addr = phdr_addr;
        out->phnum = eh->e_phnum;
        out->phentsize = eh->e_phentsize;
    }
    return E2_LOAD_OK;
}

e2_load_result e2_load_elf(e2_machine *machine, const uint8_t *image, size_t size) {
    return e2_load_elf_full(machine, image, size, false, NULL);
}

// --- Relocation ---------------------------------------------------------------

#define DT_NULL 0
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_SYMENT 11

#define R_AARCH64_NONE 0
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE 1027

typedef struct {
    uint32_t st_name;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} e2_elf64_sym;

typedef struct {
    int64_t d_tag;
    uint64_t d_val;
} e2_elf64_dyn;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
} e2_elf64_rela;

/// Locates a DT_ entry value inside the PT_DYNAMIC table.
static bool find_dynamic(e2_machine *machine, const e2_loaded_image *info, int64_t tag,
                         uint64_t *value_out) {
    if (info->dynamic_addr == 0) {
        return false;
    }
    for (uint64_t offset = 0; offset + sizeof(e2_elf64_dyn) <= info->dynamic_size;
         offset += sizeof(e2_elf64_dyn)) {
        e2_elf64_dyn entry;
        if (e2_memory_read(&machine->mem, info->dynamic_addr + offset, &entry,
                           sizeof(entry)) != 0) {
            return false;
        }
        if (entry.d_tag == DT_NULL) {
            break;
        }
        if (entry.d_tag == tag && value_out != NULL) {
            *value_out = entry.d_val;
        }
    }
    return false;
}

e2_load_result e2_apply_relative_relocations(e2_machine *machine,
                                             const uint8_t *image, size_t size,
                                             const e2_loaded_image *image_info) {
    (void)image;
    (void)size;
    if (image_info == NULL || image_info->dynamic_addr == 0) {
        return E2_LOAD_OK;
    }

    uint64_t rela = 0;
    uint64_t relasz = 0;
    uint64_t relaent = sizeof(e2_elf64_rela);
    find_dynamic(machine, image_info, DT_RELA, &rela);
    find_dynamic(machine, image_info, DT_RELASZ, &relasz);
    find_dynamic(machine, image_info, DT_RELAENT, &relaent);

    if (rela == 0 || relasz == 0) {
        return E2_LOAD_OK;  // nothing to do
    }
    if (relaent != sizeof(e2_elf64_rela)) {
        return E2_LOAD_UNSUPPORTED;
    }

    const uint64_t base = image_info->load_bias;
    const uint64_t table = base + rela;

    for (uint64_t offset = 0; offset + relaent <= relasz; offset += relaent) {
        e2_elf64_rela entry;
        if (e2_memory_read(&machine->mem, table + offset, &entry, sizeof(entry)) != 0) {
            return E2_LOAD_TRUNCATED;
        }
        const unsigned type = (unsigned)(entry.r_info & 0xFFFFFFFFu);
        switch (type) {
            case R_AARCH64_NONE:
                break;
            case R_AARCH64_RELATIVE: {
                const uint64_t target = base + entry.r_offset;
                const uint64_t value = base + (uint64_t)entry.r_addend;
                if (e2_memory_write(&machine->mem, target, &value, sizeof(value)) != 0) {
                    return E2_LOAD_NO_MEMORY;
                }
                break;
            }
            case R_AARCH64_GLOB_DAT:
            case R_AARCH64_JUMP_SLOT: {
                // A self-contained interpreter binds these to its own symbols.
                // External binding needs a full resolver, which this backend does
                // not have yet, so an undefined symbol is a hard error rather than
                // a silently wrong address.
                const unsigned sym_index = (unsigned)(entry.r_info >> 32);
                uint64_t symtab = 0;
                uint64_t syment = sizeof(e2_elf64_sym);
                find_dynamic(machine, image_info, DT_SYMTAB, &symtab);
                find_dynamic(machine, image_info, DT_SYMENT, &syment);
                if (symtab == 0 || syment != sizeof(e2_elf64_sym)) {
                    return E2_LOAD_UNSUPPORTED;
                }
                e2_elf64_sym sym;
                if (e2_memory_read(&machine->mem,
                                   base + symtab + (uint64_t)sym_index * syment,
                                   &sym, sizeof(sym)) != 0) {
                    return E2_LOAD_TRUNCATED;
                }
                if (sym.st_shndx == 0) {
                    return E2_LOAD_UNSUPPORTED;  // undefined: needs an external resolver
                }
                const uint64_t value = base + sym.st_value;
                if (e2_memory_write(&machine->mem, base + entry.r_offset, &value,
                                    sizeof(value)) != 0) {
                    return E2_LOAD_NO_MEMORY;
                }
                break;
            }
            default:
                return E2_LOAD_UNSUPPORTED;
        }
    }
    return E2_LOAD_OK;
}
