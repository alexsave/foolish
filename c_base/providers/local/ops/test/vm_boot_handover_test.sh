#!/bin/bash
# vm_boot_handover_test.sh - vm_boot.sh's step 2c against a temp root: on an instance where
# ./cbase deploy installed an app's socket unit, the boot ends before step 3 and so never
# reinstalls or restarts cbase.service; on one without (a new instance), it goes on to step 3.
# No AWS calls.
set -u
here="$(cd "$(dirname "$0")/.." && pwd)"
fails=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; fails=$((fails + 1)); fi; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
# The step, then a stand-in for step 3 that only says it ran.
# shellcheck disable=SC2016  # the step expands its own variables when it runs
{ printf 'set -eu\nR="${VM_ROOT:-}"\n'; sed -n '/^# 2c\./,/^# end 2c/p' "$here/vm_boot.sh"; echo 'echo "step 3 ran"'; } > "$tmp/step.sh"
check "the step was found" "$(grep -c '^# end 2c' "$tmp/step.sh")" "1"

export VM_ROOT="$tmp/root"
mkdir -p "$VM_ROOT/etc/systemd/system"
: > "$VM_ROOT/etc/systemd/system/cbase.service"
out="$(bash "$tmp/step.sh")"; rc=$?
check "a new instance (no app socket): step 3 runs" "$out" "step 3 ran"
check "a new instance: exit 0" "$rc" "0"

: > "$VM_ROOT/etc/systemd/system/cbase-foolish.socket"
out="$(bash "$tmp/step.sh")"; rc=$?
check "an app socket installed: the boot ends before step 3" "$(grep -c 'step 3 ran' <<< "$out")" "0"
check "an app socket installed: it names the socket" "$out" "cbase: cbase-foolish.socket holds the server: cbase.service, its port and its data left alone"
check "an app socket installed: exit 0" "$rc" "0"

# A unit whose name only looks alike is not an app's socket.
rm -f "$VM_ROOT/etc/systemd/system/cbase-foolish.socket"
: > "$VM_ROOT/etc/systemd/system/cbase.socket"
out="$(bash "$tmp/step.sh")"
check "cbase.socket is not an app's: step 3 runs" "$out" "step 3 ran"

if [ "$fails" -ne 0 ]; then echo "vm_boot_handover_test: $fails failed"; exit 1; fi
echo "vm_boot_handover_test: all passed"
