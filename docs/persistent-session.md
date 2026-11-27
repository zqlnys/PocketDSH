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

## Measured later: the shell half already works

The note above assumed the guest side needed building. It does not:

```sh
printf 'cd /tmp\npwd\nX=hello\necho "X=$X"\n' | rune busybox sh
# /tmp
# X=hello
```

One guest shell reads commands from a pipe on its stdin and keeps its state between them - the
working directory, the variables, everything a session is for. Nothing on the guest side has to
change; `run_proc` already handles a shell blocked on a read.

What is missing is the turn-taking: the app has to be able to write the next command after the shell
has finished the previous one, rather than sending them all before the run starts. That is the same
four decisions listed below, minus the first two - the channel exists and the framing can be the
shell's own prompt or a per-command sentinel echo.

So the estimate in this document was wrong in the direction that matters: the expensive part is done.

A first attempt at the turn-taking loop was written and taken back out. It ran the first command and
then stopped: the shell answered, the marker for that round appeared, and the second round never
reached it. The marker also leaked into the output the app would have seen, because the slice was
taken at the marker rather than at the line before it.

Neither is hard. Both are the kind of thing that needs the loop to be looked at as a whole rather
than extended while it is half working - which is exactly what the first attempt at this feature did,
and the reason this document exists. The guest half stays as measured; the loop is the next piece of
work, not a larger one.

### What the second attempt found

A slow writer proves the guest is fine: feeding a FIFO that pauses two seconds between commands runs
both, so a shell blocked on a pipe really does stay alive and keep its state.

Two things about the harness came out of it, both worth knowing before the next attempt:

1. **`lseek(fd, 0, SEEK_END)` on a duplicate of fd 1 moves the offset fd 1 shares.** The first
   attempt used it to find how much the guest had written, which put the guest's next write where
   the seek had left off. `fstat` reports the size without touching the offset.
2. **In the command-line harness, fd 1 is the terminal.** The guest's output goes straight there and
   reading back through a duplicate of a terminal returns nothing useful, so the loop only has a
   channel when the bridge has redirected stdout to a file. Threading the loop through the bridge -
   where the file exists - is the honest way to test it; testing it from the command line tests the
   wrong thing.

The second attempt still stopped after one round with no diagnostic, which is the failure mode this
document keeps warning about. It is taken back out rather than left in place.

### The missing primitive, found

```sh
mkfifo /tmp/f; (printf 'echo A\n'; sleep 20) > /tmp/f &
rune busybox sh < /tmp/f      # prints A, then runs until it is killed: exit 124 at 10 s
```

A guest shell blocked on a read, with the pipe's write end still open and no data in it, does not
give control back. `run_proc` owns the machine until the guest exits or the instruction cap is
reached, and `fs_read` returning EAGAIN goes to the guest, not to the caller.

That is why both attempts stalled: the loop was waiting for a return that no code path produces. The
first two ideas - a command file to poll, a marker protocol - were solutions to a problem that sits
one layer further down.

**What a session needs first is a yield.** When a blocking read would block and nothing can be run,
the harness has to be able to leave `run_proc` and come back to it, so the caller can write the next
command. Everything in the list below is downstream of that, and none of it is worth building until
the yield exists.

Two shapes were open: a flag that `fs_read` sets and `run_proc` checks on the way out, or a
`run_proc` budget that expires on a blocked read instead of spinning. The first is built.

### The yield, working

`E2_SESSION` now makes the harness set its stdin non-blocking and turn a read that nothing can
satisfy into a return from `run_proc`, leaving the guest alive. Measured with a FIFO that stays open:

```
timeout 20 rune busybox sh < /tmp/f7     # A, then: "e2: the guest is waiting for input"
# exit 0, 0 seconds
```

Three facts were needed and only the third was the missing one:

1. The guest side was already right - a shell on a pipe keeps its state (`cd`, variables).
2. `lseek(SEEK_END)` on a dup of fd 1 moves the offset fd 1 shares; `fstat` does not.
3. **The host read blocked, so `fs_read` never returned and a flag inside it could never fire.**
   Setting the descriptor non-blocking is what makes the yield reachable.

What remains is the loop itself: write the next command, watch for the marker, hand the output back.
The primitive it needed now exists.

### Where it stands

The harness side works and is tested: `E2_SESSION=1` keeps one shell across commands, and
`E2_SESSION_MARK` sends the completion marker to its own file so the output has no framing in it. The
filesystem suite checks both - six commands, six markers, no marker text in stdout.

