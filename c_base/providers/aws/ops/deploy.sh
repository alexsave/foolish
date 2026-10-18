#!/bin/bash
# Deploys c_base to AWS from this Mac with the AWS CLI: the first deploy and
# every later one, the same script. The aws provider's side of the deploy
# contract (docs/PLATFORM.md 8.5): it deploys a package, dist/<app>/aws/
# (providers/aws/ops/package.sh: bootstrap.zip, template.yaml, manifest), and
# refuses a replacement, refuses a secret regenerated against a live stack,
# refuses provisioned units past the account's free tier, waits, and probes
# /health, exiting nonzero on any of them.
#
#   aws configure            # or: aws sso login --profile <p>; export AWS_PROFILE=<p>
#   c_base/providers/aws/ops/deploy.sh [options] [<package dir> [<settings file>]]
#     with no package dir: package first (package.sh), then deploy dist/<app>/aws
#     <settings file>: instead of the ops settings file below
#   --import DIR   and then import a foolish/server/migrate/export.sh export into DynamoDB
#   --plan         make the change set, print it and run every check, and stop
#                  there: nothing is executed (the change set is left for a
#                  person to read and execute, or delete)
#   --keep-code    deploy the template with the stack's live CodeKey (no upload):
#                  a template-only change
#
# What it does, in order:
#   1. checks the CLI has credentials and a region (aws sts get-caller-identity);
#   2. reads the ops settings file (ops/ops_env.sh: $CBASE_OPS_ENV, else
#      ~/.config/cbase/ops.env, else ops/.env). For a stack that does not exist
#      yet it generates the missing secrets into it (openssl rand -hex 32):
#      CBASE_JWT_SECRET and CBASE_ADMIN_TOKEN. Later runs reuse them, so
#      sessions survive a redeploy. Those two go to SSM Parameter Store as
#      SecureStrings (<SecretsPrefix>jwt-secret, admin-token), written only
#      when missing or changed; the function reads them from there;
#   3. creates the code bucket if it is missing (private, public access blocked);
#   4. uploads the package's zip under a name carrying its sha256 (the
#      manifest's), so a new build is a new CodeKey and CloudFormation updates
#      the function's code;
#   5. refuses provisioned units past the free tier, ACCOUNT-WIDE: every other
#      table in the region (every other product's stack) plus this template's
#      table and indexes must stay within 25 RCU and 25 WCU
#      (providers/aws/ops/free_tier_units.jq; CBASE_FREE_UNITS to set another
#      ceiling);
#   6. a change set of the package's template.yaml, printed and refused when it
#      replaces a resource (CBASE_ALLOW_REPLACE=1 to mean it), then executed
#      and waited for (with --plan, not executed: the run ends here); prints
#      the stack's outputs (FunctionUrl, DistributionDomain, TableName,
#      PublicUrl) and probes <PublicUrl>/health;
#   7. seeds the 49 bots into a table that has none (cbase-import --seed-bots),
#      moves rating rows to the leaderboard's index key (--reindex-ratings),
#      and with --import DIR runs build/cbase-import against the stack's table,
#      both with temporary credentials exported from the CLI.
#
#      Against an existing stack it never generates one: a missing secret
#      stops the run, and a JWT secret that differs from the live function's
#      stops it too (either would sign every user out).
# Settings, from the environment or the settings file (the environment wins);
# the defaults marked (manifest) are the app's descriptor's (cbase/app.h cb_deploy):
#   CBASE_CERT_ARN         required: the ACM certificate for the domain, in
#                          us-east-1 (CloudFront reads certificates only there)
#   CBASE_DOMAIN           the manifest's domain
#   CBASE_STACK            the manifest's stack (the CloudFormation stack name)
#   CBASE_FUNCTION_NAME    the manifest's stack
#   CBASE_TABLE            the manifest's stack
#   CBASE_CODE_BUCKET      cbase-code-<account>-<region>
#   CBASE_ALLOWED_ORIGINS  *
#   CBASE_ORIGIN_ENFORCED  yes (no: send the origin header without checking it,
#                          for the first deploy of a new or rotated secret)
#   CBASE_ALARM_EMAIL      where the alarms are emailed ("" for nobody)
#   CBASE_IOT              yes: realtime over AWS IoT Core too (the account's
#                          iot:Data-ATS endpoint is read and passed as
#                          IotEndpoint); no: none of it
#
# The JWT secret and the admin token travel to SSM through a 0600 file; the
# origin secret travels to CloudFormation as a NoEcho parameter, so it appears
# in this Mac's process list while the deploy runs (CloudFront holds it in the
# distribution anyway). Nothing else ever prints them.
set -euo pipefail

