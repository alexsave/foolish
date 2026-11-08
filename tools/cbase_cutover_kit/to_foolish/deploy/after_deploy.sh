#!/bin/bash
# after_deploy.sh - foolish's own steps against its DynamoDB table after every c_base deploy
# (c_base/providers/aws/ops/deploy.sh step 7 runs it as CBASE_DEPLOY_AFTER, from foolish/server,
# with CBASE_STORE=dynamo, CBASE_TABLE, CBASE_CF_DISTRIBUTION and CBASE_IMPORT_DIR set).
#
# A fresh table has no bots (nothing else seeds DynamoDB; SQLite seeds them at open), so every
# deploy seeds the schema's into a table that has none and leaves a table with bots alone; the
# leaderboard's sparse, rating-ordered index key moves rows from before it once; with --import DIR
# the export is imported, replacing the bots with the hosted rows, as it does on SQLite.
set -euo pipefail
make -s build/cbase-import
build/cbase-import --seed-bots
build/cbase-import --reindex-ratings
if [ -n "${CBASE_IMPORT_DIR:-}" ]; then
  echo "importing $CBASE_IMPORT_DIR into DynamoDB table $CBASE_TABLE"
  build/cbase-import "$CBASE_IMPORT_DIR"
fi
