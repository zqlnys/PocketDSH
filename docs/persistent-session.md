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

---

# Threads: what was learned, and where it stands

Attempted seven times across as many rounds. Not working, but less mysterious than it was.

## Fixed along the way

- **The thread's entry point was the parent's link register.** `svc` does not write x30, so
  `clone`'s child resumes at the instruction *after* the svc, exactly as the parent does. Passing
  the LR sent the new thread into whatever `pthread_create` would have returned to - which is why
  its pc sat inside `pthread_create` and it executed that function's remaining code as its body.
  `exec.c` has already advanced pc past the svc when the syscall handler runs, so pc is the resume
  address.
- **`TL` becomes the thread's `TPIDR_EL0`.** Inheriting the creator's means the child reads the
  parent's thread-local storage, which is worse than having none.
- **`CLONE_PARENT_SETTID` and `CLONE_CHILD_SETTID`** write the tid into the caller's pointers, which
  `pthread_create` waits on.
- **`rt_sigsuspend` (133)** was unimplemented and is where a thread parks.

## Where it stands

The thread now *runs*, which it never did. Two more fixes were needed to get that far:

- **`pump_children_for_tab` skipped anything sharing the caller's file table**, with the comment
  "that is us". A thread shares its creator's table by definition, so every thread was skipped.
  Re-entry is prevented by the `running` flag, which is what that check was reaching for.
- **A parked futex wait yielded without running anyone.** `run_proc` returned, the scheduler called
  it again for the same process, the wait parked again, and the thread that was supposed to set
  the word never ran. The wait now pumps first.

The watchdog made this look like a thread spinning for 46 million instructions. That count was
inherited: `t->m = *parent` copies the instruction counter, and the thread had executed none of it.

With `E2_THREADS=1`, `start()` still does not return: the thread runs, reaches musl, and repeats
one instruction at `pthread_create+0x768` without advancing.

Without it, the default path raises `RuntimeError: can't start new thread`, which a program can
catch and degrade from. That has been true through all seven attempts and every regression suite
passes.

## What to look at next

The child runs a long time without reaching the start routine. musl's `__clone` child path takes a
branch on `x0` being zero and then calls through a function pointer; either that branch is taken
wrongly, or the pointer is not what musl stored. Instrumenting `__clone`'s child path - the pc of
first execution and the value it branches on - is the next concrete step, and it is a better one
than another theory.
