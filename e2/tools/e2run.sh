#!/bin/sh
# Run the E2 harness and print its output so the status line cannot be lost.
#
# Written because the same mistake has been made repeatedly when testing by hand: pipe the
# harness through `grep -v '^\[exit'` to strip the status line, then conclude from the
# remaining bytes. That line is where the fault reason lives - "FAULT reason=instruction
# cap", "cannot load", a syscall error - so filtering it out makes a reported failure look
# like silence.
#
# Program output goes to stdout exactly as the guest wrote it. Everything else - the exit
# status, instruction count and fault reason - goes to stderr, prefixed, so a plain
# `>/dev/null` on stdout cannot hide it either.
#
#   tools/e2run.sh BUSYBOX [args...]
#   tools/e2run.sh /tmp/alpine-root/usr/bin/basename --help
set -u
here=$(dirname "$0")
rune="$here/rune"
[ -x "$rune" ] || { echo "e2run: $rune missing - run make in e2/ first" >&2; exit 2; }

out=$("$rune" "$@" 2>&1)
status=$?

# Mark the status lines in place rather than splitting the stream: the harness can emit
# several bracketed sections ("[stderr]" among them), and cutting at the first marker
# separated the wrong halves.
printf '%s\n' "$out" | sed 's/^\(\[exit \|\[stderr\]\)/[status] \1/' >&2

exit $status
