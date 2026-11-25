#!/bin/bash
# up_trip1.sh: trip 1 launch, a copy of grav/out/e1/up_e1.sh that uploads campaign/trip1/pkg (adds prof.sh, trip.bt and the
# unstripped twin cbase_D.unstripped) and installs perf, bpftrace and strace on the server in user data. It still writes
# grav/state.env, so grav/down.sh tears it down unchanged. Original header:
# grav/up.sh : the Graviton confirmation (copied from ../up.sh): throwaway role/profile, SG, bucket, and two boxes (t4g.nano server, c7g.2xlarge load), all tagged purpose=cbase-mem-test owner=cdom
set -euo pipefail
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
D=/Users/alex/Dev/foolish-bench-results/grav; ST=$D/state.env; PK=/Users/alex/Dev/foolish-bench-results/campaign/trip1/pkg
echo "launch_start_utc=$(date -u +%FT%TZ)" >> /Users/alex/Dev/foolish-bench-results/campaign/trip1/out/launch.txt
if [ -n "${RESUME:-}" ]; then source $ST; TAGS=; SUBNET=subnet-REDACTED05; else : > $ST
TAGS='Key=purpose,Value=cbase-mem-test Key=owner,Value=cdom'
# The default VPC (us-east-1a, public IPv4), not the cbase-vm stack's subnet the morning run used: nothing of production.
SUBNET=subnet-REDACTED05; VPC=vpc-REDACTED06
SUF=$(openssl rand -hex 4); echo SUF=$SUF >> $ST
B=cbase-mem-test-$SUF; echo B=$B >> $ST
aws s3api create-bucket --bucket $B >/dev/null
aws s3api put-bucket-tagging --bucket $B --tagging 'TagSet=[{Key=purpose,Value=cbase-mem-test},{Key=owner,Value=cdom}]'
for f in src.tgz srv.sh load.sh cpu.sh prof.sh trip.bt rematch.c tlsres.c; do aws s3 cp --only-show-errors $PK/$f s3://$B/pkg/$f; done
for n in D; do aws s3 cp --only-show-errors $D/bin/cbase_$n s3://$B/bin/cbase_$n; done
aws s3 cp --only-show-errors $PK/cbase_D.unstripped s3://$B/bin/cbase_D.unstripped
ROLE=cbase-mem-test-role-$SUF; echo ROLE=$ROLE >> $ST
aws iam create-role --role-name $ROLE --tags Key=purpose,Value=cbase-mem-test Key=owner,Value=cdom --assume-role-policy-document '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"Service":"ec2.amazonaws.com"},"Action":"sts:AssumeRole"}]}' >/dev/null
aws iam attach-role-policy --role-name $ROLE --policy-arn arn:aws:iam::aws:policy/AmazonSSMManagedInstanceCore
aws iam put-role-policy --role-name $ROLE --policy-name s3 --policy-document "{\"Version\":\"2012-10-17\",\"Statement\":[{\"Effect\":\"Allow\",\"Action\":[\"s3:GetObject\",\"s3:PutObject\",\"s3:ListBucket\"],\"Resource\":[\"arn:aws:s3:::$B\",\"arn:aws:s3:::$B/*\"]}]}"
aws iam create-instance-profile --instance-profile-name $ROLE --tags Key=purpose,Value=cbase-mem-test Key=owner,Value=cdom >/dev/null
aws iam add-role-to-instance-profile --instance-profile-name $ROLE --role-name $ROLE
SG=$(aws ec2 create-security-group --group-name cbase-mem-test-sg-$SUF --description "cbase mem test, default egress + self 8080-8081" --vpc-id $VPC --tag-specifications "ResourceType=security-group,Tags=[{Key=purpose,Value=cbase-mem-test},{Key=owner,Value=cdom}]" --query GroupId --output text)
echo SG=$SG >> $ST
aws ec2 authorize-security-group-ingress --group-id $SG --ip-permissions "IpProtocol=tcp,FromPort=8080,ToPort=8081,UserIdGroupPairs=[{GroupId=$SG}]" >/dev/null
AMI=$(aws ssm get-parameter --name /aws/service/ami-amazon-linux-latest/al2023-ami-kernel-default-arm64 --query Parameter.Value --output text); echo AMI=$AMI >> $ST
sleep 8   # instance profile propagation
fi
for role in ${ROLES:-srv load}; do
  UD=$(mktemp)
  cat > $UD <<UDE
#!/bin/bash
shutdown -h +80
mkdir -p /opt/mt; echo $B > /opt/mt/bucket
E=https://s3.dualstack.us-east-1.amazonaws.com
for f in srv.sh load.sh cpu.sh prof.sh trip.bt src.tgz rematch.c tlsres.c; do aws s3 cp --quiet s3://$B/pkg/\$f /opt/mt/\$f --endpoint-url \$E; done
chmod +x /opt/mt/*.sh
if [ $role = load ]; then /opt/mt/load.sh build > /var/tmp/build_out.txt 2>&1; aws s3 cp --quiet /var/tmp/build_out.txt s3://$B/res/build_out.txt --endpoint-url \$E; fi
if [ $role = srv ]; then dnf install -y perf bpftrace strace > /var/tmp/dnf_srv.txt 2>&1; aws s3 cp --quiet s3://$B/bin/cbase_D.unstripped /opt/mt/cbase_D.unstripped --endpoint-url \$E; for n in D; do aws s3 cp --quiet s3://$B/bin/cbase_\$n /opt/mt/cbase_\$n --endpoint-url \$E; done; until aws s3 cp --quiet s3://$B/bin/tlsres /opt/mt/tlsres --endpoint-url \$E 2>/dev/null; do sleep 5; done; chmod +x /opt/mt/cbase_* /opt/mt/tlsres; touch /opt/mt/READY; fi
touch /opt/mt/UD_DONE
UDE
  case $role in srv) T=t4g.nano; CR=(--credit-specification CpuCredits=unlimited);; load) T=c7g.2xlarge; CR=(--monitoring Enabled=false);; esac
  ID=$(aws ec2 run-instances --image-id $AMI --instance-type $T --subnet-id $SUBNET --security-group-ids $SG \
    --iam-instance-profile Name=$ROLE --instance-initiated-shutdown-behavior terminate "${CR[@]}" \
    --metadata-options HttpTokens=required,HttpEndpoint=enabled,HttpPutResponseHopLimit=1 \
    --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=cbase-mem-test-$role},{Key=purpose,Value=cbase-mem-test},{Key=owner,Value=cdom}]" "ResourceType=volume,Tags=[{Key=purpose,Value=cbase-mem-test},{Key=owner,Value=cdom}]" \
    --user-data file://$UD --query 'Instances[0].InstanceId' --output text)
  rm -f $UD
  echo "$(echo $role | tr a-z A-Z)=$ID" >> $ST; echo "$role $ID"; echo "run_instances_$role=$ID utc=$(date -u +%FT%TZ)" >> /Users/alex/Dev/foolish-bench-results/campaign/trip1/out/launch.txt
done
cat $ST
