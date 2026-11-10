#!/bin/bash
# ssm.sh INSTANCE "command" [timeout_s] : run a shell command through SSM and print its output
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
id=$1; cmd=$2; to=${3:-300}
P=$(python3 -c 'import json,sys; print(json.dumps({"commands":[sys.argv[1]],"executionTimeout":[sys.argv[2]]}))' "$cmd" "$to")
C=$(aws ssm send-command --instance-ids $id --document-name AWS-RunShellScript --parameters "$P" --timeout-seconds 600 --query Command.CommandId --output text) || exit 1
n=0; while :; do
  s=$(aws ssm get-command-invocation --command-id $C --instance-id $id --query Status --output text 2>/dev/null)
  case $s in Success|Failed|Cancelled|TimedOut) break;; esac
  sleep 2; n=$((n+1)); [ $n -gt $((to/2+30)) ] && break
done
aws ssm get-command-invocation --command-id $C --instance-id $id --query '[Status,StandardOutputContent,StandardErrorContent]' --output text
