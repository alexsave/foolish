# Cutover: foolish, utils and uttt onto ./cbase

This is the runbook that moves foolish.cards, www, utils.foolish.cards, api.foolish.cards, uttt.live and www.uttt.live off the hand-built stacks and onto `./cbase` (the submodule at `cbase/`).
Nothing in it has run against live AWS yet.
Every phase says what it changes, and the last section is the rollback.

What moves, and where to:

| Today | After |
|---|---|
| VM stack `cbase-vm`, instance `i-08f26ffb900294ee8` (t4g.nano, IPv6-only, `::10`, port 8080), unit `cbase`, data `/var/lib/cbase/cbase.sqlite` | VM group `foolish` (stack `cbase-group-vm-foolish`), unit `cbase-foolish.service`, port 87xx, data `/srv/cbase/foolish/data/cbase.sqlite` |
| Env in SSM `/cbase-vm/env/*` | SSM `/cbase/foolish/*`, written to `/srv/cbase/foolish/env` (`CBASE_ENV_FILE`) |
| Bucket `cbase-web-487337210065` (`foolish/`, `utils/`, `uttt/`) | Static group `foolish` (stack `cbase-group-static-foolish`): one bucket, folders `foolish/`, `utils/`, `uttt/` |
| `cbase-web`: `E1YC4MJXHIOBQD` (foolish.cards, www, utils), `E1PSTHCGSF35HM` (uttt.live, www) | `cbase-app-foolish` (foolish.cards, www, and its `ApiDistribution` for api), `cbase-app-utils`, `cbase-app-uttt` |
| Stack `cbase`: `EUSADBN5SXTDP` (api.foolish.cards) | `cbase-app-foolish`'s `ApiDistribution` |

The configs are `foolish/cbase.conf`, `utils/cbase.conf` and `uttt/cbase.conf`.
The tool reads `./cbase.conf` and resolves the static dir and the server's Makefile against the working directory, so each app is deployed from its own directory (`-f` names another file; utils runs from the foolish-utils checkout).
In every block below, `T` is the repository's root (`~/Dev/foolish-cbase-switch`, or main once this lands) and the shell is bash: paste a block into `bash` (zsh does not word-split, and errors on an empty glob).

## 0. Gates: what must hold before phase B

Phase A changes nothing users see and can run any day once G1 holds.
Phase B (the switch) waits for every gate.

- **G1. The cbase pin.** `cbase/` must be bumped to the merge of cbase's `cli-static-gaps` (CBASE_SHELL, several hostnames in CBASE_DOMAIN, `/.well-known/` as `application/json`, the ws behaviour on `CBASE_API_HOST`).
  Until then `cbase/tools/cbase config check` refuses `foolish/cbase.conf` and `uttt/cbase.conf` at their `CBASE_DOMAIN` line.
  Check after the bump: `for d in foolish uttt; do (cd "$T/$d" && ../cbase/tools/cbase config check); done; (cd "$T/utils" && ../cbase/tools/cbase config check)`.
