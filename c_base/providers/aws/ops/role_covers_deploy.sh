#!/bin/bash
# role_covers_deploy.sh [TOOL_DIR] [github_oidc.yaml] [CONF...]: the GitHub deploy role grants
# every AWS call `./cbase deploy` makes in CI.
#
# The role is least privilege, so a call the tool gains (a cbase pin bump) and the role lacks
# fails the deploy on main, after every test passed, half way through: the server shipped, the
# stack not updated. This reads every AWS call in the deploy tool (TOOL_DIR, default the cbase
# submodule's tools/cbase_cli, the Python program tools/cbase launches) with role_calls.py, which
# parses its modules and knows each form the tool makes a call in (aws.call, aws.callv, stack.py's
# _cfn and _cs, a direct cli.run of aws), and requires, for each, the IAM action(s) it needs in the
# role, or a reason CI never makes it. A call this table does not know stops the check, and so does
# a call role_calls.py cannot read: decide which it is, here and in the role.
#
# The prune (on by default in the tool) is a call CI never makes because c_base.yml passes
# --no-prune to every deploy; that is checked here too (the workflow is CBASE_DEPLOY_WORKFLOW,
# default .github/workflows/c_base.yml), so a deploy line that drops it fails this check rather
# than a live deploy half way, or deletes files a tab still open across two deploys needs
# (c_base/RUNBOOK.md: the prune is by hand).
#
# Two lists the tool's text cannot show are checked too:
#   HANDLER_ACTIONS, what CloudFormation calls as this role to detect drift on and update the
#     app stacks' resources (the read and update handlers of AWS::CloudFront::Distribution,
#     ::Function, ::OriginAccessControl and ::ResponseHeadersPolicy, from
#     `aws cloudformation describe-type`), less the ones for features the stacks do not use
#     (connection groups, VPC origins, staging distributions);
#   each CONF's names (default foolish/cbase.conf and uttt/cbase.conf, the apps c_base.yml
#     deploys): its stack cbase-app-<app>, its router, its folder in the web bucket, and for an
#     app on a VM its server prefix vm/<app>/ and its state file vm/<app>.txt. The role scopes
#     each action to those, so an app CI starts deploying must be named there too.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
tool="${1:-$root/cbase/tools/cbase_cli}"
role="${2:-$here/github_oidc.yaml}"
shift $(($# < 2 ? $# : 2))
confs=("$@")
[ "${#confs[@]}" -gt 0 ] || confs=("$root/foolish/cbase.conf" "$root/uttt/cbase.conf")

# The IAM action(s) a call needs, blank-separated; "-" for none; "ci-never: WHY" for a call a CI
# deploy of an existing app never makes (so the role must not need it).
action_of() {
  case "$1" in
    "sts get-caller-identity") echo - ;;
    "configure get") echo - ;;   # aws configure get region: the CLI's own settings, no AWS call
    # The two app stacks' change-set flow (stack.sh). Every stack waiter is DescribeStacks.
    "cloudformation describe-stacks"|"cloudformation wait") echo cloudformation:DescribeStacks ;;
    "cloudformation describe-stack-events") echo cloudformation:DescribeStackEvents ;;
    "cloudformation get-template") echo cloudformation:GetTemplate ;;
    "cloudformation detect-stack-drift") echo cloudformation:DetectStackDrift cloudformation:DetectStackResourceDrift cloudformation:BatchDescribeTypeConfigurations ;;
    "cloudformation describe-stack-drift-detection-status") echo cloudformation:DescribeStackDriftDetectionStatus ;;
    "cloudformation describe-stack-resource-drifts") echo cloudformation:DescribeStackResourceDrifts ;;
    "cloudformation create-change-set") echo cloudformation:CreateChangeSet cloudformation:TagResource ;;
    "cloudformation describe-change-set"|"cloudformation wait change-set-create-complete") echo cloudformation:DescribeChangeSet ;;
    "cloudformation delete-change-set") echo cloudformation:DeleteChangeSet ;;
    "cloudformation execute-change-set") echo cloudformation:ExecuteChangeSet ;;
    "cloudformation delete-stack") echo "ci-never: only after a first create's change set, or undeploy" ;;
    "cloudformation describe-stack-resources") echo cloudformation:DescribeStackResources ;;
    # The edge.
    "cloudfront list-distributions") echo cloudfront:ListDistributions ;;
    "cloudfront create-invalidation") echo cloudfront:CreateInvalidation ;;
    "cloudfront wait invalidation-completed") echo cloudfront:GetInvalidation ;;
    "cloudfront get-distribution") echo cloudfront:GetDistribution ;;
    "cloudfront associate-alias") echo "ci-never: a staged attach (--attach), by hand" ;;
    "acm describe-certificate") echo acm:DescribeCertificate ;;
    "acm list-certificates"|"acm request-certificate"|"acm wait certificate-validated") echo "ci-never: both confs name CBASE_CERT_ARN" ;;
    "route53 list-hosted-zones-by-name") echo route53:ListHostedZonesByName ;;
    "route53 change-resource-record-sets"|"route53 wait resource-record-sets-changed") echo "ci-never: no Route 53 zone in the account serves these hostnames" ;;
    # The server (vm.sh) and its group's state (state.sh).
    "ec2 describe-instances") echo ec2:DescribeInstances ;;
    "ec2 describe-instance-types") echo ec2:DescribeInstanceTypes ;;
    "autoscaling describe-auto-scaling-groups") echo autoscaling:DescribeAutoScalingGroups ;;
    "ssm get-parameter") echo ssm:GetParameter ;;
    "ssm send-command") echo ssm:SendCommand ;;
    "ssm get-command-invocation") echo ssm:GetCommandInvocation ;;
    "s3api head-object") echo s3:GetObject s3:ListBucket ;;
    "s3api head-bucket") echo s3:ListBucket ;;
    "s3api get-object") echo s3:GetObject ;;
    "s3api put-object") echo s3:PutObject ;;
    # The files (static.sh).
    "s3api get-bucket-location") echo s3:GetBucketLocation ;;
    "s3api get-bucket-policy") echo s3:GetBucketPolicy ;;
    "s3 cp") echo s3:PutObject ;;
    "s3api list-objects-v2"|"s3api delete-objects"|"s3api delete-object") echo "ci-never: c_base.yml deploys with --no-prune (checked below); undeploy and group-destroy" ;;
    # group-init and group-destroy, and the tag lookups of groups the confs do not adopt by name.
    "s3api create-bucket"|"s3api put-public-access-block"|"s3api put-bucket-versioning"|"s3api delete-bucket-tagging") echo "ci-never: group-init and group-destroy" ;;
    "ec2 describe-managed-prefix-lists"|"ec2 describe-instance-type-offerings") echo "ci-never: group-init" ;;
    "resourcegroupstaggingapi get-resources") echo "ci-never: both confs adopt their groups by name (CBASE_VM_STACK, CBASE_STATIC_BUCKET)" ;;
    *) return 1 ;;
  esac
}

