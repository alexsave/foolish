# Run book

Two recipes: the long overnight run, and how to resume from saved weights.

## Overnight: collect → train → eval → inspect

Builds, collects 80k esp-esp games, trains 35 total epochs with an
LR ladder (0.05 → 0.02 → 0.005), evaluates on never-seen seeds, and
runs the inspector on a sample game. Tees everything to `/tmp/overnight.log`.

```bash
cd /Users/alex/Dev/foolish/cnitro && make >/dev/null && {
  echo "=== $(date) starting overnight run ===" ;
  ./build/cnitro_collect --from=1 --to=80000 --pairs=esp-esp \
      --min_margin=3 --out=/tmp/overnight_corpus.bin --log_every=5000 ;
  ./build/cnitro_train --corpus=/tmp/overnight_corpus.bin \
      --out=/tmp/overnight_w.bin --epochs=15 --batch=32 --lr=0.05 --seed=1 ;
  ./build/cnitro_train --corpus=/tmp/overnight_corpus.bin \
      --in=/tmp/overnight_w.bin --out=/tmp/overnight_w.bin \
      --epochs=10 --batch=32 --lr=0.02 --seed=2 ;
  ./build/cnitro_train --corpus=/tmp/overnight_corpus.bin \
      --in=/tmp/overnight_w.bin --out=/tmp/overnight_w.bin \
      --epochs=10 --batch=32 --lr=0.005 --seed=3 ;
  echo "=== $(date) eval ===" ;
  ./build/cnitro_eval --weights=/tmp/overnight_w.bin --opp=random   --from=80001 --to=82000 ;
  ./build/cnitro_eval --weights=/tmp/overnight_w.bin --opp=espresso --from=80001 --to=82000 ;
  echo "=== $(date) inspect (sample game) ===" ;
  ./build/cnitro_inspect --weights=/tmp/overnight_w.bin --seed=80001 --opp=espresso --max=10 ;
  echo "=== $(date) done ===" ;
} 2>&1 | tee /tmp/overnight.log
```

Monitor: `tail -f /tmp/overnight.log`

Weights are saved at the end of every epoch to `/tmp/overnight_w.bin`.
Killing mid-epoch loses only the in-flight epoch.

## Resume from saved weights

Continue from where you stopped. Lower `--lr` for fine-tuning since the
model is already past its initial steep descent.

```bash
cd /Users/alex/Dev/foolish/cnitro && \
./build/cnitro_train --corpus=/tmp/overnight_corpus.bin \
    --in=/tmp/overnight_w.bin --out=/tmp/overnight_w.bin \
    --epochs=10 --batch=32 --lr=0.02 --seed=42
```

`--in` and `--out` can be the same path — weights are loaded once at
startup, then re-saved each epoch.

After resume, evaluate again:

```bash
./build/cnitro_eval --weights=/tmp/overnight_w.bin --opp=random   --from=80001 --to=82000
./build/cnitro_eval --weights=/tmp/overnight_w.bin --opp=espresso --from=80001 --to=82000
```

## Inspect the policy

Print top-K moves with probabilities at each nitro decision point in
one game. Useful for sanity-checking what the model has actually learned.

```bash
./build/cnitro_inspect --weights=/tmp/overnight_w.bin \
    --seed=12345 --opp=espresso --max=20
```
