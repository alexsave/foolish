# Chui Niu

Chui Niu (吹牛) is Liar's Dice in an iMessage thread: 2 to 6 players each hide five dice, raise bids on the whole table, and call a bid they do not believe.
It is a public-domain folk bluffing game, and the name is the game's own generic Chinese one (`LEGAL.md`).

**This is a PROOF OF CONCEPT.**
It skipped the design phase on purpose, and it has no App Store Connect record, no signing for distribution and no TestFlight build.
Every decision taken on the owner's behalf is one paragraph in [docs/DECISIONS.md](docs/DECISIONS.md), and each one can be vetoed.

Commands below run from inside `chuiniu/`.

## Layout

| Folder | What |
|---|---|
| `c/` | the C kernel (`c/src/cn_*`), its tests (`c/tests/`) and the iOS bridge (`c/ios/`, header `cn_api.h`, module `CChuiniu`) (K1) |
| `ios/` | `ChuiniuKit`, `ChuiniuMessages` (the extension) and `ChuiniuMessagesApp` (the container), from `ios/project.yml` (I1) |
| `docs/` | `DECISIONS.md`, the one decisions doc |
| `LEGAL.md` | what is safe to clone here, what is avoided, and what was not checked |

## Build and test

```
make -C c run             every C test and the bridge smoke
make -C c asan            the same under ASan + UBSan
make -C c ios-lib         ios/vendor/Chuiniu.xcframework and ios/Generated/ (Xcode)
make -C c swift-smoke     the bridge driven from Swift (a Mac)

cd ios && xcodegen generate
xcodebuild -project Chuiniu.xcodeproj -scheme ChuiniuMessagesApp -destination 'generic/platform=iOS Simulator' build
```

After any xcodegen run, `git status --short -- '*.entitlements'` must be empty.
CI: `.github/workflows/chuiniu.yml` runs `run`, `asan`, `structgen` and `datagen` on Linux; not the Xcode half.

## What is verified

To be filled in by the tie-together package, with dates and counts from real runs.
Until then, nothing here is claimed as verified.

## Legal

Read [LEGAL.md](LEGAL.md) before naming, theming or drawing anything.
