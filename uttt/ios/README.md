# The app

```
make -C ../c ios-lib          # writes ios/vendor/Uttt.xcframework (needs Xcode)
xcodegen generate             # the .xcodeproj is an artifact, never committed
xcodebuild -scheme UtttPreview -destination 'id=<sim udid>' build
```

`UtttPreview` is a development harness and ships in nothing. It shows the game
screen at the three sizes Messages gives, because the only other way to look at
one is to drive the real Messages app - and a design that can only be inspected
by playing a game is a design nobody inspects.

```
xcrun simctl boot <udid>
xcrun simctl install <udid> "$(find ~/Library/Developer/Xcode/DerivedData/Uttt-*/Build/Products/Debug-iphonesimulator -name UtttPreview.app -maxdepth 1 | head -1)"
xcrun simctl launch <udid> cards.uttt.preview
xcrun simctl io <udid> screenshot out.png
```

## What Swift does not do

It computes no coordinate. `UtttKernel.swift` forwards, `UtttBoardView` fills
the polygons that come back, and `UtttModel` remembers which answer is on
screen. If any of them ever calculates something instead of asking the kernel,
that is the same class of bug as a hand-written byte reader - see
`docs/ARCHITECTURE_AS_A_PATTERN.md` in the repo root.

The pen is in the kernel because two phones looking at one bubble have to
produce the same sheet stroke for stroke. The moment the geometry lives in a
renderer, "the bubble IS the game" becomes "the bubble is a picture of it".

## Two build failures that read as something else

**"cannot find uti_draw_last in scope"** is a stale xcframework, not a missing
symbol. The header in DerivedData moved on and the precompiled module did not.
`rm -rf ~/Library/Developer/Xcode/DerivedData/Uttt-*` and rebuild `ios-lib`.

**"ignoring file ... found architecture arm64, required architecture x86_64"**,
followed by forty undefined symbols, is a one-arch simulator slice.
`-destination 'generic/platform=iOS Simulator'` builds both, so the sim library
is lipo'd from arm64 and x86_64 in `../c/Makefile`.

## Not done yet

- The bubble. `MSMessageTemplateLayout` is baked at insert, 300x195 landscape,
  and nothing writes one yet - the extension currently only shows the board.
- Sealing a move into a message. `Uttt.code` is the whole game in ~21 bytes
  and the URL round-trip is written but untested against a real thread.
- The lobby. The roster seals once two have joined and the seed is the first
  board's timestamp; none of that is implemented.
- Performance. A finished position is 14,064 polygons. It is cached as an image
  and only the moving stroke is redrawn, but that has not been measured on a
  device.
