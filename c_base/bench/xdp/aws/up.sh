#!/bin/bash
# up.sh : launch the XDP spike. Creates (all named cbase-perf-temp-xdp-*, all ids in state/<run>.env):
#   a private bucket that only the instance role may use, a security group whose only ingress is from itself,
#   a t4g.nano (unlimited credits) that builds xsrv on itself and runs srvctl.py, and a c7g.large that builds xload and
#   runs master.sh. Both boxes: terminate on shutdown, `shutdown -h +240` plus a systemd poweroff timer, IPv6 only,
#   dual-stack SSM. Nothing here touches any other resource; collect.sh and cleanup.sh act only on the state file's ids.
set -euo pipefail
X=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export AWS_PAGER= AWS_DEFAULT_REGION=${AWS_DEFAULT_REGION:-us-east-1}
REGION=$AWS_DEFAULT_REGION
SUBNET=${XDP_SUBNET:-subnet-0560285f2f6b3c851}
PROFILE=${XDP_PROFILE:-cbase-vm-InstanceProfile-J78j0oPTjXv7}
aws sts get-caller-identity --query Arn --output text
[ -f "$X/lwip-2_2_1.tar.gz" ] || "$X/fetch_lwip.sh"

SUF=$(openssl rand -hex 4)
RUN=$(date -u +%Y%m%dT%H%M%SZ)-$SUF
mkdir -p "$X/state"
STATE=$X/state/$RUN.env
: > "$STATE"
rec() { echo "$1=$2" >> "$STATE"; }
rec RUN "$RUN"; rec REGION "$REGION"
VPC=$(aws ec2 describe-subnets --subnet-ids "$SUBNET" --query 'Subnets[0].VpcId' --output text)
AMI=$(aws ssm get-parameter --name /aws/service/ami-amazon-linux-latest/al2023-ami-kernel-default-arm64 --query Parameter.Value --output text)
ROLE=$(aws iam get-instance-profile --instance-profile-name "$PROFILE" --query 'InstanceProfile.Roles[0].Arn' --output text)
rec AMI "$AMI"

B=cbase-perf-temp-xdp-$SUF
aws s3api create-bucket --bucket "$B" >/dev/null
rec BUCKET "$B"
aws s3api put-public-access-block --bucket "$B" --public-access-block-configuration BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/bpol.json" <<EOF
{"Version":"2012-10-17","Statement":[
 {"Effect":"Allow","Principal":{"AWS":"$ROLE"},"Action":["s3:PutObject","s3:GetObject"],"Resource":"arn:aws:s3:::$B/*"},
 {"Effect":"Allow","Principal":{"AWS":"$ROLE"},"Action":["s3:ListBucket"],"Resource":"arn:aws:s3:::$B"}]}
EOF
aws s3api put-bucket-policy --bucket "$B" --policy "file://$TMP/bpol.json"

SG=$(aws ec2 create-security-group --group-name "cbase-perf-temp-xdp-sg-$SUF" --description "xdp spike, self ingress only" --vpc-id "$VPC" --query GroupId --output text)
rec SG "$SG"
aws ec2 create-tags --resources "$SG" --tags "Key=Name,Value=cbase-perf-temp-xdp-sg-$SUF"
aws ec2 authorize-security-group-ingress --group-id "$SG" --ip-permissions "IpProtocol=-1,UserIdGroupPairs=[{GroupId=$SG}]" >/dev/null
aws ec2 authorize-security-group-egress --group-id "$SG" --ip-permissions 'IpProtocol=-1,Ipv6Ranges=[{CidrIpv6=::/0}]' >/dev/null
aws ec2 revoke-security-group-egress --group-id "$SG" --ip-permissions 'IpProtocol=-1,IpRanges=[{CidrIp=0.0.0.0/0}]' >/dev/null || true

mkdir -p "$TMP/pkg/port/arch"
cp "$X"/{common.h,be.h,srv.c,be_xdp.c,xdp_prog.c,load.c,build.sh,lwip-2_2_1.tar.gz} "$TMP/pkg/"
cp "$X/port/lwipopts.h" "$TMP/pkg/port/"; cp "$X/port/arch/cc.h" "$TMP/pkg/port/arch/"
cp "$X/aws/srvctl.py" "$X/aws/master.sh" "$TMP/pkg/"
echo "$B" > "$TMP/pkg/bucket.txt"
(cd "$TMP/pkg" && tar czf "$TMP/pkg.tgz" .)
aws s3 cp --only-show-errors "$TMP/pkg.tgz" "s3://$B/xdp/pkg.tgz"

