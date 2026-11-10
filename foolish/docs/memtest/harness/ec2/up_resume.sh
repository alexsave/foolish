#!/bin/bash
# up.sh : throwaway role/profile, SG, bucket, and two boxes (t4g.nano server, c7g.2xlarge load), all tagged purpose=cbase-mem-test owner=cdom
set -euo pipefail
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
ST=/Users/alex/Dev/cdom-memtest/state.env; source $ST
D=/Users/alex/Dev/cdom-memtest; SUBNET=subnet-0560285f2f6b3c851

source $ST

for role in load; do
  UD=$(mktemp)
  cat > $UD <<UDE
#!/bin/bash
shutdown -h +30
mkdir -p /etc/amazon/ssm
echo '{"Agent": {"UseDualStackEndpoint": true}}' > /etc/amazon/ssm/amazon-ssm-agent.json
systemctl restart amazon-ssm-agent
mkdir -p /opt/mt; echo $B > /opt/mt/bucket
E=https://s3.dualstack.us-east-1.amazonaws.com
for f in srv.sh load.sh src.tgz; do aws s3 cp --quiet s3://$B/pkg/\$f /opt/mt/\$f --endpoint-url \$E; done
chmod +x /opt/mt/*.sh
if [ $role = load ]; then /opt/mt/load.sh build > /var/tmp/build_out.txt 2>&1; aws s3 cp --quiet /var/tmp/build_out.txt s3://$B/res/build_out.txt --endpoint-url \$E; fi
if [ $role = srv ]; then until aws s3 cp --quiet s3://$B/bin/cbase /opt/mt/cbase --endpoint-url \$E 2>/dev/null; do sleep 5; done; chmod +x /opt/mt/cbase; touch /opt/mt/READY; fi
touch /opt/mt/UD_DONE
UDE
  case $role in srv) T=t4g.nano; CR=(--credit-specification CpuCredits=unlimited);; load) T=c7g.2xlarge; CR=(--monitoring Enabled=false);; esac
  ID=$(aws ec2 run-instances --image-id $AMI --instance-type $T --subnet-id $SUBNET --security-group-ids $SG --ipv6-address-count 1 \
    --iam-instance-profile Name=$ROLE --instance-initiated-shutdown-behavior terminate "${CR[@]}" \
    --metadata-options HttpTokens=required,HttpEndpoint=enabled,HttpProtocolIpv6=enabled,HttpPutResponseHopLimit=1 \
    --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=cbase-mem-test-$role},{Key=purpose,Value=cbase-mem-test},{Key=owner,Value=cdom}]" "ResourceType=volume,Tags=[{Key=purpose,Value=cbase-mem-test},{Key=owner,Value=cdom}]" \
    --user-data file://$UD --query 'Instances[0].InstanceId' --output text)
  rm -f $UD
  echo "LOAD=$ID" >> $ST; echo "$role $ID"
done
cat $ST
