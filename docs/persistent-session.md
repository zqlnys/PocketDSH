# Persistent session: what was measured, and what a real attempt needs

Written after a round that measured the problem, tried an optimisation that did not pay, and
started an implementation that was not designed well enough to finish.

## The measurement

| | instructions | wall clock |
| --- | --- | --- |
| `busybox true` | 10,053 | 0.01 s |
| `python3 -S -c pass` | 31,022,080 | 2.35 s |
| `python3 -c 'print(1)'` | 39,169,126 | 3.00 s |
| `python3 -S -c 'import json'` | 65,140,277 | — |

A bare CPython start is three thousand times a busybox command, and it is not the loader: it is
`Py_Initialize` building types, modules and the builtins namespace. `-S` removes 21% and changes
`sys.path` semantics, so it is not a default.

The import breakdown (`-X importtime`) is `site`, `os`, `encodings`, `_collections_abc` - nothing
dominant, nothing removable without changing what Python does.

## What was tried and rejected

A 64-slot direct-mapped hint table for the region lookup, on the theory that startup touching a
hundred-odd mappings made the linear scan expensive. Three runs before and three after: 2.98-3.01 s
and 2.99-3.11 s. No change. Startup runs at about 76 ns per instruction against the benchmark's 46,
and that is cold code and data in the guest, which a lookup hint cannot touch.

## Why busybox does not need this

Ten busybox commands take 0.12 s in total - 12 ms each. A persistent shell would save perhaps 10 ms
per command. Not worth a session protocol.

## What a real attempt needs

`run_proc` is already resumable: it returns when its budget is exhausted and the process stays
alive, which is how the thread pumping works. So the mechanism exists.

1. **Turn ownership.** The harness runs the machine in quanta and, between quanta, decides whether
   to feed the next command. The guest polls a command file and appends to an output file, ending
   each result with a sentinel. Without a rule for who acts when, the two sides deadlock or spin.
2. **Framing.** The sentinel must be unforgeable by command output - a UUID per session, not a
   fixed string, or a command that prints it desynchronises the session.
3. **Cost of the wait.** A polling guest burns instructions while idle. Either the poll sleeps (the
   guest needs a working `nanosleep`) or the harness stops running the machine between commands.
   The second is better and needs the loop above to own the waiting, not the guest.
4. **Failure.** A command that hangs, a guest that crashes, a session that outlives a call: each
   needs an answer, and the answer has to be "report it", not "hang" - which is the same class of
   mistake this project has already fixed three times.

None of that is hard. All of it has to be decided before the first line, which is what went wrong
the first time.
