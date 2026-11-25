#!/bin/bash
# down.sh : terminate the boxes and delete the bucket, SG, role and profile named in state.env.
# Copy of the Graviton harness down.sh with one addition: every target's purpose tag is read and must be
# cbase-mem-test before it is printed as a target and before it is deleted. A target without the tag is skipped.
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
D=/Users/alex/Dev/foolish-bench-results/grav
source $D/state.env
TAG=cbase-mem-test
ok() { [ "$1" = "$TAG" ]; }
inst_tag() { aws ec2 describe-tags --filters Name=resource-id,Values=$1 Name=key,Values=purpose --query 'Tags[].Value' --output text; }

echo "== targets from state.env: SRV=$SRV LOAD=$LOAD SG=$SG ROLE=$ROLE BUCKET=$B"
TERM=""
for i in $SRV $LOAD; do
  t=$(inst_tag $i); if ok "$t"; then echo "terminate $i (purpose=$t)"; TERM="$TERM $i"; else echo "SKIP $i: purpose tag is '$t'"; fi
done
[ -n "$TERM" ] && { aws ec2 terminate-instances --instance-ids $TERM --query 'TerminatingInstances[].[InstanceId,CurrentState.Name]' --output text; aws ec2 wait instance-terminated --instance-ids $TERM; }
aws ec2 describe-instances --instance-ids $SRV $LOAD --query 'Reservations[].Instances[].[InstanceId,State.Name]' --output text

bt=$(aws s3api get-bucket-tagging --bucket $B --query "TagSet[?Key=='purpose'].Value" --output text 2>/dev/null)
if ok "$bt"; then
  echo "empty and delete bucket $B (purpose=$bt)"
  aws s3 rm --only-show-errors --recursive s3://$B/ && aws s3api delete-bucket --bucket $B && echo "bucket $B deleted"
else echo "SKIP bucket $B: purpose tag is '$bt'"; fi

st=$(aws ec2 describe-security-groups --group-ids $SG --query "SecurityGroups[].Tags[?Key=='purpose'].Value" --output text 2>/dev/null)
if ok "$st"; then
  echo "delete SG $SG (purpose=$st)"
  for i in 1 2 3 4 5 6; do aws ec2 delete-security-group --group-id $SG 2>/dev/null && { echo "sg $SG deleted"; break; }; sleep 5; done
else echo "SKIP SG $SG: purpose tag is '$st'"; fi

rt=$(aws iam list-role-tags --role-name $ROLE --query "Tags[?Key=='purpose'].Value" --output text 2>/dev/null)
if ok "$rt"; then
  echo "delete role and profile $ROLE (purpose=$rt)"
  aws iam remove-role-from-instance-profile --instance-profile-name $ROLE --role-name $ROLE
  aws iam delete-instance-profile --instance-profile-name $ROLE
  aws iam detach-role-policy --role-name $ROLE --policy-arn arn:aws:iam::aws:policy/AmazonSSMManagedInstanceCore
  aws iam delete-role-policy --role-name $ROLE --policy-name s3
  aws iam delete-role --role-name $ROLE && echo "role $ROLE deleted"
else echo "SKIP role $ROLE: purpose tag is '$rt'"; fi

echo "== leftovers (purpose=$TAG)"
aws ec2 describe-instances --filters Name=tag:purpose,Values=$TAG Name=instance-state-name,Values=pending,running,stopping,stopped --query 'Reservations[].Instances[].InstanceId' --output text | wc -w
aws ec2 describe-volumes --filters Name=tag:purpose,Values=$TAG --query 'Volumes[].VolumeId' --output text | wc -w
aws ec2 describe-security-groups --filters Name=tag:purpose,Values=$TAG --query 'SecurityGroups[].GroupId' --output text | wc -w
aws ec2 describe-instances --instance-ids i-REDACTED01 --query 'Reservations[].Instances[].State.Name' --output text