The bridge has the API for it - `e2_bridge_session_start`, `_write`, `_wait`, `_alive`, `_close` - and
it does not work yet: the first `_wait` times out with no marker. Nothing calls it, so nothing is
broken by it; the command path the app uses is unchanged and its tests pass.

Two things to look at first. The session thread calls `e2_rune_main` directly, so the stdout
redirection `e2_bridge_run_in` performs is not in place - the guest's output goes to the app's real
stdout rather than the file the caller was given. And the harness makes its own stdin pipe, replacing
the one the bridge set up, so there are two layers of forwarding where one was intended; whether the
inner writer thread ever sees a write is the thing to check before anything else.

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

With `E2_THREADS=1`, `start()` still does not return, but the thread now goes much further than it
ever did: `E2_TRACE_THREAD=1` shows it advancing normally through musl and into libpython
(`0x400020512c` and beyond), with a stack of its own and `st=0` on every step.

An earlier reading of this trace said the thread repeated one instruction without advancing. That
was an artefact of sampling its pc at `run_proc`'s entry: consecutive entries each took one step, so
the pc looked unchanged. The per-step trace above `run_proc` shows it moving.

So the remaining problem is not that the thread is stuck; it is that the handshake never completes -
the parent stays parked on its futex while the child runs Python code that should eventually set the
word it is waiting on.

## The one hard fact

The thread's pc was seen at `0x872ca4`, and that address is **inside none of its 185 mappings**.
Against the address it should be:

```
0x400000872ca4   what it should be, inside musl
0x000000872ca4   what it is
0x400000000000   the difference - the top thirty-two bits
```

So a pointer lost its high half somewhere between being formed and being branched to. That is a
concrete defect with a shape, unlike the three rounds of "it spins" that preceded it, and the thing
to find next is which instruction dropped the bits: a 32-bit register used where a 64-bit one was
meant, either in the guest's own code or in how this backend decodes a 32-bit form.

A check for a jump to an unmapped address was added and removed in the same round: it was a linear
scan over the region table on every instruction, which took the benchmark from 52 ns to 89 on
busybox alone and would have been far worse on Python's 185 mappings. It also never fired, because
the pc does not arrive by a clean jump - the fetch faults instead, which is where to look.

Without it, the default path raises `RuntimeError: can't start new thread`, which a program can
catch and degrade from. That has been true through all seven attempts and every regression suite
passes.

## What to look at next

The child runs a long time without reaching the start routine. musl's `__clone` child path takes a
branch on `x0` being zero and then calls through a function pointer; either that branch is taken
wrongly, or the pointer is not what musl stored. Instrumenting `__clone`'s child path - the pc of
first execution and the value it branches on - is the next concrete step, and it is a better one
than another theory.

---

# Threads: closed for now

Eleven rounds. Not working, and this is where it stops being chased.

## What is known

The thread runs — it never did before this — through musl and into libpython, with its own stack,
`st=0` on every step. The parent stays parked on a futex. The thread's pc is eventually seen at
`0x872ca4`, which is inside none of its 185 mappings and is exactly `0x400000872ca4` with the high
half gone.

## What was ruled out

- **The region tables have not diverged** — both machines report 185 mappings.
- **`futex_wake` does clear the wait flag**, so a woken parent would proceed.
- **Load width is correct.** `ldr x` and `ldr w` from the same address differ exactly as qemu says
  they should, which is the case that would produce this truncation. `tests/guest/ldrsize.s`
  keeps it that way.
- **The thread is not stuck** — it advances normally; an earlier reading that said otherwise was
  sampled at `run_proc`'s entry, where consecutive calls each took one step.

## What is left

Finding the instruction that drops the top thirty-two bits. It is not the load path, and it is not
the clone entry point (which was wrong, and is fixed). The next thing to try is a narrow one: log
every instruction that writes a general register with a 32-bit form while the value is later used
as an address — expensive, but bounded to a thread's first thousand steps rather than a whole run.

## Why stop here

Eleven rounds, four of which produced a false conclusion of my own making: an inherited instruction
counter read as a spinning thread, a pc sampled at the wrong point read as a stuck one, a jump
detector that scanned every mapping per instruction and cost half the performance while never
firing, and an `sigsuspend` that belonged to a shell wrapper rather than the harness.

The default is honest and safe — `RuntimeError: can't start new thread`, which a program can catch
and degrade from — and every regression suite passes. Continuing to spend rounds on this while
calling the result progress would be the dishonest option.
