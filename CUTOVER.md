# Cutover: foolish, utils and uttt onto ./cbase

The command sequence that moves foolish.cards, www, utils.foolish.cards, api.foolish.cards, uttt.live and www.uttt.live onto `./cbase` (the submodule at `cbase/`, cbase main 664de030).
The dispatcher runs it, on Alex's word, top to bottom: each step has its command, the check that must hold before the next one, and the way back from that point.
`./cbase` ADOPTS the live setup (Alex, 2026-11-09).
The VM is still `cbase-vm`'s instance, IPv6-only, and the database is still `/var/lib/cbase/cbase.sqlite` with its backups at the same key.
The files are still in `cbase-web-487337210065` and the certificates are today's, so no data moves.
What changes: on the VM, `cbase.service` gives way to `cbase-foolish.socket` and `cbase-foolish.service` (the same port, data and backups); at the edge, three new stacks (`cbase-app-foolish` with its API distribution, `cbase-app-utils`, `cbase-app-uttt`) take the hostnames from the three old distributions.
**No DNS record changes** (Alex: "Try not to change DNS. It's fine as is").
The Squarespace records (`www`, `utils` and the apex to dgow77nr7v5kf, `api` to d3rcf2zypd78ll, uttt.live to d2hb0vxf10cj0w) keep reaching CloudFront's edge, which serves a hostname from whichever distribution holds it as an alias.
The old distributions stay, without aliases, as the rollback: nothing in the cutover deletes them.
Downtime is accepted for this one migration: the sites and the API are down from step 5 until step 7.

Every block is bash: run `bash` first (zsh does not word-split and errors on an empty glob).
Paste this once per shell; every later block uses it:

```
T=~/Dev/foolish                       # a checkout of main at or after #482's merge, with the submodule: git -C "$T" submodule update --init cbase
VM=i-08f26ffb900294ee8
OLD_FOOLISH=E1YC4MJXHIOBQD            # foolish.cards www.foolish.cards utils.foolish.cards (stack cbase-web)
OLD_UTTT=E1PSTHCGSF35HM               # uttt.live www.uttt.live (stack cbase-web)
OLD_API=EUSADBN5SXTDP                 # api.foolish.cards (stack cbase)
BUCKET=cbase-web-487337210065
K="$HOME/cutover-$(date -u +%Y%m%d)"; mkdir -p "$K"; chmod 700 "$K"   # what the rollback reads back
```

## The values the confs name

Each one was read from the live stacks, read-only:

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

## What runs on the VM, and when (the boot race)

- **A plain reboot runs no boot script.** The launch template's user data (version 6, the ASG's) is the plain `#!/bin/bash` stub, which cloud-init runs once per instance, at its first boot.
  On a reboot, systemd starts what is enabled; after step 6 that is `cbase-foolish.socket` and `cbase-foolish.service`, and `cbase.service` is disabled.
- **A new instance** (an ASG replacement) runs the stub, which runs the pinned boot script (`/cbase-vm/release/boot`), which installs and starts `cbase.service` on a fresh disk.
  That disk has no database, so serve restores `cbase/latest.sqlite` from the backup bucket at its start.
  serve restores only into a missing database (`backup.c` `cb_backup_restore`), never over one, and the boot script restores nothing itself.
  After a replacement, run step 6 again.
