#!/bin/bash
# down.sh : terminate the boxes and delete bucket, SG, role and profile from state.env
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
source /Users/alex/Dev/cdom-memtest/state.env
aws ec2 terminate-instances --instance-ids $SRV $LOAD --query 'TerminatingInstances[].[InstanceId,CurrentState.Name]' --output text
aws ec2 wait instance-terminated --instance-ids $SRV $LOAD
aws ec2 describe-instances --instance-ids $SRV $LOAD --query 'Reservations[].Instances[].[InstanceId,State.Name]' --output text
aws s3 rm --only-show-errors --recursive s3://$B/ && aws s3api delete-bucket --bucket $B && echo "bucket $B deleted"
for i in 1 2 3 4 5 6; do aws ec2 delete-security-group --group-id $SG 2>/dev/null && { echo "sg $SG deleted"; break; }; sleep 5; done
aws iam remove-role-from-instance-profile --instance-profile-name $ROLE --role-name $ROLE
aws iam delete-instance-profile --instance-profile-name $ROLE
aws iam detach-role-policy --role-name $ROLE --policy-arn arn:aws:iam::aws:policy/AmazonSSMManagedInstanceCore
aws iam delete-role-policy --role-name $ROLE --policy-name s3
aws iam delete-role --role-name $ROLE && echo "role $ROLE deleted"
aws ec2 describe-instances --filters Name=tag:purpose,Values=cbase-mem-test Name=instance-state-name,Values=pending,running,stopping,stopped --query 'Reservations[].Instances[].InstanceId' --output text | wc -w
aws ec2 describe-volumes --filters Name=tag:purpose,Values=cbase-mem-test --query 'Volumes[].VolumeId' --output text | wc -w
aws ec2 describe-instances --instance-ids i-08f26ffb900294ee8 --query 'Reservations[].Instances[].State.Name' --output text
