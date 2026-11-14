#!/bin/bash
# providers/aws/ops/role_covers_deploy.sh against a small Python tool, role, conf and workflow: a
# covered tool passes, an action the role lacks fails, a call the check does not know fails, a call
# in a form role_calls.py cannot read fails, a call CI never makes needs no action, a deploy line
# without --no-prune fails, and an app whose names the role lacks fails; then the real cbase
# submodule, role, confs and workflow pass.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
fail() { echo "role_covers_deploy_test: $*" >&2; exit 1; }
export CBASE_DEPLOY_WORKFLOW="$t/wf.yml"
check() { "$here/role_covers_deploy.sh" "$t/tools" "$t/role.yaml" "$t/app.conf"; }
refused() {   # refused WHAT GREP: the check fails, and says GREP
  if "$here/role_covers_deploy.sh" "$t/tools" "${ROLE:-$t/role.yaml}" "$t/app.conf" 2> "$t/err"; then fail "$1 passed"; fi
  grep -q -- "$2" "$t/err" || fail "$1 was refused without \"$2\": $(cat "$t/err")"
}
mkdir -p "$t/tools"
cat > "$t/tools/aws.py" <<'S'
# aws.call("iam", "delete-role") is in a comment: not a call
def call(*args):
    return exec_(None, args)
def init():
    rc, out = call("sts", "get-caller-identity", "--query", "Account", "--output", "text")
S
cat > "$t/tools/stack.py" <<'S'
import aws
def _cfn(*args):
    return aws.callv(["cloudformation", "--region", stk_region], *args)
def _cs(*args):
    return aws.callv(["cloudformation", "--region", stk_region, "describe-change-set"], *args)
def status():
    rc, out = _cfn("describe-stacks", "--stack-name", "x")
    rc, out = _cs("--query", "Id", "--output", "text")
S
cat > "$t/wf.yml" <<'S'
      # ../cbase/tools/cbase deploy in a comment needs nothing
      - run: ../cbase/tools/cbase deploy --no-prune
S
cat > "$t/app.conf" <<'S'
CBASE_APP = demo
CBASE_VM = none
S
{
  printf '      Action: [cloudformation:DescribeStacks, cloudformation:DescribeChangeSet]\n'
  printf '      Resource: arn:x:stack/cbase-app-demo/* arn:x:function/cbase-demo-router arn:x:s3:::b/demo/*\n'
  printf '      Action: [%s]\n' "$(sed -nE '/^HANDLER_ACTIONS="/,/"$/p' "$here/role_covers_deploy.sh" | tr -d '"' | sed 's/^HANDLER_ACTIONS=//' | tr '\n' ' ' | sed 's/ *$//; s/ /, /g')"
} > "$t/role.yaml"
check > /dev/null || fail "a covered tool was refused"
# The role without describe-change-set (what _cs is): refused, naming it.
sed 's/, cloudformation:DescribeChangeSet//' "$t/role.yaml" > "$t/short.yaml"
ROLE="$t/short.yaml" refused "a missing cloudformation:DescribeChangeSet" "lacks cloudformation:DescribeChangeSet"
# A call CI never makes needs no action.
echo '    aws.call("s3api", "create-bucket", "--bucket", "x")' >> "$t/tools/stack.py"
check > /dev/null || fail "a group-init call needed an action"
# A call the table does not know: refused, naming it.
cp "$t/tools/stack.py" "$t/stack.keep"
echo '    aws.call("lambda", "update-function-code", "--function-name", "x")' >> "$t/tools/stack.py"
refused "an unknown call" "lambda update-function-code"
cp "$t/stack.keep" "$t/tools/stack.py"
# A starred call read from its list literal: known when the list says what it is.
printf '    cw = ["execute-change-set", "--stack-name", "x"]\n    rc, o = _cfn(*cw)\n' >> "$t/tools/stack.py"
refused "an execute-change-set read from a list" "lacks cloudformation:ExecuteChangeSet"
cp "$t/stack.keep" "$t/tools/stack.py"
# A call whose words cannot be read: refused, naming where.
echo '    rc, o = aws.call(*words_from_elsewhere())' >> "$t/tools/stack.py"
refused "an unreadable call" "stack.py:[0-9]* in .* cannot read"
cp "$t/stack.keep" "$t/tools/stack.py"
# A direct run of the CLI is a call too.
echo '    cli.run(["aws", "kms", "decrypt"])' >> "$t/tools/stack.py"
refused "a direct aws run" "kms decrypt"
cp "$t/stack.keep" "$t/tools/stack.py"
# A deploy line that prunes (the tool's default): refused.
echo '      - run: ../cbase/tools/cbase deploy --dry-run' >> "$t/wf.yml"
refused "a deploy without --no-prune" "prunes (the tool's default)"
sed -i.bak '$d' "$t/wf.yml"
check > /dev/null || fail "the restored workflow was refused"
# An app on a VM needs its server prefix and state file named.
echo 'CBASE_VM = demo' >> "$t/app.conf"
sed -i.bak '/^CBASE_VM = none$/d' "$t/app.conf"
if check 2> /dev/null; then fail "a VM app whose vm/demo/* the role does not name passed"; fi
printf '      Resource: arn:x:s3:::c/vm/demo/* arn:x:s3:::s/vm/demo.txt\n' >> "$t/role.yaml"
check > /dev/null || fail "a VM app the role names was refused"
unset CBASE_DEPLOY_WORKFLOW
"$here/role_covers_deploy.sh" > /dev/null || fail "the real cbase tool, role, confs and workflow do not agree"
echo "role_covers_deploy_test: ok"
