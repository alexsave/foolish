#!/bin/bash
# Mutation check of the gbrun scripts: break one control at a time in a copy
# of the tree (build/romut/N), rebuild the ROM, run every script, and require
# at least one to fail. The mutants are tests/rom_mutants.txt.
set -u
cd "$(dirname "$0")/.." || exit 2
caught=0; missed=0; i=0
# The control: the unmutated copy must pass, or a "catch" proves nothing.
d=build/romut/0
rm -rf "$d"; mkdir -p "$d/build" "$d/shots"
cp -R src audio art tests scripts tools Makefile "$d/"
cp build/gbrun build/gd_binom.h "$d/build/"
cp shots/*_ref.png "$d/shots/"          # references a script compares against (gbrun same)
make -s -C "$d" build/durak.gb >"$d/make.log" 2>&1 && (cd "$d" && sh tests/scripts.sh >scripts.log 2>&1) \
    || { echo "control: the unmutated copy fails; see $d"; exit 1; }
echo "control: the unmutated copy passes"
while IFS='~' read -r file from to what; do
    [ -z "$file" ] && continue
    i=$((i+1))
    d=build/romut/$i
    rm -rf "$d"; mkdir -p "$d/build" "$d/shots"
    cp -R src audio art tests scripts tools Makefile "$d/"
    cp build/gbrun build/gd_binom.h "$d/build/"
    cp shots/*_ref.png "$d/shots/"      # references a script compares against (gbrun same)
    FROM="$from" TO="$to" perl -0pi -e 'my $n = s/\Q$ENV{FROM}\E/$ENV{TO}/g; die "no match\n" unless $n' "$d/src/$file" \
        || { echo "mutant $i: TEXT NOT FOUND ($what)"; missed=$((missed+1)); continue; }
    if ! make -s -C "$d" build/durak.gb >"$d/make.log" 2>&1; then
        echo "mutant $i: DOES NOT BUILD ($what)"; missed=$((missed+1)); continue
    fi
    if (cd "$d" && sh tests/scripts.sh >scripts.log 2>&1); then
        echo "mutant $i: MISSED - $what"; missed=$((missed+1))
    else
        echo "mutant $i: caught - $what ($(grep -c '^FAIL' "$d/scripts.log") scripts failed: $(grep '^FAIL' "$d/scripts.log" | sed 's/FAIL scripts\///' | tr '\n' ' '))"
        caught=$((caught+1))
    fi
done < <(grep -v "^#" tests/rom_mutants.txt)
echo "rom mutants: $caught caught, $missed missed"
[ "$missed" -eq 0 ]