HANDLER_ACTIONS="cloudfront:GetDistribution cloudfront:GetDistributionConfig cloudfront:UpdateDistribution
cloudfront:ListTagsForResource cloudfront:TagResource cloudfront:UntagResource
cloudfront:DescribeFunction cloudfront:GetFunction cloudfront:UpdateFunction cloudfront:PublishFunction
cloudfront:GetOriginAccessControl cloudfront:UpdateOriginAccessControl
cloudfront:GetResponseHeadersPolicy cloudfront:UpdateResponseHeadersPolicy"

[ -f "$tool/aws.py" ] || { echo "role_covers_deploy: no deploy tool at $tool/aws.py (git submodule update --init cbase)" >&2; exit 1; }
# Every call, as "SERVICE COMMAND" ("SERVICE wait WAITER" for a waiter); "UNRESOLVED file:line" for a
# call role_calls.py cannot read.
calls="$(python3 -I -B "$here/role_calls.py" "$tool")" || { echo "role_covers_deploy: role_calls.py could not read $tool" >&2; exit 1; }
[ -n "$calls" ] || { echo "role_covers_deploy: no AWS call in $tool" >&2; exit 1; }

# The role's text, comments left out: an action is granted when it is a word of a line.
granted="$(grep -v '^[[:space:]]*#' "$role")"
has() { grep -qE "(^|[^A-Za-z0-9:])$1([^A-Za-z0-9]|$)" <<<"$granted"; }

bad=0
n=0
while IFS= read -r c; do
  case "$c" in UNRESOLVED\ *)
    echo "role_covers_deploy: the AWS call at ${c#UNRESOLVED } in $tool is in a form role_calls.py cannot read: teach it the form" >&2
    bad=1; continue ;;
  esac
  if ! actions="$(action_of "$c")"; then
    echo "role_covers_deploy: \`$c\` in $tool is a call this check does not know: add it here (its action, or why CI never makes it) and to $role" >&2
    bad=1; continue
  fi
  n=$((n + 1))
  case "$actions" in -|ci-never:*) continue ;; esac
  for a in $actions; do
    has "$a" || { echo "role_covers_deploy: the deploy role lacks $a, which \`$c\` needs" >&2; bad=1; }
  done
done <<<"$calls"
for a in $HANDLER_ACTIONS; do
  has "$a" || { echo "role_covers_deploy: the deploy role lacks $a, which CloudFormation needs to apply a change set" >&2; bad=1; }
done

# Every deploy the workflow runs passes --no-prune, so the prune's calls above are ones CI never makes.
wf="${CBASE_DEPLOY_WORKFLOW:-$root/.github/workflows/c_base.yml}"
deploys="$(grep -v '^[[:space:]]*#' "$wf" | grep -oE 'cbase/tools/cbase deploy[^;|&]*' || true)"
[ -n "$deploys" ] || { echo "role_covers_deploy: $wf runs no cbase/tools/cbase deploy" >&2; bad=1; }
while IFS= read -r d; do
  [ -z "$d" ] || grep -qE -- '(^|[[:space:]])--no-prune([[:space:]]|$)' <<<"$d" || {
    echo "role_covers_deploy: \`$d\` in $wf prunes (the tool's default): pass --no-prune, or grant the prune's calls and say so here" >&2
    bad=1
  }
done <<<"$deploys"

# conf_value FILE KEY: the first value of KEY in a cbase.conf ("KEY = value").
conf_value() { sed -nE "s/^$2[[:space:]]*=[[:space:]]*//p" "$1" | head -1; }
for conf in "${confs[@]}"; do
  app="$(conf_value "$conf" CBASE_APP)"
  [ -n "$app" ] || { echo "role_covers_deploy: $conf names no CBASE_APP" >&2; bad=1; continue; }
  prefix="$(conf_value "$conf" CBASE_STATIC_PREFIX)"
  names=("stack/cbase-app-$app/" "function/cbase-$app-router" "/${prefix:-$app}/*")
  vm="$(conf_value "$conf" CBASE_VM)"
  [ -z "$vm" ] || [ "$vm" = none ] || names+=("/vm/$app/*" "/vm/$app.txt")
  for nm in "${names[@]}"; do
    grep -qF -- "$nm" <<<"$granted" || { echo "role_covers_deploy: the deploy role does not name $nm, which $app's deploy ($conf) uses" >&2; bad=1; }
  done
done
[ "$bad" = 0 ] && echo "role_covers_deploy: ok ($n calls, ${#confs[@]} apps)"
exit "$bad"
