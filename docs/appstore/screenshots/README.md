# The listing screenshots

Seven frames, in this order, uploaded to App Store Connect for version 1.1
(build 69) on 2026-09-14 into BOTH display types the record needs:
`IMESSAGE_APP_IPHONE_67` and `APP_IPHONE_67`.
ASC refuses a submission that has only the iMessage set - the plain iPhone set
is required even for a Messages-only app.

| # | title | app theme | ground |
|---|-------|-----------|--------|
| 01 | Play Дурак in Messages | dark | red |
| 02 | Up to 8 players | light | felt |
| 03 | Tap cards to play | dark | coal |
| 04 | Drag cards to play | light | red |
| 05 | Last one out is the дурак | dark | felt |
| 06 | Send your move to the chat | light | coal |
| 07 | Learn a popular Russian game | dark | red |

Two rules hold the set together, and both are about POSITION rather than about
any one frame:

- **The app theme alternates.** Odd frames are dark, even frames light. Every
  one of the ten candidate states was captured in both appearances, so a frame
  can be reordered and simply take the theme of its new slot - which is exactly
  what happened when 05 and 06 traded places.
- **The ground rotates red / felt / coal on a three-cycle**, deliberately out of
  phase with the two-cycle above, so ten cards in a row never read as one block.

Frames 08-10 of the built set ("Real Дурак rules", "Everyone plays in the
chat", "Attack, cover, pick up, or pass") exist and were cut from the listing.

The frames are composed by `ios/Tools/store/market.py`; the screenshots inside
them come from `ios/Tools/rig`. Do not retouch a frame by hand - change the
generator or re-shoot, so the next set is reproducible.
