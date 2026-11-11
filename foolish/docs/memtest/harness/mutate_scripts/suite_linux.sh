#!/bin/bash
# suite_linux.sh : runs inside the wrapup-ci container (ubuntu 24.04); the CI suite on a clone of /src at HEAD
# Each step's log goes to /out/<step>.log and its exit code to /out/status.txt
set -u
O=/out; S=$O/status.txt; : > $S
step() { local n=$1; shift; local t0=$(date +%s); ( cd /w 2>/dev/null || cd /; "$@" ) > $O/$n.log 2>&1; local rc=$?; echo "$n rc=$rc $(( $(date +%s)-t0 ))s" >> $S; }
export DEBIAN_FRONTEND=noninteractive
step apt bash -c 'apt-get update -q && apt-get install -y -q clang shellcheck liburing-dev build-essential libssl-dev libsqlite3-dev git python3-venv'
git config --global --add safe.directory "*"; rm -rf /w; git clone -q --no-checkout /Users/alex/Dev/foolish /w; cd /w; git checkout -q ${SHA:?}; git log --oneline -1 >> $S
J=$(getconf _NPROCESSORS_ONLN)
step build make -C c_base -j$J
step check make -C c_base -j$J check
step test_uring make -C c_base -j$J test-uring
step check_asan make -C c_base -j$J check-asan
step fuzz_replay make -C c_base -j$J fuzz-replay
step coverage_cbase make -C c_base -j$J coverage
step kernel_coverage make -C foolish/c coverage
step kernel_volume make -C foolish/c -j$J volume
step foolyard_build make -C foolyard -j$J
step foolyard_chaos env SEEDS="1 2 3" bash foolyard/tools/chaos_suite.sh
step fuzz_build make -C c_base -j$J fuzz
step fuzz_select bash -c ".github/scripts/fuzz_select.sh --base ${BASE:-400a65d09} --head HEAD"
step fuzz_select_test bash c_base/fuzz/test/fuzz_select_test.sh
for T in $(tr -d '[]",' < $O/fuzz_select.log | tr ' ' '\n' | grep -v '^$'); do
  case "$T" in
    client:*) t=${T#client:}; step "fuzz_${t}" bash -c "make -C c_base/client -j$J build/fuzz/$t && make -C c_base/client fuzz-smoke FUZZ_TIME=30" ;;
    product:*) t=${T#product:}; step "fuzz_p_${t}" bash -c "make -C shared/fuzz -j$J build/$t && make -C shared/fuzz smoke TARGETS=$t FUZZ_TIME=30" ;;
    *) step "fuzz_${T}" bash -c "make -C c_base -j$J build/fuzz/$T && make -C c_base fuzz-smoke FUZZ_TIME=30 FUZZ_TARGETS=$T" ;;
  esac
done
step lint_shellcheck bash -c "$(grep -o 'shellcheck -x .*' .github/workflows/c_base.yml | head -1)"
for t in c_base/ops/test/web_deploy_gate_test.sh c_base/ops/test/ops_env_test.sh c_base/ops/test/s3_prune_test.sh c_base/providers/local/ops/test/vm_boot_logs_test.sh c_base/providers/local/ops/test/vm_boot_stub_test.sh c_base/providers/local/ops/test/vm_release_boot_test.sh c_base/providers/aws/ops/test/changeset_gate_test.sh c_base/providers/aws/ops/test/role_covers_release_test.sh c_base/fuzz/test/fuzz_mk_test.sh; do step "lint_$(basename $t .sh)" bash $t; done
echo DONE >> $S
