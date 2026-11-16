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

/// Loads a statically linked aarch64 ELF into the machine's address space and
/// points PC at its entry.
///
/// Deliberately refuses dynamic executables for now: silently loading only the
/// main image would fail in confusing ways later, and a clear E2_LOAD_NEEDS_INTERP
/// is a better signal.
e2_load_result e2_load_elf(e2_machine *machine, const uint8_t *image, size_t size);

/// Human-readable name, for tests and logs.
const char *e2_load_result_name(e2_load_result result);

#endif
