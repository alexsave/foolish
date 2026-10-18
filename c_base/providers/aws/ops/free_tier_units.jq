# free_tier_units.jq - the provisioned units of a set of DynamoDB tables, as
# "<read> <write>": deploy.sh's account-wide free-tier gate sums every table
# of the account but the stack's own with it.
#
# Input: a JSON array (jq -s) of `aws dynamodb describe-table --output json`
# answers. A table billed per request provisions nothing (and is outside the
# free tier's units altogether); a provisioned table counts its own units and
# every global secondary index's.
def units(f):
  if (.Table.BillingModeSummary.BillingMode // "PROVISIONED") == "PAY_PER_REQUEST" then 0
  else (.Table.ProvisionedThroughput[f] // 0)
       + ([.Table.GlobalSecondaryIndexes[]?.ProvisionedThroughput[f] // 0] | add // 0)
  end;
"\(map(units("ReadCapacityUnits")) | add // 0) \(map(units("WriteCapacityUnits")) | add // 0)"
