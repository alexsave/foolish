#!/bin/bash
# suite_linux2.sh : the steps suite_linux.sh got wrong or that the container blocked, run again: foolyard at its real path
# (foolish/foolyard), the kernel volume with -k (every part, not only up to the first failure), the product fuzz target,
# the changeset gate test with jq, and c_base coverage with io_uring allowed (this container runs with seccomp unconfined).
set -u
O=/out2; S=$O/status.txt; mkdir -p $O; : > $S
step() { local n=$1; shift; local t0=$(date +%s); ( cd /w 2>/dev/null || cd /; "$@" ) > $O/$n.log 2>&1; local rc=$?; echo "$n rc=$rc $(( $(date +%s)-t0 ))s" >> $S; }
export DEBIAN_FRONTEND=noninteractive
step apt bash -c 'apt-get update -q && apt-get install -y -q clang jq shellcheck liburing-dev build-essential libssl-dev libsqlite3-dev git'
git config --global --add safe.directory "*"; rm -rf /w; git clone -q --no-checkout /Users/alex/Dev/foolish /w; cd /w; git checkout -q ${SHA:?}; git log --oneline -1 >> $S
J=$(getconf _NPROCESSORS_ONLN)
step foolyard_build make -C foolish/foolyard -j$J
step foolyard_chaos bash -c "cd foolish && SEEDS=\"1 2 3\" bash foolyard/tools/chaos_suite.sh"
step kernel_volume_k make -k -C foolish/c -j$J volume
step fuzz_p_foolish_wire bash -c "make -C shared/fuzz -j$J build/foolish_wire && make -C shared/fuzz smoke TARGETS=foolish_wire FUZZ_TIME=30"
step lint_changeset_gate_test bash c_base/providers/aws/ops/test/changeset_gate_test.sh
step coverage_cbase_uring make -C c_base -j$J coverage
step fuzz_build_k make -k -C c_base -j$J fuzz
echo DONE >> $S
