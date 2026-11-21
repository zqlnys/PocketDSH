// Trace-state globals the E2 core expects to find at link time.
//
// The interpreter's syscall layer prints `g_global_step` and `g_dbg_pid` when a tracing
// environment variable is set. Those two used to live in tools/rune.c, the command-line
// harness, which the iOS app does not compile - so the device build failed on two undefined
// symbols. Defining them here keeps the core unchanged and lets tracing work in the app too,
// in case it is ever needed from a device.
//
// They are plain counters: nothing in the interpreter reads them except the fprintf that
// guards on getenv, so their value only matters when tracing is on.

unsigned long long g_global_step = 0;
int g_dbg_pid = 1;
