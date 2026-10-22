#!/bin/bash
# mutation_check.sh - breaks the code each test covers, one mutation at a time, and confirms the
# test goes red for it, then restores the file from a copy (never from git) and removes its object,
# so the next build recompiles it whatever the clock says (a restored file with its old mtime left
# the mutated object in place once, and make's one-second mtime granularity let a mutation made
# within the second of the previous build go unbuilt: two tests looked green that were not). Run from lichess_ws/server with Redis and MongoDB up
# (the smoke needs them). Prints one line per mutation; exits 1 when one stayed green.
set -u
cd "$(dirname "$0")/.."
fail=0
mutate() {   # <file> <test program> <sed expression (-E)> <label>
  local f=$1 t=$2 expr=$3 label=$4
  cp "$f" "$f.orig"
  sed -i '' -E "$expr" "$f"
  rm -f "build/obj/rel/${f%.c}.o"   # make's 1 s mtime granularity cannot tell a file mutated, built, restored and
                                     # mutated again within one second from its object: drop the object instead
  if cmp -s "$f" "$f.orig"; then echo "MUTATION NOT APPLIED: $label"; fail=1; mv "$f.orig" "$f" && touch "$f"; return; fi
  local out rc
  if ! out=$(perl -e 'alarm shift; exec ' 600 make -j4 "build/test/$t" build/lws 2>&1); then   # a build is bounded: one spun for 12 minutes once
    echo "red (did not compile): $label"
  else
    if [ "$t" = smoke ]; then out=$(build/test/smoke build/lws build/wsbench 2>&1); rc=$?; else out=$(build/test/$t 2>&1); rc=$?; fi
    if [ $rc -ne 0 ]; then echo "red: $label ($(echo "$out" | grep -c '^FAIL') failing checks)"; else echo "STILL GREEN: $label"; fail=1; fi
  fi
  mv "$f.orig" "$f" && touch "$f"
  rm -f "build/obj/rel/${f%.c}.o"
}
# unit_json_wire
mutate src/json_wire.c unit_json_wire 's/return n == 4 \|\| strchr\("qrbnk", u\[4\]\) != NULL;/return true;/' "uci promotion letter unchecked"
mutate src/json_wire.c unit_json_wire 's/long v = strtol\(mt, &end, 36\);/long v = strtol(mt, \&end, 10);/' "move time not base 36"
mutate src/json_wire.c unit_json_wire 's/if \(end >= n \|\| end == body\)/if (end >= n)/' "empty dests object stripped"
mutate src/json_wire.c unit_json_wire 's/\*wc = atoi\(w\);/*wc = atoi(w) + 1;/' "clock misread"
mutate src/json_wire.c unit_json_wire 's/winner < 0 \? "\\",\\"win\\":null\}\}"/winner < 0 ? "\\",\\"win\\":\\"-\\"}}"/' "finish winner null form"
# unit_lila_wire
mutate src/lila_wire.c unit_lila_wire 's/if \(s == INT32_MIN\) return 0;/if (s == INT32_MIN) return 1;/' "Int.MinValue lane"
mutate src/lila_wire.c unit_lila_wire 's/h = 31u \* h \+ \(uint8_t\)msg\[i\];/h = 33u * h + (uint8_t)msg[i];/' "hash multiplier"
mutate src/lila_wire.c unit_lila_wire 's/if \(k == out->want - 1\) \{/if (false) {/' "last argument keeps its spaces"
mutate src/lila_wire.c unit_lila_wire "s/blur \? '\+' : '-'/blur ? '-' : '+'/" "blur sign"
# unit_bson
mutate src/bson.c unit_bson 's/case BSON_INT32: vlen = 4; break;/case BSON_INT32: vlen = 8; break;/' "int32 width"
mutate src/bson.c unit_bson 's/if \(n < 5 \|\| n > len \|\| p\[n - 1\] != 0\) return false;/if (n < 5 || n > len) return false;/' "missing terminator accepted"
# unit_smap
mutate src/smap.c unit_smap 's/if \(s->hash == h && strcmp\(s->key, key\) == 0\) return s;/if (s->hash == h) return s;/' "key compare skipped"
mutate src/smap.c unit_smap 's/m->n--;/m->n -= 0;/' "delete does not count"
# smoke
mutate src/hub.c smoke 's/if \(n != history_at\(r, 0\)->v - c->from_version\)/if (false)/' "resync hole undetected"
mutate src/hub.c smoke 's/if \(c->credits > 0\) \{ c->credits--; return true; \}/if (c->credits > 0) { return true; }/' "rate limit never counts"
mutate src/hub.c smoke 's/if \(\(e->f_troll && !c->troll\) \|\| \(e->f_owner && !c->is_player\)/if ((e->f_troll \&\& !c->troll)/' "owner-only event leaks to watchers"
mutate src/hub.c smoke 's/bool match = strcmp\(c->player_id, full_id \+ 8\) == 0;/bool match = true;/' "gone to the wrong player"
mutate src/routes.c smoke 's/if \(strcmp\(g.player_id\[i\], player_id\) == 0 && strcmp\(g.user\[i\], c->user\) == 0\) color = i;/if (strcmp(g.player_id[i], player_id) == 0) color = i;/' "player without the user check"
mutate src/routes.c smoke 's/if \(strcmp\(origin, CFG.csrf_origin\) == 0\) return true;/return true;/' "csrf origin unchecked"
mutate src/hub.c smoke 's/int n = r->hist_n < 5 \? r->hist_n : 5;/int n = 0;/' "no-version catch-up sends nothing"
mutate src/hub.c smoke "s/r->pos_turn = move_by < 0 \? 'w' : \(move_by == 0 \? 'b' : 'w'\);/r->pos_turn = 'w';/" "fen turn color"
[ $fail -eq 0 ] && echo "every mutation went red" || echo "SOME MUTATION STAYED GREEN"
make -j4 build/lws build/test/smoke > /dev/null 2>&1   # leave the tree built as it is
exit $fail
