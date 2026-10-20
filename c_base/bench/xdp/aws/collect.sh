#!/bin/bash
# collect.sh STATE : sync the run's results from its bucket into ../results_<run>/, show the master log tail and the
# state of the run's instances. Acts only on the bucket and instance ids recorded in STATE.
set -euo pipefail
X=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
STATE=${1:?usage: collect.sh state/<run>.env}
# shellcheck disable=SC1090
. "$STATE"
export AWS_PAGER= AWS_DEFAULT_REGION=$REGION
OUT=$X/results_$RUN
mkdir -p "$OUT"
aws s3 sync --only-show-errors "s3://$BUCKET/xdp/" "$OUT/" --exclude pkg.tgz
[ -f "$OUT/master.log" ] && tail -5 "$OUT/master.log"
aws ec2 describe-instances --instance-ids "$INSTANCE_srv" "$INSTANCE_load" --query 'Reservations[].Instances[].[InstanceId,State.Name]' --output text
echo "results: $OUT"
