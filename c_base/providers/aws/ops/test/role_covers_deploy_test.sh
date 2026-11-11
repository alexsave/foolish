#!/bin/bash
# providers/aws/ops/role_covers_deploy.sh against a small tool, role and conf: a covered tool
# passes, an action the role lacks fails, a call the check does not know fails, a call CI never
# makes needs no action, and an app whose names the role lacks fails; then the real cbase
# submodule, role and confs pass.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
fail() { echo "role_covers_deploy_test: $*" >&2; exit 1; }
check() { "$here/role_covers_deploy.sh" "$t/tools" "$t/role.yaml" "$t/app.conf"; }
mkdir -p "$t/tools/cbase.d"
cat > "$t/tools/cbase" <<'S'
#!/bin/bash
# aws_call iam delete-role is in a comment: not a call
AWS_ACCOUNT="$(aws_call sts get-caller-identity --query Account --output text)"
S
cat > "$t/tools/cbase.d/stack.sh" <<'S'
cfn() { aws_call cloudformation --region us-east-1 "$@"; }
cs() { cfn describe-change-set --stack-name x "$@"; }
st="$(cfn describe-stacks --stack-name x)"
cs --query Id --output text
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
# The role without describe-change-set (what cs, a cfn line, is): refused.
sed 's/, cloudformation:DescribeChangeSet//' "$t/role.yaml" > "$t/short.yaml"
if "$here/role_covers_deploy.sh" "$t/tools" "$t/short.yaml" "$t/app.conf" 2> /dev/null; then fail "a missing cloudformation:DescribeChangeSet passed"; fi
# A call CI never makes needs no action.
echo 'aws_call s3api create-bucket --bucket x' >> "$t/tools/cbase.d/stack.sh"
check > /dev/null || fail "a group-init call needed an action"
# A call the table does not know: refused.
echo 'aws_call lambda update-function-code --function-name x' >> "$t/tools/cbase.d/stack.sh"
if check 2> /dev/null; then fail "an unknown call passed"; fi
sed -i.bak '/lambda/d' "$t/tools/cbase.d/stack.sh"
# An app on a VM needs its server prefix and state file named.
echo 'CBASE_VM = demo' >> "$t/app.conf"
sed -i.bak '/^CBASE_VM = none$/d' "$t/app.conf"
if check 2> /dev/null; then fail "a VM app whose vm/demo/* the role does not name passed"; fi
printf '      Resource: arn:x:s3:::c/vm/demo/* arn:x:s3:::s/vm/demo.txt\n' >> "$t/role.yaml"
check > /dev/null || fail "a VM app the role names was refused"
"$here/role_covers_deploy.sh" > /dev/null || fail "the real cbase tool, role and confs do not agree"
echo "role_covers_deploy_test: ok"
