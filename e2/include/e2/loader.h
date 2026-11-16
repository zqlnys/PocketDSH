#ifndef E2_LOADER_H
#define E2_LOADER_H

#include <stddef.h>
#include <stdint.h>

#include "e2/exec.h"

/// Outcome of loading a guest executable.
typedef enum {
    E2_LOAD_OK = 0,
    E2_LOAD_BAD_MAGIC,      ///< not an ELF file
    E2_LOAD_UNSUPPORTED,    ///< 32-bit, big-endian, or not aarch64
    E2_LOAD_NEEDS_INTERP,   ///< dynamic executable; needs a dynamic linker first
    E2_LOAD_NO_MEMORY,      ///< a segment could not be mapped
    E2_LOAD_TRUNCATED,      ///< headers or segment data run past the image
} e2_load_result;

/// Where a position-independent image was placed. Relocations are expressed
/// relative to the image, so the loader has to report the bias it chose.
typedef struct {
    uint64_t load_bias;     ///< added to every p_vaddr
    uint64_t entry;         ///< entry point with the bias applied
    uint64_t dynamic_addr;  ///< guest address of PT_DYNAMIC, 0 when absent
    uint64_t dynamic_size;
    uint64_t phdr_addr;     ///< guest address of the program headers
    uint16_t phnum;
    uint16_t phentsize;
} e2_loaded_image;

/// Loads an aarch64 ELF into the machine's address space.
///
/// Handles ET_EXEC (fixed addresses) and ET_DYN (position independent; placed at
/// a deterministic base so failures reproduce). Returns E2_LOAD_NEEDS_INTERP when
/// the image carries PT_INTERP and the caller asked to refuse dynamic
/// executables; callers that intend to run the interpreter pass
/// `allow_interpreter = true` and load the interpreter themselves.
e2_load_result e2_load_elf_full(e2_machine *machine, const uint8_t *image, size_t size,
                                bool allow_interpreter, e2_loaded_image *out);

/// Convenience wrapper that refuses dynamic executables.
e2_load_result e2_load_elf(e2_machine *machine, const uint8_t *image, size_t size);

/// Applies the relocations a position-independent image needs before it can run.
///
/// Handles R_AARCH64_RELATIVE (the bulk of what a static-pie needs) and ignores
/// R_AARCH64_NONE. Symbol-based relocations are reported as unsupported for now:
/// silently skipping them would produce a link that fails somewhere far away.
///
/// The image bytes must be the same buffer that was loaded, because relocation
/// targets are addressed through the bias.
e2_load_result e2_apply_relative_relocations(e2_machine *machine,
                                             const uint8_t *image, size_t size,
                                             const e2_loaded_image *image_info);

/// Human-readable name, for tests and logs.
const char *e2_load_result_name(e2_load_result result);

#endif
