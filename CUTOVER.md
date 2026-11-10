# Cutover: foolish, utils and uttt onto ./cbase

This runbook moves foolish.cards, www, utils.foolish.cards, api.foolish.cards, uttt.live and www.uttt.live onto `./cbase` (the submodule at `cbase/`, pinned to cbase main b4d5cd41).
`./cbase` ADOPTS what runs today (Alex, 2026-11-09).
The VM is still `cbase-vm`'s instance, the database is still `/var/lib/cbase/cbase.sqlite` on its disk, and its backups still go to the same key.
The files are still in `cbase-web-487337210065`, and the certificates are today's.
So no data moves.
What changes is the systemd unit on the VM (`cbase.service` becomes `cbase-foolish.service`, on the same port and the same data) and the three distributions, which cbase makes new (`cbase-app-foolish` with its API distribution, `cbase-app-utils`, `cbase-app-uttt`).
Downtime is accepted for this one migration.
Nothing in this runbook has run against live AWS except the dry runs in section 1.

The live values the confs name, each read from the stacks:

| Key | Value | Read from |
|---|---|---|
| `CBASE_VM_STACK` | `cbase-vm` (ASG `cbase-vm`, instance `i-08f26ffb900294ee8`, t4g.nano) | describe-stack-resources |
| `CBASE_VM_ORIGIN` | `vm-origin.foolish.cards` (AAAA `2600:1f18:e01:9f00::10`, the stack's `OriginIpv6`) | stack outputs, dig |
| `CBASE_VM_PORT`, `CBASE_VM_DATA`, `CBASE_VM_REPLACES` | `8080`, `/var/lib/cbase`, `cbase.service` | `c_base/providers/local/ops/cbase.service` |
| `CBASE_VM_SETTINGS` | `/cbase-vm/env/` (ADMIN_TOKEN, ALLOWED_ORIGINS, BACKUP_BUCKET, BACKUP_REGION, JWT_SECRET, ORIGIN_SECRET, PUBLIC_URL) | describe-parameters |
| servers (the adopted default) | `s3://cbase-code-487337210065-us-east-1/vm/foolish/` (the role reads `vm/*`; vm_release.sh's prune touches `vm/cbase-serve-*` alone) | the release parameter, the role |
| `CBASE_BACKUP_BUCKET`, `CBASE_BACKUP_KEY` | `cbase-vm-backupbucket-whuc9g4lhome`, `cbase/latest.sqlite` | stack output `BackupBucket`, `/cbase-vm/env/CBASE_BACKUP_BUCKET`, the bucket's listing |
| `CBASE_STATIC_BUCKET`, `CBASE_STATIC_PREFIX` | `cbase-web-487337210065`, `foolish` / `uttt` / `utils` | `cbase-web` output `BucketName`, the bucket's listing |
| `CBASE_CERT_ARN` (foolish, utils) | `...certificate/42ecb64e-bf84-4372-9170-2e645119b12f` | `E1YC4MJXHIOBQD`'s ViewerCertificate |
| `CBASE_CERT_ARN` (uttt) | `...certificate/6f0b38fc-6ed7-40b3-83ef-14347c9cb4ae` | `E1PSTHCGSF35HM`'s ViewerCertificate |
| `CBASE_API_CERT_ARN` | `...certificate/30120b19-e9b9-419c-b3a2-6ce73f2e8765` | `EUSADBN5SXTDP`'s ViewerCertificate |

Each app deploys from its own directory, because the tool resolves the static dir against the working directory.
utils is built in the foolish-utils repository and deployed from there with `-f`.
In the blocks below, `T` is this repository's root, and the shell is bash.

## 1. The dry runs (2026-11-09, cbase 664de030)

A dry run's AWS calls, read in `cbase/tools/cbase` and `tools/cbase.d/*.sh` at 664de030, are all reads except one set: `create-change-set`, its wait and describe, then `delete-change-set`, and for a new stack `delete-stack` of the empty REVIEW_IN_PROGRESS stack the create leaves.
The reads are `sts get-caller-identity`, `cloudformation describe-stack-resources`/`describe-stacks`, `autoscaling describe-auto-scaling-groups`, `ec2 describe-instances`/`describe-instance-types`, `ssm get-parameter --with-decryption` (the origin secret, masked), `s3api head-bucket`/`get-bucket-location`/`get-bucket-policy`, `acm describe-certificate`, `route53 list-hosted-zones-by-name` and `cloudfront list-distributions`.
The state bucket a real deploy makes (`group_state_bucket_adopted`) is only printed in a dry run, and no state file is read while it is missing.
The `put-parameter` and `start-instances` in that path are text the tool prints for a person to run.
No dry run left a stack behind.

| App | Change set | Problems listed (exit 1, by design) |
|---|---|---|
| foolish | CREATE `cbase-app-foolish`: Add `ApiDistribution`, `Distribution`, `Oac`, `Router`, `ServerTimingPolicy` | the bucket policy; foolish.cards and www.foolish.cards held by `E1YC4MJXHIOBQD`, api.foolish.cards by `EUSADBN5SXTDP` |
| uttt | CREATE `cbase-app-uttt`: Add `Distribution`, `Oac`, `Router` | the bucket policy; uttt.live and www.uttt.live held by `E1PSTHCGSF35HM` |
| utils (on a copy of the live `utils/` folder) | CREATE `cbase-app-utils`: Add `Distribution`, `Oac`, `Router` | the bucket policy; utils.foolish.cards held by `E1YC4MJXHIOBQD` |

Nothing is modified, removed or replaced, no change set touches a bucket, and nothing touches `cbase-vm`'s instance, ASG or launch template (deploy never updates an adopted stack).
foolish's VM side, from the dry run and `vm_remote.sh`: it would make the state bucket and seed `vm/foolish.txt` with `app=foolish port=8080 service=cbase-foolish.service mem=~128 data=/var/lib/cbase legacy=1` (128 of the 320 MiB for apps).
On the instance it would install `cbase-foolish.socket` (`ListenStream=[::]:8080`, dual-stack, Backlog 4096) and `cbase-foolish.service`, stop and disable `cbase.service`, give `/var/lib/cbase` to `cbase-foolish`, start the socket, then start the service on it.

**BLOCKER: the server cannot take the socket yet.** cbase 664de030's unit hands serve its port as systemd's socket (fd 3, `LISTEN_PID`/`LISTEN_FDS`), and cbase's own serve takes it (`cbase/core/src/net/server.c` `inherited_listener`, test `core/test/unit_socket_activation.c`).
foolish's server is the in-tree `c_base/`, which has no such code: it would bind `:::8080` itself, find the socket holding it, and exit.
The deploy would then end with `cbase.service` already stopped and disabled, and the API would be down until the rollback.
The fix is to port `inherited_listener` into `c_base/core/src/net/server.c` with its test, before any real foolish deploy.

## 2. The hand steps

### 2a. Before the window

1. **The state bucket.** foolish's first real deploy makes `s3://cbase-state-487337210065` (private, versioned) and seeds its member; nothing to do by hand.
2. **The old deploys off.** `c_base.yml`'s `deploy` job runs `vm_release.sh`, which reinstalls and restarts `cbase.service` on port 8080 and would fight `cbase-foolish.service` for it.
   `web_aws.yml`'s `deploy` job uploads into the same folders and rewrites the old routers.
   Both jobs are turned off in a PR merged before the window, and nothing else merges during it.
3. **The bucket policy.** `cbase-web-487337210065`'s policy lets only `E1YC4MJXHIOBQD` and `E1PSTHCGSF35HM` read it (by `AWS:SourceArn`), so a new distribution would get 403s.
   Add the one statement the dry run prints, keeping both of the existing ones:
   `{"Sid": "TheAccountsDistributionsRead", "Effect": "Allow", "Principal": {"Service": "cloudfront.amazonaws.com"}, "Action": ["s3:GetObject", "s3:ListBucket"], "Resource": ["arn:aws:s3:::cbase-web-487337210065", "arn:aws:s3:::cbase-web-487337210065/*"], "Condition": {"StringEquals": {"aws:SourceAccount": "487337210065"}}}`.
   The policy is `cbase-web`'s `BucketPolicy`, so this makes that stack drift; the stack is kept and never updated again.
4. **The builds.** `cd "$T/foolish" && npm run build`, `cd "$T/uttt/web" && npm ci && npm run build`, foolish-utils' export (with `output: 'export'` and `images.unoptimized` set, as c_base/RUNBOOK.md W5 says), and the server: `cd "$T" && c_base/providers/aws/ops/build_bootstrap.sh serve`.
5. **TTLs.** Lower to 300 at Squarespace a day ahead (www.foolish.cards is 14400 today).

### 2b. The window

1. **The aliases off the old distributions.** CloudFront serves a hostname from one distribution only, and deploy stops on each one that another distribution holds, naming it.
   - `E1YC4MJXHIOBQD` (`cbase-web`, dgow77nr7v5kf.cloudfront.net): foolish.cards, www.foolish.cards, utils.foolish.cards.
   - `E1PSTHCGSF35HM` (`cbase-web`, d2hb0vxf10cj0w.cloudfront.net): uttt.live, www.uttt.live.
   - `EUSADBN5SXTDP` (`cbase`, d3rcf2zypd78ll.cloudfront.net): api.foolish.cards.
   Use the CloudFront console (each distribution, General, Edit, the alternate domain names) and keep each certificate where it is, so the rollback only puts the names back.
   The sites and the API are down from here until DNS points at the new distributions.
2. **The deploys.**
   ```
   (cd "$T/foolish" && CBASE_SERVE_BIN=../c_base/dist/cbase-serve-linux-arm64 ../cbase/tools/cbase deploy)
   (cd "$T/uttt" && ../cbase/tools/cbase deploy)
   (cd ~/Dev/foolish-utils && "$T/cbase/tools/cbase" -f "$T/utils/cbase.conf" deploy)
   ```
   foolish's deploy uploads the server to `s3://cbase-code-487337210065-us-east-1/vm/foolish/<sha256>`, then runs one SSM command on `i-08f26ffb900294ee8`.
   That command writes `/etc/cbase/foolish.env` from `/cbase-vm/env/`, installs `cbase-foolish.socket` and `cbase-foolish.service`, stops and disables `cbase.service`, gives `/var/lib/cbase` to the new user `cbase-foolish`, starts the socket on 8080 and the service on it (the same database and backup key), and waits for `/health`.
   It needs the server to take the inherited socket (the BLOCKER in section 1).
   Then it uploads the files and makes the stack.
3. **The DNS records (Squarespace).** Each deploy prints its records; with the distributions' names it prints:

   | Domain | Host | Type | Data |
   |---|---|---|---|
   | foolish.cards | `@` | ALIAS | foolish's `DistributionDomain` |
   | foolish.cards | `www` | CNAME | foolish's `DistributionDomain` |
   | foolish.cards | `api` | CNAME | foolish's `ApiDistributionDomain` |
   | foolish.cards | `utils` | CNAME | utils' `DistributionDomain` |
   | uttt.live | `@` | ALIAS | uttt's `DistributionDomain` |
   | uttt.live | `www` | CNAME | uttt's `DistributionDomain` |

   `aws cloudformation describe-stacks --stack-name cbase-app-<app> --query 'Stacks[0].Outputs' --output text` prints them again.
   `vm-origin.foolish.cards` and the ACM validation CNAMEs stay as they are.
4. **Check** (with `--connect-to host:443:<distribution>:443` before the records have spread):
   - foolish.cards `/`, `/dashboard`, `/privacy`, `/abc123` and `/abc123/x` are 200 `text/html` (the last two are the shell, and the deeper one shows the app's not-found view).
   - `/.well-known/apple-app-site-association` is 200 `application/json` with no redirect, and `/no-such.js` is a 404.
   - `/m/1x` has its `<title>`, `https://uttt.live/234` lands on `/243`, and utils.foolish.cards `/` is 200.
   - `https://api.foolish.cards/health` is `ok cbase ...`, and `/v1/leaderboard` is 200 with a `server-timing` header.
   - Then sign in and play a move against a bot, open a replay link, run `cd foolish && npm run test:validate`, and tap a foolish.cards link on a phone.
   - In `journalctl -u cbase-foolish` (`aws ssm start-session --target i-08f26ffb900294ee8`), a `backed up to ...` line follows the first write.

## 3. Rollback

Every old stack, distribution, bucket and parameter is kept.

1. `cd "$T/foolish" && ../cbase/tools/cbase undeploy` (and the same for uttt and utils): it deletes the three new stacks, stops and removes `cbase-foolish.service`, and keeps the data and the files.
2. On the instance (SSM), free the port, give the data back to the old unit's user and start it: `systemctl disable --now cbase-foolish.socket; chown -R cbase:cbase /var/lib/cbase && systemctl enable --now cbase`, then `c_base/providers/local/ops/vm_release.sh --verify` from a tree that contains main.
3. Put the aliases back on `E1YC4MJXHIOBQD`, `E1PSTHCGSF35HM` and `EUSADBN5SXTDP`, as in 2b.1.
4. Put the records back at Squarespace: `@`, `www` and `utils` to `dgow77nr7v5kf.cloudfront.net`, `api` to `d3rcf2zypd78ll.cloudfront.net`, and uttt.live `@` and `www` to `d2hb0vxf10cj0w.cloudfront.net`.
5. Turn the old deploy jobs back on.

No data is lost either way: both units serve the same file and back it up to the same key.

## 4. After it holds

- If the instance is ever replaced or rebooted, its boot script installs and restarts `cbase.service` again, which then races `cbase-foolish.socket` for 8080: run foolish's `./cbase deploy` right after (cbase README, CBASE_VM_REPLACES), and until cbase-vm's boot script stops doing it, treat a reboot as a deploy.
- The old distributions, `cbase-web`'s routers and the `cbase` (API) stack go when Alex says so; the bucket and the VM stack stay, since cbase now runs on them.
- CI deploys through `./cbase` once the GitHub role can make every call `cbase/tools/cbase deploy` makes.
