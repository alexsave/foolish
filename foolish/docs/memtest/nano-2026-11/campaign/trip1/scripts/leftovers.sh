#!/bin/bash
# leftovers.sh: independent zero-leftover proof after grav/down.sh (describe/list only)
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
echo "== trip1 leftover proof $(date -u +%FT%TZ)"
echo "instances purpose=cbase-mem-test pending/running/stopping/stopped: $(aws ec2 describe-instances --filters Name=tag:purpose,Values=cbase-mem-test Name=instance-state-name,Values=pending,running,stopping,stopped --query 'Reservations[].Instances[].InstanceId' --output text | wc -w)"
echo "trip instances state: $(aws ec2 describe-instances --instance-ids i-REDACTED02 i-REDACTED03 --query 'Reservations[].Instances[].[InstanceId,State.Name]' --output text | tr '\n\t' '  ')"
echo "volumes purpose=cbase-mem-test: $(aws ec2 describe-volumes --filters Name=tag:purpose,Values=cbase-mem-test --query 'Volumes[].VolumeId' --output text | wc -w)"
echo "security groups purpose=cbase-mem-test: $(aws ec2 describe-security-groups --filters Name=tag:purpose,Values=cbase-mem-test --query 'SecurityGroups[].GroupId' --output text | wc -w)"
echo "IAM roles cbase-mem-test-*: $(aws iam list-roles --query "Roles[?starts_with(RoleName,'cbase-mem-test')].RoleName" --output text | wc -w)"
echo "instance profiles cbase-mem-test-*: $(aws iam list-instance-profiles --query "InstanceProfiles[?starts_with(InstanceProfileName,'cbase-mem-test')].InstanceProfileName" --output text | wc -w)"
echo "S3 buckets cbase-mem-test-*: $(aws s3api list-buckets --query "Buckets[?starts_with(Name,'cbase-mem-test')].Name" --output text | wc -w)"
echo "production i-REDACTED01: $(aws ec2 describe-instances --instance-ids i-REDACTED01 --query 'Reservations[].Instances[].State.Name' --output text)"