common() { # the first part of both user-data scripts
  cat <<EOF
#!/bin/bash
shutdown -h +240
systemd-run --unit xs-killswitch --on-active=14700 /usr/sbin/poweroff
mkdir -p /etc/amazon/ssm
echo '{"Agent": {"UseDualStackEndpoint": true}}' > /etc/amazon/ssm/amazon-ssm-agent.json
systemctl restart amazon-ssm-agent
fallocate -l 1G /swapfile && chmod 600 /swapfile && mkswap /swapfile && swapon /swapfile
mkdir -p /var/tmp/xs && cd /var/tmp/xs
export AWS_USE_DUALSTACK_ENDPOINT=true AWS_DEFAULT_REGION=$REGION
for i in \$(seq 1 30); do aws s3 cp --only-show-errors s3://$B/xdp/pkg.tgz pkg.tgz && break; sleep 10; done
tar xzf pkg.tgz && chmod +x *.sh
EOF
}
launch() { # role type userdata-file credit-args...
  local role=$1 type=$2 ud=$3; shift 3
  aws ec2 run-instances --image-id "$AMI" --instance-type "$type" --subnet-id "$SUBNET" --security-group-ids "$SG" \
    --ipv6-address-count 1 --iam-instance-profile "Name=$PROFILE" --instance-initiated-shutdown-behavior terminate "$@" \
    --metadata-options HttpTokens=required,HttpEndpoint=enabled,HttpProtocolIpv6=enabled,HttpPutResponseHopLimit=1 \
    --private-dns-name-options HostnameType=resource-name,EnableResourceNameDnsAAAARecord=true \
    --block-device-mappings "DeviceName=/dev/xvda,Ebs={VolumeSize=8,VolumeType=gp3,DeleteOnTermination=true,Encrypted=true}" \
    --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=cbase-perf-temp-xdp-$role-$SUF}]" "ResourceType=volume,Tags=[{Key=Name,Value=cbase-perf-temp-xdp-$role-$SUF}]" \
    --user-data "file://$ud" --query 'Instances[0].InstanceId' --output text
}

{ common; cat <<EOF
systemd-run --unit xs-srvctl -p LimitNOFILE=65536 -p LimitMEMLOCK=infinity --working-directory=/var/tmp/xs /usr/bin/python3 /var/tmp/xs/srvctl.py
dnf install -y --setopt=install_weak_deps=False gcc clang libbpf-devel kernel-headers ethtool > dnf.log 2>&1
if ./build.sh /var/tmp/xs/bin > build.log 2>&1; then touch BUILD_OK; else touch BUILD_FAILED; fi
aws s3 cp --only-show-errors build.log s3://$B/xdp/srv/srv_build.log; aws s3 cp --only-show-errors dnf.log s3://$B/xdp/srv/srv_dnf.log
swapoff /swapfile && rm -f /swapfile
EOF
} > "$TMP/ud_srv.sh"
SRV=$(launch srv t4g.nano "$TMP/ud_srv.sh" --credit-specification CpuCredits=unlimited)
rec INSTANCE_srv "$SRV"; echo "srv $SRV"
S6=None
for i in $(seq 1 30); do
  S6=$(aws ec2 describe-instances --instance-ids "$SRV" --query 'Reservations[0].Instances[0].Ipv6Address' --output text)
  [ "$S6" != None ] && break; sleep 2
done
rec SRV6 "$S6"

{ common; cat <<EOF
dnf install -y --setopt=install_weak_deps=False gcc > dnf.log 2>&1
XLOAD_ONLY=1 ./build.sh /var/tmp/xs/bin > build.log 2>&1
aws s3 cp --only-show-errors build.log s3://$B/xdp/srv/load_build.log
swapoff /swapfile && rm -f /swapfile
systemd-run --unit xs-master -p LimitNOFILE=65536 --working-directory=/var/tmp/xs /bin/bash /var/tmp/xs/master.sh $S6 $B
EOF
} > "$TMP/ud_load.sh"
LOAD=$(launch load c7g.large "$TMP/ud_load.sh")
rec INSTANCE_load "$LOAD"; echo "load $LOAD"
echo "state: $STATE"