- **`vm_release.sh` (every push to main until step 0.2, from `c_base.yml`'s old deploy job; by hand only since) and `vm_deploy.sh --restart`** send the stub over SSM to the running instance, which runs the boot script now.
  Before #482 that reinstalled and restarted `cbase.service`, a second server on the socket's port, and its `StateDirectory=cbase` would take `/var/lib/cbase` back from `cbase-foolish`.
  #482 adds step 2c to `vm_boot.sh`: once a `cbase-<app>.socket` is installed, the boot ends before step 3 and leaves `cbase.service`, the port and the data alone (`c_base/providers/local/ops/test/vm_boot_handover_test.sh`).
- **How that change reaches the instance:** `vm_release.sh` uploads the tree's boot script as `vm/cbase-vm-boot-<sha12>`, points `/cbase-vm/release/boot` at it and runs it through SSM.
  There is no launch-template version and no instance replacement: the stub, the only thing in the launch template, does not change.
  `c_base.yml`'s deploy job does exactly that when #482 merges (it touches `c_base/**`).
  It ships the boot script with step 2c and the server that takes the socket, both before step 1 below.
  On that run no socket exists yet, so the boot restarts `cbase.service` as every release does: a blip of a second or two.

## 0. Before the window

1. **#482 is merged, and main has deployed it.**
   ```
   git -C "$T" fetch -q origin && git -C "$T" checkout -q origin/main && git -C "$T" submodule update --init cbase
   git -C "$T" log --oneline -1
   gh run list --workflow c_base.yml --branch main --limit 1     # completed, success, for that commit
   aws ssm get-parameter --name /cbase-vm/release/boot --query Parameter.Value --output text   # ... vm/cbase-vm-boot-<sha12> ...: the boot with step 2c
   ```
   Check: the run is green.
   Its `serve_binary` job made the release `cbase-serve-<short sha>`, and `vm_release.sh --verify` passed.
2. **The old deploy jobs off.** A PR that turns off `c_base.yml`'s `deploy` job and `web_aws.yml`'s `deploy` job is merged, and nothing else merges until step 8.
   Otherwise the next push would release to `cbase.service`'s path (step 2c then makes that a failed release), or upload into the folders and rewrite the old routers.
   Check: `gh run list --branch main --limit 3` shows that PR's runs with no deploy job.
3. **The server binary**, main's own release, the one `cbase.service` runs now:
   ```
   sha="$(git -C "$T" rev-parse --short=9 HEAD)"
   gh release download "cbase-serve-$sha" -p 'cbase-serve-linux-arm64*' -D "$T/c_base/dist" --clobber
   (cd "$T/c_base/dist" && shasum -a 256 -c cbase-serve-linux-arm64.sha256)
   aws ssm get-parameter --name /cbase-vm/release/serve --query Parameter.Value --output text | cut -d' ' -f3   # = the .sha256 file's hash
   ```
   If the PR in 0.2 changed `c_base/**`, the newest release is that PR's commit: use the commit `/cbase-vm/release/serve` names.
4. **The static builds:**
   ```
   (cd "$T/foolish" && npm ci && npm run build) && ls "$T/foolish/out/_shell.html"
   (cd "$T/uttt/web" && npm ci && npm run build) && ls "$T/uttt/web/out/_shell.html"
   (cd ~/Dev/foolish-utils && npm run build) && ls ~/Dev/foolish-utils/out/index.html
   ```
   foolish-utils' committed `next.config.ts` has no `output: 'export'`, so its build writes no `out/`.
   Build it with `output: 'export'` and `images.unoptimized: true` (c_base/RUNBOOK.md W5), or deploy what is live today: `mkdir -p "$K/utils" && aws s3 sync --only-show-errors s3://$BUCKET/utils/ "$K/utils/out/"`, then use `$K/utils` as utils' directory in steps 3 and 7.
5. **The SSM helper**, used in steps 6 and 9:
   ```
   ssm_run() {   # ssm_run INSTANCE FILE: FILE as root through Run Command, waited for, its output printed
     local b64 id n=0 s
     b64="$(base64 < "$2" | tr -d '\n')"
     id="$(aws ssm send-command --instance-ids "$1" --document-name AWS-RunShellScript --timeout-seconds 600 \
             --parameters "commands=f=\$(mktemp) && echo $b64 | base64 -d > \$f && bash \$f; rc=\$?; rm -f \$f; exit \$rc" \
             --query Command.CommandId --output text)"
     until s="$(aws ssm get-command-invocation --command-id "$id" --instance-id "$1" --query Status --output text 2> /dev/null)" \
           && [ "$s" != Pending ] && [ "$s" != InProgress ] && [ "$s" != Delayed ] || [ $n -gt 120 ]; do sleep 5; n=$((n + 1)); done
     aws ssm get-command-invocation --command-id "$id" --instance-id "$1" --query '[Status,StandardOutputContent,StandardErrorContent]' --output text
   }
   ```

## 1. The bucket statement (no downtime)

The bucket's policy lets only the two old distributions read it, by `AWS:SourceArn`, so a new distribution would get 403s.
This adds cbase's one statement and keeps both old ones.

```
aws s3api get-bucket-policy --bucket $BUCKET --query Policy --output text > "$K/policy.before.json"
python3 -I - "$K/policy.before.json" "$K/policy.after.json" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))
assert 'TheAccountsDistributionsRead' not in [s.get('Sid') for s in p['Statement']]
p['Statement'].append({"Sid": "TheAccountsDistributionsRead", "Effect": "Allow", "Principal": {"Service": "cloudfront.amazonaws.com"},
                       "Action": ["s3:GetObject", "s3:ListBucket"],
                       "Resource": ["arn:aws:s3:::cbase-web-487337210065", "arn:aws:s3:::cbase-web-487337210065/*"],
                       "Condition": {"StringEquals": {"aws:SourceAccount": "487337210065"}}})
json.dump(p, open(sys.argv[2], 'w'))
PY
aws s3api put-bucket-policy --bucket $BUCKET --policy "file://$K/policy.after.json"
```

Check: `aws s3api get-bucket-policy --bucket $BUCKET --query Policy --output text | python3 -I -c 'import json,sys; print([s["Sid"] for s in json.load(sys.stdin)["Statement"]])'` prints `['CloudFrontRead', 'CloudFrontList', 'TheAccountsDistributionsRead']`, and `curl -sI https://www.foolish.cards/ | head -1` is still 200.
The policy is `cbase-web`'s `BucketPolicy`, so that stack now drifts; it is kept and never updated again.
Back: `aws s3api put-bucket-policy --bucket $BUCKET --policy "file://$K/policy.before.json"`.

## 2. The baseline (no downtime)

What the old distributions answer today; step 7 expects each of these to change:

```
curl -s -o /dev/null -w '%{http_code}\n' https://uttt.live/234                  # 302 (the old router's redirect); new: 200, the shell
curl -s https://foolish.cards/abc/def | grep -c game_id                         # 0 (the old router's /404.html); new: 1 or more, the shell
curl -s -o /dev/null -w '%{http_code}\n' https://utils.foolish.cards/no-such-page   # 404 (no shell); new: 200 (the shell, index.html)
```

These were checked on 2026-11-09: 302, 0 and 404.

## 3. The dry runs (no downtime)

```
(cd "$T/foolish" && ../cbase/tools/cbase deploy --dry-run) 2>&1 | tee "$K/dry-foolish.txt" | tail -12
(cd "$T/uttt" && ../cbase/tools/cbase deploy --dry-run) 2>&1 | tee "$K/dry-uttt.txt" | tail -8
(cd ~/Dev/foolish-utils && "$T/cbase/tools/cbase" -f "$T/utils/cbase.conf" deploy --dry-run) 2>&1 | tee "$K/dry-utils.txt" | tail -8
```

Check: each change set is a CREATE of Adds only (foolish: `ApiDistribution`, `Distribution`, `Oac`, `Router`, `ServerTimingPolicy`; uttt and utils: `Distribution`, `Oac`, `Router`), and each lists exactly one problem, "a hostname another distribution holds".
foolish also prints `would create state bucket s3://cbase-state-487337210065 ... and seed vm/foolish.txt with foolish (grandfathered: port 8080, data /var/lib/cbase)` and `memory: foolish ~128 = 128 MiB of the 320 MiB`.
Any other problem, or any Modify, Remove or Replace: stop here; nothing has changed but step 1.
A dry run makes and deletes a change set (and an empty stack), and changes nothing else.

## 4. The alias script

```
cat > "$K/aliases.sh" <<'EOF'
#!/bin/bash
# aliases.sh drop ID | add ID ALIAS...: a distribution's aliases removed, or set, and nothing else
# changed (its certificate stays); waits until the change is deployed. The config holds the origin
# secret as a header value, so it goes to a private temporary file and is never printed.
set -euo pipefail
umask 077
t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
op="$1"; id="$2"; shift 2
aws cloudfront get-distribution-config --id "$id" > "$t/all"
python3 -I - "$t/all" "$t/cfg" "$t/etag" "$op" "$@" <<'PY'
import json, sys
a = json.load(open(sys.argv[1]))
open(sys.argv[3], 'w').write(a['ETag'])
c = a['DistributionConfig']
print('aliases were:', ' '.join(c['Aliases'].get('Items', [])) or '(none)', file=sys.stderr)
new = sys.argv[5:] if sys.argv[4] == 'add' else []
c['Aliases'] = {'Quantity': len(new), 'Items': new} if new else {'Quantity': 0}
json.dump(c, open(sys.argv[2], 'w'))
PY
aws cloudfront update-distribution --id "$id" --if-match "$(cat "$t/etag")" --distribution-config "file://$t/cfg" --query Distribution.Status --output text
aws cloudfront wait distribution-deployed --id "$id"
echo "$id: aliases now: $(aws cloudfront get-distribution-config --id "$id" --query 'DistributionConfig.Aliases.Items' --output text)"
EOF
chmod 700 "$K/aliases.sh"
```

## 5. The aliases off the old distributions (THE DOWNTIME STARTS)

```
"$K/aliases.sh" drop $OLD_API
"$K/aliases.sh" drop $OLD_UTTT
"$K/aliases.sh" drop $OLD_FOOLISH
```

Each waits until its distribution is deployed (minutes).
Check: each prints `aliases now: None`, and `aws cloudfront list-distributions --query 'DistributionList.Items[?Aliases.Quantity > `0`].[Id,join(`,`,Aliases.Items)]' --output text` names none of the six hostnames.
`curl -sI https://www.foolish.cards/ | head -1` now fails or is a CloudFront 403: no distribution holds the name.
Back:
```
"$K/aliases.sh" add $OLD_FOOLISH foolish.cards www.foolish.cards utils.foolish.cards
"$K/aliases.sh" add $OLD_UTTT uttt.live www.uttt.live
"$K/aliases.sh" add $OLD_API api.foolish.cards
```

## 6. foolish: the VM switch and its distributions

```
(cd "$T/foolish" && CBASE_SERVE_BIN=../c_base/dist/cbase-serve-linux-arm64 ../cbase/tools/cbase deploy) 2>&1 | tee "$K/deploy-foolish.txt"
```

What it does, in order:
1. It makes the state bucket and seeds `vm/foolish.txt` (`app=foolish port=8080 ... data=/var/lib/cbase legacy=1`).
2. It uploads the server to `s3://cbase-code-487337210065-us-east-1/vm/foolish/<sha256>`.
3. It runs one SSM command on the instance:
   - It writes `/etc/cbase/foolish.env` from `/cbase-vm/env/` and installs `cbase-foolish.socket` (`[::]:8080`, dual-stack, backlog 4096) and `cbase-foolish.service`.
   - It stops and disables `cbase.service`, and gives `/var/lib/cbase` to the new user `cbase-foolish`.
   - It starts the socket, then the service on it, and waits for `/health` on 127.0.0.1:8080.
4. It uploads `out/` to `foolish/` (hashed files first, html last).
5. It creates `cbase-app-foolish`, whose distributions take foolish.cards, www.foolish.cards and api.foolish.cards.

Check:
- `deploy-foolish.txt` ends `deployed foolish: https://foolish.cards/`.
- Its `vm:` lines say `cbase.service stopped and disabled` and `cbase-foolish.service is up`.
- `curl -s https://api.foolish.cards/health` is `ok cbase ...`.
- On the instance:
  ```
  printf 'systemctl is-active cbase-foolish.socket cbase-foolish.service; systemctl is-enabled cbase.service || true\nstat -c %%U /var/lib/cbase/cbase.sqlite\njournalctl -u cbase-foolish -o cat --no-pager | grep -m1 "inherited socket"\n' > "$K/probe.sh"
  ssm_run $VM "$K/probe.sh"
  ```
  It prints `active active disabled`, `cbase-foolish`, and the line `the listener is the inherited socket, fd 3`.

Back, from here (the VM first, then the edge):
```
(cd "$T/foolish" && ../cbase/tools/cbase undeploy)     # the stack deleted; cbase-foolish.socket and .service stopped, disabled, removed; the data kept
printf 'set -e\nsystemctl disable --now cbase-foolish.socket cbase-foolish.service 2>/dev/null || true\nchown -R cbase:cbase /var/lib/cbase\nsystemctl enable --now cbase\nsystemctl is-active cbase\n' > "$K/back.sh"
ssm_run $VM "$K/back.sh"                                 # active
"$K/aliases.sh" add $OLD_FOOLISH foolish.cards www.foolish.cards utils.foolish.cards
"$K/aliases.sh" add $OLD_API api.foolish.cards
```
The port step comes first: as long as `cbase-foolish.socket` listens on 8080, `cbase.service` cannot bind it, and a connection would start `cbase-foolish.service` again.
If the deploy failed inside its SSM command (`cbase.service` already stopped, the service not up), run `back.sh` alone first: the API is back on `cbase.service` at once, and nothing at the edge changed yet.

## 7. uttt and utils

```
(cd "$T/uttt" && ../cbase/tools/cbase deploy) 2>&1 | tee "$K/deploy-uttt.txt" | tail -3
(cd ~/Dev/foolish-utils && "$T/cbase/tools/cbase" -f "$T/utils/cbase.conf" deploy) 2>&1 | tee "$K/deploy-utils.txt" | tail -3
```

Check: they end `deployed uttt: https://uttt.live/` and `deployed utils: https://utils.foolish.cards/`.
Back: `(cd "$T/uttt" && ../cbase/tools/cbase undeploy)` and `(cd ~/Dev/foolish-utils && "$T/cbase/tools/cbase" -f "$T/utils/cbase.conf" undeploy)`, then the `add` lines of step 5 for `$OLD_UTTT` and `$OLD_FOOLISH`.

## 8. Verify: the hostnames are on the new distributions (THE DOWNTIME ENDS)

No DNS changes: the records still name the old `cloudfront.net` names, and the edge serves each hostname from the distribution that holds it.
Two proofs that the new distributions answer: which distribution holds each alias, and behaviour only the new ones have.

```
for s in foolish uttt utils; do aws cloudformation describe-stacks --stack-name cbase-app-$s --query 'Stacks[0].Outputs[?ends_with(OutputKey,`Id`)].[OutputKey,OutputValue]' --output text; done
aws cloudfront list-distributions --query 'DistributionList.Items[?Aliases.Quantity > `0`].[Id,join(`,`,Aliases.Items)]' --output text
```
Check: the six hostnames sit on the new stacks' distribution ids: foolish.cards and www on `cbase-app-foolish`'s `DistributionId`, api on its `ApiDistributionId`, utils on `cbase-app-utils`'s, and uttt.live and www on `cbase-app-uttt`'s.
None of the six is on `$OLD_FOOLISH`, `$OLD_UTTT` or `$OLD_API`.

```
curl -s -o /dev/null -w '%{http_code}\n' https://uttt.live/234                  # 200 (was 302): the new router serves the shell; the page goes to /243 itself
curl -s -o /dev/null -w '%{http_code}\n' https://www.uttt.live/                  # 200
curl -s https://foolish.cards/abc/def | grep -c game_id                         # 1 or more (was 0): the apex reaches the new router
curl -s https://www.foolish.cards/abc/def | grep -c game_id                     # 1 or more
curl -s -o /dev/null -w '%{http_code}\n' https://utils.foolish.cards/no-such-page   # 200 (was 404)
curl -s https://api.foolish.cards/health                                        # ok cbase ...
curl -sI https://api.foolish.cards/v1/leaderboard | grep -i '^server-timing'    # present
for p in / /dashboard /privacy /privacy/ /support; do echo "$p $(curl -s -o /dev/null -w '%{http_code} %{content_type}' https://foolish.cards$p)"; done   # 200 text/html each
curl -sI https://foolish.cards/.well-known/apple-app-site-association | grep -i -e '^HTTP' -e '^content-type'   # 200, application/json
curl -s -o /dev/null -w '%{http_code}\n' https://foolish.cards/no-such.js       # 404
curl -s https://foolish.cards/m/1x | grep -o '<title>[^<]*'                     # the unfurl's title, from the VM
```
api has no behaviour of its own to tell the two apart, but its old distribution no longer holds the name (step 5), so an answer at all is the new one.
Then by hand: open https://foolish.cards, sign in, play a move against a bot, open a replay link, open https://uttt.live/234 in a browser (it lands on /243), run `cd "$T/foolish" && npm run test:validate`, and tap a foolish.cards link on a phone.
Within a minute of the first write, `printf 'journalctl -u cbase-foolish -o cat --no-pager | grep -m1 "backed up"\n' > "$K/b.sh"; ssm_run $VM "$K/b.sh"` shows a `backed up to ...` line.
If a check fails, the way back is step 9, never a DNS change.

## 9. Rollback, from any point after step 5

Every old stack, distribution, bucket and parameter is kept.
In order:

0. CI deploys off, so no merge puts `./cbase` back during the rollback: `gh variable delete CBASE_DEPLOY` (every `c_base` run is then a dry run; c_base/RUNBOOK.md 13).
1. The new stacks gone (each is a no-op for an app not deployed yet):
   ```
   (cd "$T/foolish" && ../cbase/tools/cbase undeploy)
   (cd "$T/uttt" && ../cbase/tools/cbase undeploy)
   (cd ~/Dev/foolish-utils && "$T/cbase/tools/cbase" -f "$T/utils/cbase.conf" undeploy)
   ```
2. **The port first**, then the old unit:
   `ssm_run $VM "$K/back.sh"` (step 6: `cbase-foolish.socket` and `.service` disabled and stopped, `/var/lib/cbase` back to `cbase`, `cbase.service` enabled and started) prints `active`.
3. The aliases back: the three `add` lines of step 5.
4. Check: `curl -s -o /dev/null -w '%{http_code}\n' https://uttt.live/234` is 302 again, `curl -s https://foolish.cards/abc/def | grep -c game_id` is 0, and `curl -s https://api.foolish.cards/health` is `ok cbase`.
5. The old deploy lanes back, if CI should deploy `cbase.service` again: revert the PR that made `c_base.yml`'s `deploy` job run `./cbase` (it removed `web_aws.yml`'s `deploy` job and the old jobs' role rights), apply the old `github_oidc.yaml` it restores (c_base/RUNBOOK.md 13, "The role"), and leave `CBASE_DEPLOY` unset.
   Until then, `vm_release.sh` and `web_deploy.sh` run by hand with an operator's credentials.

No data is lost either way: both units serve the same file and back it up to the same key.
Step 1's bucket statement can stay; it only adds a reader.

## 10. After it holds

- After an instance replacement, run step 6 again (the new instance boots `cbase.service` and restores the newest backup into its empty disk).
- The old distributions stay as they are, without aliases, until Alex says they go; `cbase-web`'s bucket and the VM stack stay, since cbase runs on them.
- CI deploys through `./cbase` again: on every push to `main` that touches a deployed path, `c_base.yml`'s `deploy` job runs step 6's command for foolish (with the run's own server build and export) and step 7's for uttt, after every gate, then checks `https://api.foolish.cards/health`, `https://foolish.cards/` and `https://uttt.live/` (c_base/RUNBOOK.md 13).
  It never passes `--accept-drift`, `--allow-downtime` or `--prune`, so a refusal fails the job and is fixed by hand.
  utils stays a hand deploy from `foolish-utils` (step 7).
- A run is live only when the repository variable `CBASE_DEPLOY` is `live`; otherwise it is `./cbase deploy --dry-run`.
  The dry run by hand: `gh workflow run c_base.yml --ref main -f dry_run=true`.
- Rolling back a bad deploy: `gh run rerun <run id>` on the `c_base` run of the commit to go back to (it rebuilds and deploys that commit), or steps 6 and 7 by hand from a checkout of it.
  Off `./cbase` entirely: step 9.