- **G2. Backups on the new VM.** foolish's server backs its database up every minute (`CBASE_BACKUP_BUCKET`, `c_base/providers/local/src/store/backup.c`) with the instance role.
  The cbase group role can read only `s3://cbase-state-<account>/artifacts/*` and `/cbase/*` parameters, so on the new VM every backup fails and there is no copy of the data off the disk.
  This needs a decision (cbase growing a backup bucket per VM group is the clean one; a policy attached to the group's role by hand is drift on `cbase-group-vm-foolish`).
  Until it is decided, phase B does not start: an instance lost after the switch would lose every game.
  G2 also makes the rollback data-safe (section 9).
- **G3. The origin secret.** Today CloudFront adds `X-Cbase-Origin` to every request to the VM and the server answers 403 without it (`CBASE_ORIGIN_SECRET`).
  cbase's distributions add no origin header, so `CBASE_ORIGIN_SECRET` is NOT copied in phase A: with it set, every request through the new distributions would be a 403.
  Without it the VM's ports are open to any CloudFront distribution (the security group admits CloudFront's whole origin-facing prefix list, which every account's distributions use), and the server trusts the viewer-address header from any of them.
  Decide before phase B: accept that, or wait for cbase to send an origin header from SSM.
- **G4. What the API distribution loses.** Live `EUSADBN5SXTDP` caches `/v1/leaderboard` and `/v1/bots` (policy `cbase-public-reads`), adds `Server-Timing` (response headers policy `cbase-server-timing`), and gives the VM origin 60 s read and keep-alive timeouts.
  cbase's `ApiDistribution` is one uncached behaviour with 30 s and 5 s; the ws behaviour on it is G1's.
  The two cached reads become uncached reads of the VM (fine at today's load), and `Server-Timing` goes; say yes to that or wait for cbase.
- **G5. Four distributions.** cbase makes one per app plus the API one: four, where today's three fit the account's three CloudFront Free plans (c_base/RUNBOOK.md, "Two distributions, not one").
  The fourth runs pay-as-you-go ($0 at this volume, as before the plans).
- **G6. The old deploys off.** On every push to main, `web_aws.yml`'s `deploy` job runs `web_deploy.sh` (to `cbase-web`) and `c_base.yml`'s `deploy` job runs `vm_release.sh` (to `cbase-vm`).
  Before phase B, a PR turns both off (and, when CI deploys through ./cbase, gives the GitHub role what `cbase/tools/cbase deploy` calls), and nothing merges to main during the window.

## A. Before the switch (no user-visible change)

### A1. The server binary

foolish's server is not built by cbase.mk, so `CBASE_SERVE_BIN` always names it.
It is the same build CI ships to `cbase-vm` today, from a tree that contains `origin/main`:

```
cd "$T" && c_base/providers/aws/ops/build_bootstrap.sh serve      # Docker; c_base/dist/cbase-serve-linux-arm64 and .sha256
(cd c_base/dist && shasum -a 256 -c cbase-serve-linux-arm64.sha256)
file c_base/dist/cbase-serve-linux-arm64                           # ELF 64-bit LSB ... ARM aarch64, statically linked
```

Or take main's release: `gh release download cbase-serve-<short sha> -p 'cbase-serve-linux-arm64*' -D c_base/dist`.
The binary must hold `cb_config_load` (this branch): the cbase unit passes every setting through `CBASE_ENV_FILE`, and an older binary would start without its secrets and refuse (`no CBASE_JWT_SECRET`).
The unit runs `/srv/cbase/foolish/serve serve` with `CBASE_LISTEN=0.0.0.0:<port>`, `CBASE_DB=/srv/cbase/foolish/data/cbase.sqlite` and `CBASE_ENV_FILE=/srv/cbase/foolish/env`; the server already took the first two (`c_base/core/src/config.c`), and `c_base/core/test/smoke_boot.c` starts it exactly so.

### A2. The groups (the one step that costs money)

```
cd "$T/foolish" && ../cbase/tools/cbase group-init vm foolish --yes     # a t4g.nano with an Elastic IP, about $3/month plus $3.60 for the IPv4
cd "$T/foolish" && ../cbase/tools/cbase group-init static foolish
cd "$T/foolish" && ../cbase/tools/cbase doctor
```

`doctor` prints the new instance id; it is `NEW` below (`NEW=i-...`).
Check, read-only, that the instance has what phase B's scripts use: `aws ssm describe-instance-information --filters Key=InstanceIds,Values=$NEW --query 'InstanceInformationList[0].PingStatus'` is `Online`.

### A3. The settings, SSM to SSM, no value printed

Four of the seven `/cbase-vm/env/*` parameters move now.
`CBASE_ORIGIN_SECRET` waits for G3, and `CBASE_BACKUP_BUCKET` and `CBASE_BACKUP_REGION` for G2 (with the role unable to write the bucket, the server would log `backup failed` every minute).

```
bash <<'EOF'
set -euo pipefail
umask 077
t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
for n in CBASE_JWT_SECRET CBASE_ADMIN_TOKEN CBASE_PUBLIC_URL CBASE_ALLOWED_ORIGINS; do
  type="$(aws ssm get-parameter --name "/cbase-vm/env/$n" --query Parameter.Type --output text)"
  aws ssm get-parameter --name "/cbase-vm/env/$n" --with-decryption --query Parameter.Value --output text > "$t/v"
  # --output text ends the value with one newline; take exactly that one off
  python3 -I -c 'import sys; p = sys.argv[1]; b = open(p, "rb").read(); open(p, "wb").write(b[:-1] if b.endswith(b"\n") else b)' "$t/v"
  # the cbase VM script refuses a value with a single quote or a newline (vm.sh step 4)
  if grep -q "'" "$t/v" || [ "$(wc -l < "$t/v")" -ne 0 ]; then echo "$n: has a quote or a newline, not copied"; exit 1; fi
  aws ssm put-parameter --name "/cbase/foolish/$n" --type "$type" --value "file://$t/v" --query Version --output text > /dev/null
  aws ssm get-parameter --name "/cbase/foolish/$n" --with-decryption --query Parameter.Value --output text > "$t/w"
  python3 -I -c 'import sys; p = sys.argv[1]; b = open(p, "rb").read(); open(p, "wb").write(b[:-1] if b.endswith(b"\n") else b)' "$t/w"
  cmp -s "$t/v" "$t/w" && echo "$n: copied ($type), the same value" || { echo "$n: the copy differs"; exit 1; }
done
EOF
aws ssm describe-parameters --parameter-filters Key=Path,Option=Recursive,Values=/cbase/foolish --query 'Parameters[].[Name,Type]' --output text
```

`put-parameter` without `--overwrite` refuses a name that exists, so a second run stops at the first one already copied.
The old parameters stay as they are (the rollback reads them).

### A4. The ACM certificate for foolish

No issued certificate covers foolish.cards, www.foolish.cards and api.foolish.cards together (today's are one for the site and one for api), and the cbase stack serves both of its distributions with one.
Request it by hand now so phase B does not wait on DNS:

```
aws acm request-certificate --region us-east-1 --domain-name foolish.cards \
  --subject-alternative-names www.foolish.cards api.foolish.cards --validation-method DNS --query CertificateArn --output text
aws acm describe-certificate --region us-east-1 --certificate-arn <arn> \
  --query 'Certificate.DomainValidationOptions[].[DomainName,ResourceRecord.Name,ResourceRecord.Value,ValidationStatus]' --output text
```

ACM gives one account the same validation record for the same name, and foolish.cards, www and api already have theirs at Squarespace (c_base/RUNBOOK.md W1), so it should issue in minutes with no new record; add any record it lists that Squarespace does not have.
`cbase deploy` finds it (an ISSUED certificate that covers every hostname).
utils (`42ecb64e...`, which names utils.foolish.cards) and uttt (`6f0b38fc...`) are covered by today's certificates.

### A5. The apps, on their cloudfront.net names

The live distributions hold every hostname, and CloudFront refuses an alias another distribution holds, so the first deploy of each app leaves its hostnames out (a copy of its conf without `CBASE_DOMAIN` and `CBASE_API_HOST`).
The stacks are the same ones phase B updates.

```
bash <<'EOF'
set -euo pipefail
T=~/Dev/foolish-cbase-switch
pre() { grep -v -e '^CBASE_DOMAIN' -e '^CBASE_API_HOST' "$1" > "$2"; }
(cd "$T/foolish" && npm run build)
(cd "$T/uttt/web" && npm ci && npm run build)
(cd ~/Dev/foolish-utils && npm run build)
pre "$T/foolish/cbase.conf" /tmp/foolish.pre.conf
pre "$T/uttt/cbase.conf" /tmp/uttt.pre.conf
pre "$T/utils/cbase.conf" /tmp/utils.pre.conf
(cd "$T/foolish" && CBASE_SERVE_BIN=../c_base/dist/cbase-serve-linux-arm64 ../cbase/tools/cbase -f /tmp/foolish.pre.conf deploy --dry-run)
(cd "$T/foolish" && CBASE_SERVE_BIN=../c_base/dist/cbase-serve-linux-arm64 ../cbase/tools/cbase -f /tmp/foolish.pre.conf deploy)
(cd "$T/uttt" && ../cbase/tools/cbase -f /tmp/uttt.pre.conf deploy)
(cd ~/Dev/foolish-utils && "$T/cbase/tools/cbase" -f /tmp/utils.pre.conf deploy)
EOF
```

A dry run makes a change set and deletes it, and on a first deploy it leaves and deletes an empty stack: it is a CloudFormation call, not a read.
The foolish service now runs on the new VM with an EMPTY database (no backup bucket to restore from); no hostname reaches it, and phase B replaces the file.
Write down the three `cloudfront.net` names it prints: `FD` (foolish), `UD` (utils), `TD` (uttt).
Check the static side on them:

```
for p in / /dashboard /privacy /abc123 /abc123/deeper /.well-known/apple-app-site-association /no-such.js; do
  echo "$p $(curl -s -o /dev/null -w '%{http_code} %{content_type}' "https://$FD$p")"
done
# / /dashboard /privacy: 200 text/html; /abc123 and /abc123/deeper: 200 (the shell; the app shows its own not-found for the deeper one)
# the association file: 200 application/json; /no-such.js: 404
curl -s -o /dev/null -w '%{http_code}\n' "https://$TD/243"                # 200
curl -s "https://$TD/234" | grep -c _shell                                 # the shell; the browser goes to /243 (web/lib/shell.ts)
curl -s -o /dev/null -w '%{http_code}\n' "https://$UD/"                   # 200
curl -s "https://$FD/m/1x" | grep -o '<title>[^<]*'                       # the unfurl, from the new VM
```

And the service, on the instance (through SSM; `ssm_run` is in section 8):

```
printf 'systemctl is-active cbase-foolish\ncurl -s http://127.0.0.1:$(sed -n "s/.*CBASE_LISTEN=0.0.0.0://p" /etc/systemd/system/cbase-foolish.service)/health\n' > /tmp/probe.sh
ssm_run "$NEW" /tmp/probe.sh       # active, then "ok cbase ..."
```

### A6. The data move, rehearsed off-prod

Rehearsed on 2026-11-09 from the newest backup, `s3://cbase-vm-backupbucket-whuc9g4lhome/cbase/latest.sqlite` (version `WnYChsnEsGeaKPBdJ7LXY9nOzf6J4aMt`, 2026-11-09T21:20:57Z, 606,208 bytes, sha256 `5101973c...427312ef`, schema `user_version` 10).
It was started on the Mac with this branch's server exactly as the cbase unit starts it (`env -i`, a temporary HOME, `CBASE_LISTEN=127.0.0.1:8711`, `CBASE_DB=<scratch>/srv/data/cbase.sqlite`, `CBASE_ENV_FILE=<scratch>/srv/env` with throwaway secrets and no backup bucket, so nothing was uploaded):

| | games | ratings | leaderboard (ratings with games_played > 0) | users | replays | integrity_check |
|---|---|---|---|---|---|---|
| the backup | 220 | 33 | 2 | 39 | 175 | ok |
| after serving | 220 | 33 | 2 | 39 | 175 | ok |

It listened on the unit's address and answered `GET /health` 200 (`ok cbase 0.1.0 mode=serve`), `/ready` 200, `/v1/leaderboard` 200 (1,349 bytes) and `/v1/bots` 200.
There is no `leaderboard` table: the leaderboard is the played ratings (`S_RATINGS_PLAYED`, server/src/store/rows_sql.c) and the bots.
Phase B's counts are held against the numbers its own copy prints, not these: the game keeps writing until B1.
B1's copy-and-count step (its Python, as written below) ran on a copy of the same backup and printed the same counts and `integrity ok`, and B4's config edit ran on a stand-in config; the signed upload and the SSM runs have not run anywhere yet.

## B. The switch (the outage window)

Lower the TTLs to 300 at Squarespace a day ahead (www.foolish.cards is 14400 today; foolish.cards and uttt.live 60, api 300).
Announce the window; the API is down from B1 to B6, and the sites from B5 to B7, about 20 to 30 minutes in all.

### B1. Stop writes and take the copy, on the old VM

Stopping the old service is the write freeze: nothing else writes the database.
The copy is SQLite's online backup API (`sqlite3 .backup` is the same call), through `python3`, which the minimal AL2023 image has (the boot script uses it); the image has no `sqlite3` or `aws` command, so the upload is signed with `curl --aws-sigv4` and the role's credentials from IMDSv2, as `vm_boot.sh` does.
The role can write the backup bucket.

```
cat > /tmp/b1.sh <<'EOF'
set -euo pipefail
umask 077
systemctl stop cbase
ts="$(date -u +%Y%m%dT%H%M%SZ)"
out="/var/lib/cbase/cutover-$ts.sqlite"
python3 - /var/lib/cbase/cbase.sqlite "$out" <<'PY'
import sqlite3, sys
src = sqlite3.connect(sys.argv[1]); dst = sqlite3.connect(sys.argv[2])
src.backup(dst)
print('CUTOVER integrity', dst.execute('PRAGMA integrity_check').fetchone()[0])
for t in ('games', 'ratings', 'users', 'replays'):
    print('CUTOVER', t, dst.execute('SELECT count(*) FROM ' + t).fetchone()[0])
print('CUTOVER leaderboard', dst.execute('SELECT count(*) FROM ratings WHERE games_played > 0').fetchone()[0])
dst.close(); src.close()
PY
sha="$(sha256sum "$out" | cut -d' ' -f1)"
IMDS='http://[fd00:ec2::254]'
tok="$(curl -sSf -X PUT "$IMDS/latest/api/token" -H 'X-aws-ec2-metadata-token-ttl-seconds: 300')"
md() { curl -sSf -H "X-aws-ec2-metadata-token: $tok" "$IMDS/latest/$1"; }
role="$(md meta-data/iam/security-credentials/)"
creds="$(md "meta-data/iam/security-credentials/$role")"
f() { python3 -c 'import json,sys; print(json.load(sys.stdin)[sys.argv[1]])' "$1" <<< "$creds"; }
key="cutover/cbase-$ts.sqlite"
curl -sSf --aws-sigv4 "aws:amz:us-east-1:s3" --user "$(f AccessKeyId):$(f SecretAccessKey)" \
  -H "x-amz-security-token: $(f Token)" -H "x-amz-content-sha256: $sha" \
  -T "$out" "https://cbase-vm-backupbucket-whuc9g4lhome.s3.dualstack.us-east-1.amazonaws.com/$key" -o /dev/null
echo "CUTOVER key $key sha256 $sha"
EOF
ssm_run i-08f26ffb900294ee8 /tmp/b1.sh
```

It must print `integrity ok`, the counts, and the key and sha256; write them down.
The old service stays stopped (`Restart=always` does not restart a stopped unit); a reboot of the old instance would start it again, so nothing reboots it during the window.
Rehearse the upload half once in phase A on a throwaway file (the same script with `systemctl stop` and the backup replaced by `head -c 1000 /dev/urandom > "$out"`, and the key under `cutover/probe-`): it is the one step here no test has run.

### B2. Verify the copy on the Mac

```
bash <<'EOF'
set -euo pipefail
d="$(mktemp -d)"; echo "$d"
aws s3api get-object --bucket cbase-vm-backupbucket-whuc9g4lhome --key "$KEY" "$d/cbase.sqlite" --query ContentLength --output text
shasum -a 256 "$d/cbase.sqlite"                     # = B1's sha256
sqlite3 "$d/cbase.sqlite" 'PRAGMA integrity_check'   # ok
sqlite3 "$d/cbase.sqlite" "SELECT (SELECT count(*) FROM games),(SELECT count(*) FROM ratings),(SELECT count(*) FROM ratings WHERE games_played>0),(SELECT count(*) FROM users),(SELECT count(*) FROM replays)"   # = B1's
aws s3api put-object --bucket cbase-state-487337210065 --key "artifacts/foolish/cutover-$SHA" --body "$d/cbase.sqlite" --query ETag --output text
EOF
```

The new VM's role reads `artifacts/*` of the state bucket and nothing else, so the copy goes there for B3; B3 deletes it.

### B3. Restore it to /srv/cbase/foolish/data

```
cat > /tmp/b3.sh <<EOF
set -euo pipefail
umask 077
d=/srv/cbase/foolish/data
systemctl stop cbase-foolish
aws s3api get-object --region us-east-1 --bucket cbase-state-487337210065 --key artifacts/foolish/cutover-$SHA "\$d/restore.sqlite" > /dev/null
echo "$SHA  \$d/restore.sqlite" | sha256sum -c --quiet -
mkdir -p "\$d/before-cutover"
for f in cbase.sqlite cbase.sqlite-wal cbase.sqlite-shm; do if [ -e "\$d/\$f" ]; then mv "\$d/\$f" "\$d/before-cutover/"; fi; done
mv "\$d/restore.sqlite" "\$d/cbase.sqlite"
chown -R cbase-foolish:cbase-foolish "\$d"
chmod 0600 "\$d/cbase.sqlite"
python3 -c 'import sqlite3; c = sqlite3.connect("file:/srv/cbase/foolish/data/cbase.sqlite?mode=ro", uri=True); print("CUTOVER integrity", c.execute("PRAGMA integrity_check").fetchone()[0])'
systemctl start cbase-foolish
port="\$(sed -n 's/.*CBASE_LISTEN=0.0.0.0://p' /etc/systemd/system/cbase-foolish.service)"
n=0; until [ "\$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:\$port/health")" = 200 ] || [ \$n -gt 30 ]; do sleep 1; n=\$((n+1)); done
echo "CUTOVER health \$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:\$port/health") leaderboard \$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:\$port/v1/leaderboard")"
EOF
ssm_run "$NEW" /tmp/b3.sh
aws s3api delete-object --bucket cbase-state-487337210065 --key "artifacts/foolish/cutover-$SHA"
```

`before-cutover/` keeps the empty database A5 made, for a look; delete it once the switch has held.

### B4. The old aliases off

A hostname can sit on one distribution only, so the old three let theirs go first.
They keep everything else, their certificates among it (a distribution may hold one and no alias), so the rollback only puts the aliases back.
The config holds the origin secret as a header value: it goes to a private temporary file, edited, and is never printed.

```
cat > /tmp/drop_aliases.sh <<'EOF'
#!/bin/bash
# drop_aliases.sh ID: the distribution's aliases removed, nothing else changed; waits until it is deployed.
set -euo pipefail
umask 077
t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
aws cloudfront get-distribution-config --id "$1" > "$t/all"
python3 -I - "$t/all" "$t/cfg" "$t/etag" <<'PY'
import json, sys
a = json.load(open(sys.argv[1]))
open(sys.argv[3], 'w').write(a['ETag'])
c = a['DistributionConfig']
print(sys.argv[1].split('/')[-1], 'aliases were:', ' '.join(c['Aliases'].get('Items', [])), file=sys.stderr)
c['Aliases'] = {'Quantity': 0}
json.dump(c, open(sys.argv[2], 'w'))
PY
aws cloudfront update-distribution --id "$1" --if-match "$(cat "$t/etag")" --distribution-config "file://$t/cfg" --query Distribution.Status --output text
aws cloudfront wait distribution-deployed --id "$1"
echo "$1: no aliases"
EOF
bash /tmp/drop_aliases.sh E1YC4MJXHIOBQD    # foolish.cards www.foolish.cards utils.foolish.cards
bash /tmp/drop_aliases.sh E1PSTHCGSF35HM    # uttt.live www.uttt.live
bash /tmp/drop_aliases.sh EUSADBN5SXTDP     # api.foolish.cards
```

These are hand edits of `cbase-web` and `cbase` (the latter was hand-edited at the VM cutover already), so both stacks drift; they are kept, not updated, and the rollback edits them back by hand.

### B5. The apps on their hostnames

`G2`'s and `G3`'s parameters go in first if they were decided for it (`/cbase/foolish/CBASE_BACKUP_BUCKET` and `CBASE_BACKUP_REGION` naming a bucket the role can write; `CBASE_ORIGIN_SECRET` only with an origin header from cbase), as in A3.

```
(cd "$T/foolish" && CBASE_SERVE_BIN=../c_base/dist/cbase-serve-linux-arm64 ../cbase/tools/cbase deploy)
(cd "$T/uttt" && ../cbase/tools/cbase deploy)
(cd ~/Dev/foolish-utils && "$T/cbase/tools/cbase" -f "$T/utils/cbase.conf" deploy)
```

Each finds its certificate, updates its stack (the aliases, and for foolish the `ApiDistribution`, a new resource), and, with no Route 53 zone for these domains, prints the record that points each hostname at its distribution.
The foolish deploy restarts `cbase-foolish` on the same binary and the restored database (its port and data folder are kept).

### B6. The DNS records (Squarespace)

Squarespace > Domains > the domain > DNS > DNS Settings; the Host field is the name without the domain.
The targets are what B5 printed (`FD`, `AD` for the `ApiDistributionDomain` output, `UD`, `TD`):

| Domain | Host | Type | Data |
|---|---|---|---|
| foolish.cards | `@` | ALIAS | `FD` |
| foolish.cards | `www` | CNAME | `FD` |
| foolish.cards | `api` | CNAME | `AD` |
| foolish.cards | `utils` | CNAME | `UD` |
| uttt.live | `@` | ALIAS | `TD` |
| uttt.live | `www` | CNAME | `TD` |

The ACM validation CNAMEs stay (ACM renews against them); `vm-origin.foolish.cards` stays until the old VM is retired (the rollback uses it).
`aws cloudformation describe-stacks --stack-name cbase-app-foolish --query 'Stacks[0].Outputs' --output text` prints `FD` and `AD` again.

### B7. Verify

Before the records have spread, force the address with `--connect-to`; after, drop it:

```
bash <<'EOF'
c() { curl -s -o /dev/null -w '%{http_code} %{content_type}' --connect-to "$1:443:$2:443" "https://$1$3"; echo "  $1$3"; }
for p in / /dashboard /privacy /imessage-privacy /support /abc123 /abc123/deeper /.well-known/apple-app-site-association /no-such.js; do c foolish.cards "$FD" "$p"; done
c www.foolish.cards "$FD" /
c utils.foolish.cards "$UD" /
c uttt.live "$TD" /243
c www.uttt.live "$TD" /
c api.foolish.cards "$AD" /health
c api.foolish.cards "$AD" /v1/leaderboard
curl -s --connect-to "api.foolish.cards:443:$AD:443" https://api.foolish.cards/health; echo
curl -s --connect-to "foolish.cards:443:$FD:443" https://foolish.cards/m/1x | grep -o '<title>[^<]*'
curl -sI -H 'Accept-Encoding: gzip, br' --connect-to "foolish.cards:443:$FD:443" https://foolish.cards/oracle.wasm.gz | grep -ci content-encoding   # 0
EOF
```

Every page is a 200 (`text/html`), the association file a 200 `application/json` with no redirect, `/no-such.js` a 404, `/health` `ok cbase ...`, and the unfurl has its title.
Then, by hand: open https://foolish.cards, sign in, play a move against a bot, see the leaderboard; open a replay link; open https://uttt.live/234 (it lands on /243); run `cd foolish && npm run test:validate` (universal_links_validation); tap a foolish.cards link on a phone and see the app open; and watch `journalctl -u cbase-foolish` on the instance (`aws ssm start-session --target $NEW`) for a few minutes of real traffic.
When the G2 backup is on, its `backed up to ... in N ms` line shows within a minute of the first write.

## 8. The SSM helper

`ssm_run INSTANCE FILE` runs FILE as root on the instance through Run Command, waits with a bounded loop, and prints its output.
The script travels base64 in one command with no comma (the CLI's shorthand splits lists on commas), as cbase's `vm_ssm_run` does.
Its output is printed whole, so a script given to it never prints a setting.

```
ssm_run() {
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

## 9. Rollback

Every old stack, bucket, parameter and the old instance are kept, so the way back is the aliases and DNS reversed and the old service started.

1. The new distributions let the hostnames go: the A5 deploys again (the confs without `CBASE_DOMAIN` and `CBASE_API_HOST`), which update the stacks to no aliases and delete the `ApiDistribution`.
2. The old aliases back, by hand, with `drop_aliases.sh`'s edit reversed (`c['Aliases'] = {'Quantity': N, 'Items': [...]}`): `E1YC4MJXHIOBQD` foolish.cards, www.foolish.cards, utils.foolish.cards; `E1PSTHCGSF35HM` uttt.live, www.uttt.live; `EUSADBN5SXTDP` api.foolish.cards.
   Their certificates never left them.
3. The Squarespace records back: `@` ALIAS and `www` and `utils` CNAMEs to `dgow77nr7v5kf.cloudfront.net`, `api` CNAME to `d3rcf2zypd78ll.cloudfront.net`, uttt.live `@` ALIAS and `www` CNAME to `d2hb0vxf10cj0w.cloudfront.net`.
4. The old service started: `printf 'systemctl start cbase\nsystemctl is-active cbase\n' > /tmp/start.sh; ssm_run i-08f26ffb900294ee8 /tmp/start.sh`, then `c_base/providers/local/ops/vm_release.sh --verify` from a tree that contains main.
5. The data: the old database is as B1 left it.
   Writes made on the new VM since B5 are lost unless G2 put the new server's backups in a bucket the old one can read: then, before step 4, copy that object over `/var/lib/cbase/cbase.sqlite` as VM_RUNBOOK.md's "To restore by hand" says.
   Without G2 the new VM cannot send its file anywhere (its role writes no bucket), which is one more reason G2 gates the switch.
6. Turn G6's deploy jobs back on.

The new groups and stacks can stay for another try, or go: `undeploy` per app (it keeps the files and the data), then `group-destroy`.

## 10. After it holds

- Retire the old side only when Alex says so, and in this order: `cbase` (the Lambda-era API stack), `cbase-web`, then `cbase-vm` (its backup bucket is retained by the stack; keep it while it holds the only history).
- Delete `/cbase-vm/env/*` with the stack, as VM_RUNBOOK.md says, and `vm-origin.foolish.cards` at Squarespace.
- `cutover/` in the backup bucket holds a copy of every user's data: delete it once the new backups run.
- Remove `c_base/core/src/env_file.[ch]` and `c_base/core/test/unit_env_file.c` when foolish's server builds through cbase.mk (they are cbase's, copied), and with them the `CBASE_SERVE_BIN` line of `foolish/cbase.conf`.
