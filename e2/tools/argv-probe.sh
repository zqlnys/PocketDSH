#!/bin/sh
# Run by the guest, not the host. Prints what it received so the caller can compare.
#
# A file rather than a one-liner because the quoting is most of the risk otherwise: four levels of
# shell (host script, harness, guest sh, and the argument itself) turns every case into an escape
# puzzle, and two of the first attempts tested the escaping rather than the argv.
printf 'argc=%s\n' "$#"
i=0
for a in "$@"; do
    printf 'arg%d: len=%d value=[%s]\n' "$i" "${#a}" "$a"
    i=$((i + 1))
done