here="$(cd "$(dirname "$0")/../../.." && pwd)"   # c_base/
# shellcheck source=c_base/ops/ops_env.sh
. "$here/ops/ops_env.sh"
import_dir=""
plan=no
keep_code=no
dist=""
settings=""

usage() {
  sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
}
while [ $# -gt 0 ]; do
  case "$1" in
    --import) [ $# -ge 2 ] || usage; import_dir="$2"; shift 2 ;;
    --plan) plan=yes; shift ;;
    --keep-code) keep_code=yes; shift ;;
    -h|--help) usage ;;
    -*) echo "deploy.sh: unknown argument $1" >&2; usage ;;
    *)
      if [ -z "$dist" ]; then dist="$1"
      elif [ -z "$settings" ]; then settings="$1"
      else echo "deploy.sh: one package dir and one settings file at most" >&2; usage; fi
      shift ;;
  esac
done
[ -z "$settings" ] || export CBASE_OPS_ENV="$settings"
env_file="$(cbase_ops_env_file "$here")"

die() { echo "deploy.sh: $*" >&2; exit 1; }
for tool in aws openssl unzip jq curl; do
  command -v "$tool" >/dev/null 2>&1 || die "$tool is not installed"
done

# ---- 0. the package ---------------------------------------------------------------
if [ -z "$dist" ]; then
  "$here/providers/aws/ops/package.sh"
  dist="$here/dist/$(make -s -C "$here" print-app)/aws"
fi
dist="$(cd "$dist" && pwd)"
for f in bootstrap.zip template.yaml manifest; do
  [ -f "$dist/$f" ] || die "$dist is not a package: no $f (providers/aws/ops/package.sh makes one)"
done
# The manifest's fixed name=value lines (provider.mk package-aws).
manifest() { sed -n "s/^$1=//p" "$dist/manifest" | head -1; }
[ "$(manifest provider)" = aws ] || die "$dist/manifest is not an aws package"
sum="$(manifest sha256)"
if command -v sha256sum >/dev/null 2>&1; then zip_sum="$(sha256sum "$dist/bootstrap.zip" | awk '{print $1}')"
else zip_sum="$(shasum -a 256 "$dist/bootstrap.zip" | awk '{print $1}')"; fi
[ -n "$sum" ] && [ "$sum" = "$zip_sum" ] || die "$dist/bootstrap.zip is not the zip its manifest names"
echo "package $dist: $(manifest app) at $(manifest commit), zip $sum"

# ---- 1. credentials and region ----------------------------------------------------
account="$(aws sts get-caller-identity --query Account --output text 2>/dev/null)" \
  || die "the AWS CLI has no working credentials: run 'aws configure' (or 'aws sso login' and export AWS_PROFILE)"
region="${AWS_REGION:-${AWS_DEFAULT_REGION:-$(aws configure get region 2>/dev/null || true)}}"
[ -n "$region" ] || die "no region: run 'aws configure set region us-east-1' (or export AWS_REGION)"
echo "account $account, region $region"

# ---- 2. the settings file and the secrets -----------------------------------------
cbase_ops_read "$env_file"
echo "settings: $env_file"
stack="${CBASE_STACK:-$(manifest stack)}"
[ -n "$stack" ] || die "no stack name (the manifest has none and CBASE_STACK is unset)"
stack_exists=no
if aws cloudformation describe-stacks --region "$region" --stack-name "$stack" >/dev/null 2>&1; then
  stack_exists=yes
fi
ensure_secret() { cbase_ops_secret "$1" "$env_file" "$stack_exists" || exit 1; }
ensure_secret CBASE_JWT_SECRET
ensure_secret CBASE_ADMIN_TOKEN
# The origin secret (the template's OriginSecret) is new to a stack made before it:
# generating it there is safe as long as the same deploy only sends it and does
# not yet check it (CBASE_ORIGIN_ENFORCED=no; RUNBOOK "Closing the origin").
origin_live=no
if [ "$stack_exists" = yes ] && aws cloudformation describe-stacks --region "$region" --stack-name "$stack" \
     --query "Stacks[0].Parameters[?ParameterKey=='OriginSecret'].ParameterKey" --output text 2>/dev/null | grep -q OriginSecret; then
  origin_live=yes
fi
cbase_ops_secret CBASE_ORIGIN_SECRET "$env_file" "$origin_live" || exit 1
if [ "$origin_live" = no ] && [ "${CBASE_ORIGIN_ENFORCED:-yes}" = yes ] && [ "$stack_exists" = yes ]; then
  die "the stack does not send X-Cbase-Origin yet: deploy once with CBASE_ORIGIN_ENFORCED=no, wait for the distribution to deploy, then again"
