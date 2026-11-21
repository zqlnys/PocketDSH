#ifndef E2_ELF_COMPAT_H
#define E2_ELF_COMPAT_H

// Enough of <elf.h> for the parts of the harness that read an ELF header.
//
// Linux and macOS ship <elf.h>; the iOS SDK does not, so building the harness into the app
// failed on the include. Only three declarations are actually used - the header, the program
// header, and PT_INTERP - so this provides those rather than a whole ELF header set, and the
// real one is preferred wherever it exists.

#include <stdint.h>

#if defined(__has_include)
#  if __has_include(<elf.h>)
#    include <elf.h>
#    define E2_HAVE_SYSTEM_ELF 1
#  endif
#endif

#ifndef E2_HAVE_SYSTEM_ELF

typedef struct {
    unsigned char e_ident[16];
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
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#define PT_INTERP 3

// The header magic and the auxiliary-vector types. These live in <elf.h> on Linux and are
// missing from the SDK, which the harness needs because it builds the guest's auxv itself.
#define ELFMAG  "\177ELF"
#define SELFMAG 4

#define AT_NULL       0
#define AT_PHDR       3
#define AT_PHENT      4
#define AT_PHNUM      5
#define AT_PAGESZ     6
#define AT_BASE       7
#define AT_FLAGS      8
#define AT_ENTRY      9
#define AT_UID       11
#define AT_EUID      12
#define AT_GID       13
#define AT_EGID      14
#define AT_PLATFORM  15
#define AT_HWCAP     16
#define AT_CLKTCK    17
#define AT_SECURE    23
#define AT_RANDOM    25
#define AT_HWCAP2    26
#define AT_EXECFN    31
#define AT_MINSIGSTKSZ 51

#endif

#endif
