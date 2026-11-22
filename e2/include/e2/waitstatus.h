// How a wait status encodes a child's end.
//
// A shell reads this with WIFEXITED/WEXITSTATUS, so the shape matters more than the value: an
// exit code sits in bits 8-15, and a signal death puts the signal number in the low seven bits
// with 0x7F left clear. Getting the signal into the exit position reports 0x8F00 where the shell
// expects 0x000F - and `$?` becomes 0 or 143 depending on which half is read.
#ifndef E2_WAITSTATUS_H
#define E2_WAITSTATUS_H

#include <stdint.h>

/// The status a parent reads back. `killed_by` is zero for a normal exit.
static inline uint32_t e2_wait_status_encode(int exit_code, int killed_by) {
    if (killed_by != 0) {
        return (uint32_t)(killed_by & 0x7F);
    }
    return (uint32_t)((exit_code & 0xFF) << 8);
}

#endif