fi
secrets_prefix="${CBASE_SECRETS_PREFIX:-/$stack/}"
# The live JWT secret: SSM's, else (a stack from before SSM) the function's environment.
ssm_value() {
  aws ssm get-parameter --region "$region" --name "$1" --with-decryption --query Parameter.Value --output text 2>/dev/null || true
}
if [ "$stack_exists" = yes ]; then
  live_jwt="$(ssm_value "${secrets_prefix}jwt-secret")"
  if [ -z "$live_jwt" ] || [ "$live_jwt" = None ]; then
    live_jwt="$(aws lambda get-function-configuration --region "$region" --function-name "${CBASE_FUNCTION_NAME:-$stack}" \
      --query 'Environment.Variables.CBASE_JWT_SECRET' --output text 2>/dev/null || true)"
  fi
  if [ -n "$live_jwt" ] && [ "$live_jwt" != None ] && [ "$live_jwt" != "$CBASE_JWT_SECRET" ]; then
    die "CBASE_JWT_SECRET in $env_file is not the live one: deploying it would sign every user out (RUNBOOK, \"The ops settings file\")"
  fi
fi
# The two secrets into SSM, as SecureStrings, before the stack points the
# function at them. Through a 0600 file, so no value is on a command line.
put_secret() {
  local name="$1" value="$2" current tmp
  current="$(ssm_value "$name")"
  [ "$current" = "$value" ] && return 0
  tmp="$(umask 077; mktemp)"
  jq -n --arg n "$name" --arg v "$value" '{Name: $n, Value: $v, Type: "SecureString", Overwrite: true, Tier: "Standard"}' > "$tmp"
  aws ssm put-parameter --region "$region" --cli-input-json "file://$tmp" >/dev/null || { rm -f "$tmp"; die "could not write $name"; }
  rm -f "$tmp"
  echo "wrote $name (SecureString)"
}
put_secret "${secrets_prefix}jwt-secret" "$CBASE_JWT_SECRET"
put_secret "${secrets_prefix}admin-token" "$CBASE_ADMIN_TOKEN"

domain="${CBASE_DOMAIN:-$(manifest domain)}"
function_name="${CBASE_FUNCTION_NAME:-$stack}"
table="${CBASE_TABLE:-$stack}"
bucket="${CBASE_CODE_BUCKET:-cbase-code-$account-$region}"
origins="${CBASE_ALLOWED_ORIGINS:-*}"
cert="${CBASE_CERT_ARN:-}"
if [ -z "$cert" ]; then
  cat >&2 <<MSG
deploy.sh: CBASE_CERT_ARN is not set. CloudFront needs an ACM certificate for
$domain in us-east-1. Request one, add the DNS record it asks for, wait for
it to be ISSUED, then put its ARN in $env_file as CBASE_CERT_ARN=...:

  aws acm request-certificate --region us-east-1 --domain-name $domain --validation-method DNS
  aws acm describe-certificate --region us-east-1 --certificate-arn <arn> \\
      --query 'Certificate.[Status,DomainValidationOptions[0].ResourceRecord]'
MSG
  exit 1
fi
case "$cert" in
  arn:aws:acm:us-east-1:*) ;;
  *) die "CBASE_CERT_ARN must be an ACM certificate in us-east-1 (CloudFront's rule), got $cert" ;;
esac

# ---- 3. the code bucket -----------------------------------------------------------
if ! aws s3api head-bucket --bucket "$bucket" >/dev/null 2>&1; then
  echo "creating s3://$bucket"
  if [ "$region" = us-east-1 ]; then
    aws s3api create-bucket --bucket "$bucket" --region "$region" >/dev/null
  else
    aws s3api create-bucket --bucket "$bucket" --region "$region" \
      --create-bucket-configuration "LocationConstraint=$region" >/dev/null
  fi
  aws s3api put-public-access-block --bucket "$bucket" --public-access-block-configuration \
    BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true
fi

# ---- 4. upload the zip ------------------------------------------------------------
if [ "$keep_code" = yes ]; then
  [ "$stack_exists" = yes ] || die "--keep-code needs a live stack"
  key="$(aws cloudformation describe-stacks --region "$region" --stack-name "$stack" \
    --query "Stacks[0].Parameters[?ParameterKey=='CodeKey'].ParameterValue" --output text)"
  [ -n "$key" ] && [ "$key" != None ] || die "the $stack stack has no CodeKey to keep"
  echo "keeping the live code: $key"
else
  key="cbase-lambda-${sum:0:16}.zip"
  aws s3 cp "$dist/bootstrap.zip" "s3://$bucket/$key" --only-show-errors
  echo "uploaded s3://$bucket/$key"
fi

# ---- 5. the free tier, account-wide -----------------------------------------------
# Provisioned units are the free tier's only currency (25 RCU and 25 WCU,
# shared by every table and index of the account in the region), so this
# stack's table may have only what the account's other tables leave.
units_read="$(manifest units_read)"
units_write="$(manifest units_write)"
free="${CBASE_FREE_UNITS:-25}"
others="$(
  for t in $(aws dynamodb list-tables --region "$region" --query 'TableNames[]' --output text); do
    [ "$t" = "$table" ] && continue
    aws dynamodb describe-table --region "$region" --table-name "$t" --output json
  done | jq -s -r -f "$here/providers/aws/ops/free_tier_units.jq"
)" || die "could not read the account's other tables"
read -r others_read others_write <<<"$others"
echo "provisioned units: this stack ${units_read} RCU ${units_write} WCU, the account's other tables ${others_read} RCU ${others_write} WCU, free ${free}"
if [ $((units_read + others_read)) -gt "$free" ] || [ $((units_write + others_write)) -gt "$free" ]; then
  die "the account would provision $((units_read + others_read)) RCU and $((units_write + others_write)) WCU, past the free $free (DESIGN.md \"The free tier, provisioned\")"
fi

# ---- 6. the stack -----------------------------------------------------------------
# The distribution's own id goes back in as a parameter (CBASE_CF_DISTRIBUTION,
# the edge cache's invalidations; the template's DistributionId says why it cannot
# be a reference). Empty for a stack that has none yet.
dist_id=""
if [ "$stack_exists" = yes ]; then
  dist_id="$(aws cloudformation describe-stack-resource --region "$region" --stack-name "$stack" \
    --logical-resource-id Distribution --query StackResourceDetail.PhysicalResourceId --output text 2>/dev/null || true)"
  [ "$dist_id" = None ] && dist_id=""
fi
echo "distribution: ${dist_id:-(none yet)}"

# Realtime over IoT Core (the template's IotEndpoint): the account's data endpoint
# in this region, which only the IoT API names (CloudFormation cannot).
iot_endpoint=""
if [ "${CBASE_IOT:-yes}" = yes ]; then
  iot_endpoint="$(aws iot describe-endpoint --region "$region" --endpoint-type iot:Data-ATS \
    --query endpointAddress --output text)" || die "could not read the IoT Core endpoint (CBASE_IOT=no to deploy without it)"
fi
echo "iot endpoint: ${iot_endpoint:-(none)}"

# The change set is made, read, and only then executed: a change that would
# REPLACE a resource (the table, the distribution, the domain's alias) stops
# the run unless CBASE_ALLOW_REPLACE=1. CloudFormation's own deploy would
# execute it unseen.
aws cloudformation deploy \
  --region "$region" \
  --stack-name "$stack" \
  --template-file "$dist/template.yaml" \
  --s3-bucket "$bucket" --s3-prefix templates \
  --capabilities CAPABILITY_IAM CAPABILITY_NAMED_IAM \
  --no-fail-on-empty-changeset \
  --no-execute-changeset \
  --parameter-overrides \
    "CodeBucket=$bucket" \
    "CodeKey=$key" \
    "SecretsPrefix=$secrets_prefix" \
    "OriginSecret=$CBASE_ORIGIN_SECRET" \
    "OriginEnforced=${CBASE_ORIGIN_ENFORCED:-yes}" \
    "AlarmEmail=${CBASE_ALARM_EMAIL:-}" \
    "DomainName=$domain" \
    "CertificateArn=$cert" \
    "AllowedOrigins=$origins" \
    "FunctionName=$function_name" \
    "TableName=$table" \
    "DistributionId=$dist_id" \
    "IotEndpoint=$iot_endpoint" | tee "$here/build/deploy-changeset.txt"
changeset="$(grep -o 'arn:aws:cloudformation:[^ ]*changeSet/[^ ]*' "$here/build/deploy-changeset.txt" | head -1 || true)"
if [ -n "$changeset" ]; then
  aws cloudformation describe-change-set --region "$region" --change-set-name "$changeset" \
    --query 'Changes[].ResourceChange.[Action,LogicalResourceId,ResourceType,Replacement]' --output table
  # What may be recreated: providers/aws/ops/changeset_replacements.jq (a "Conditional" on
  # an attribute of a resource that is only modified is not).
  aws cloudformation describe-change-set --region "$region" --change-set-name "$changeset" --output json \
    > "$here/build/deploy-changeset.json"
  jq -r '.Changes[].ResourceChange | select(.Replacement == "Conditional")
         | "  conditional: \(.LogicalResourceId) <- \([.Details[] | select(.Target.RequiresRecreation != "Never")
           | "\(.ChangeSource) \(.CausingEntity // .Target.Name)"] | join(", "))"' "$here/build/deploy-changeset.json"
  replacing="$(jq -r -f "$here/providers/aws/ops/changeset_replacements.jq" "$here/build/deploy-changeset.json")"
  if [ -n "$replacing" ] && [ "$replacing" != None ] && [ "${CBASE_ALLOW_REPLACE:-}" != 1 ]; then
    aws cloudformation delete-change-set --region "$region" --change-set-name "$changeset" >/dev/null || true
    die "the change set replaces: $replacing. Read it, then rerun with CBASE_ALLOW_REPLACE=1 if that is meant"
  fi
  if [ "$plan" = yes ]; then
    echo "--plan: change set $changeset is made and NOT executed (aws cloudformation execute-change-set --change-set-name <it>, or delete-change-set)"
    exit 0
  fi
  aws cloudformation execute-change-set --region "$region" --change-set-name "$changeset"
  if [ "$stack_exists" = yes ]; then
    aws cloudformation wait stack-update-complete --region "$region" --stack-name "$stack"
  else
    aws cloudformation wait stack-create-complete --region "$region" --stack-name "$stack"
  fi
  echo "stack $stack updated"
else
  echo "no changes to the stack"
  if [ "$plan" = yes ]; then echo "--plan: nothing to execute"; exit 0; fi
fi

# ---- 7. what the stack made, and is it live -----------------------------------------
output() {
  aws cloudformation describe-stacks --region "$region" --stack-name "$stack" \
    --query "Stacks[0].Outputs[?OutputKey=='$1'].OutputValue" --output text
}
aws cloudformation describe-stacks --region "$region" --stack-name "$stack" \
  --query 'Stacks[0].Outputs[].[OutputKey,OutputValue]' --output table
cat <<MSG

Next (RUNBOOK.md): point the DNS CNAME for $domain at $(output DistributionDomain),
then build the web app with NEXT_PUBLIC_BACKEND_URL=$(output PublicUrl).
MSG
# /health through the domain, as a client reaches it (the Function URL itself
# refuses a request without the edge's origin header). A first deploy has no
# DNS for the domain yet, so there it is a warning.
healthy=no
url="$(output PublicUrl)"
for i in 1 2 3 4 5 6; do
  body="$(curl -fsS --max-time 20 "$url/health" 2>/dev/null || true)"
  case "$body" in "ok cbase "*) echo "$url/health: $body"; healthy=yes; break ;; esac
  echo "attempt $i: [$body]"
  sleep 5
done
if [ "$healthy" = no ] && [ "$stack_exists" = no ]; then
  echo "deploy.sh: $url/health does not answer yet; a new stack's domain needs its DNS first" >&2
  healthy=yes
fi
[ "$healthy" = yes ] || die "$url/health did not answer ok cbase"

# ---- 8. the bots, and the import ----------------------------------------------------
# A fresh table has no bots (nothing else seeds DynamoDB; SQLite seeds them at
# open), so every deploy seeds the schema's 49 into a table that has none and
# leaves a table with bots alone. The import then replaces them with the
# hosted rows, as it does on SQLite.
make -s -C "$here" build/cbase-import
stack_table="$(output TableName)"
(
  # Temporary credentials for this subshell only (SSO and assumed roles included).
  eval "$(aws configure export-credentials --format env)"
  export AWS_REGION="$region"
  # The import tool invalidates what it changed at the edge (app/edge.h).
  export CBASE_CF_DISTRIBUTION="$(output DistributionId)"
  CBASE_STORE=dynamo CBASE_TABLE="$stack_table" "$here/build/cbase-import" --seed-bots
  # The leaderboard's sparse, rating-ordered index key: rows from before it
  # move once; afterwards this rewrites nothing.
  CBASE_STORE=dynamo CBASE_TABLE="$stack_table" "$here/build/cbase-import" --reindex-ratings
  if [ -n "$import_dir" ]; then
    [ -d "$import_dir" ] || die "--import: $import_dir is not a directory"
    echo "importing $import_dir into DynamoDB table $stack_table"
    CBASE_STORE=dynamo CBASE_TABLE="$stack_table" "$here/build/cbase-import" "$import_dir"
  fi
)
if [ -n "$import_dir" ]; then
  echo "import done. The export holds password hashes: rm -rf $import_dir"
fi
