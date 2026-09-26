# Ultimate Tic-Tac-Toe - App Store Connect record, submission checklist

The live state of App Store Connect record 6815039449 (`cards.uttt.msg`), version 1.0, audited against the API on 2026-09-26.
`uttt/docs/APP_STORE.md` holds the reasoning and the drafted copy; this file is what is actually filed and what is left.
Legend: [x] already set, [x] **set 2026-09-26** (by the API audit), [ ] owner step.
Nothing was submitted for review, no build was attached, and no screenshot was uploaded.

## App Information (appInfo 61cf8d25-cb35-4b39-a740-d84bd443796f)

- [x] Name: `Ultimate Tic-Tac-Toe Messages`, set when the record was created.
  It is the owner's choice and is left as is; the Messages drawer still shows `Ultimate` (`CFBundleDisplayName`).
- [x] **set 2026-09-26** Subtitle: `Tic-tac-toe with a twist` (24 of 30), from `APP_STORE.md` section 2.
- [x] Primary category Games, subcategories Board and Strategy.
- [x] Primary language English (U.S.), SKU `UTTTMSG1`.
- [x] Privacy Policy URL: `https://uttt.live/privacy-msg` (live, 200, plain HTML, says the app collects no data).
- [x] Age rating: every content question None, every capability question No, computed rating 4+ (Korea ALL, Brazil L).
- [x] **set 2026-09-26** Content rights: "does not use third-party content", as foolish's iMessage app files it (no bundled fonts, audio or art from others).
- [x] App Privacy (nutrition label): "Data Not Collected", published by the owner 2026-09-26.

## Version 1.0 (appStoreVersion 31311df3-71e8-4d94-9273-fb5405294875)

- [x] **set 2026-09-26** Description (966 characters), from `APP_STORE.md` section 3; every feature it names was checked in the code (Copy code, Again, the highlighter, the replay prefix `https://uttt.live/`).
- [x] **set 2026-09-26** Promotional text (153 of 170).
- [x] **set 2026-09-26** Keywords (98 of 100): `tic tac toe,ultimate,super tic tac toe,noughts and crosses,board game,strategy,two player,imessage`.
- [x] **set 2026-09-26** Support URL: `https://uttt.live/support-msg` (live, 200, contact alexvsaveliev@gmail.com).
- [x] **set 2026-09-26** Marketing URL: `https://uttt.live/about` (live, 200).
- [x] **set 2026-09-26** Copyright: `2026 Alexander Saveliev`, the form foolish's 1.1 record uses.
- [x] What's New: not asked on a first version, left empty.
- [x] Release: automatically after approval (`AFTER_APPROVAL`); change it on the version page if a manual release is wanted.
- [ ] Screenshots: none uploaded yet (another agent is shooting them).
  Both sets are needed, as foolish learned: iMessage App > iPhone 6.9" (`IMESSAGE_APP_IPHONE_67`) and iPhone 6.9" (`APP_IPHONE_67`), 1320x2868 portrait, 3 to 5 each.
  See `docs/appstore/screenshots/README.md` at the repo root.

## Build

- [ ] Build: none attached, and 1.0(11) must not be the one.
  1.0(11) was uploaded 2026-09-25 15:11 UTC, before the privacy manifests (`d07c2ec1`, 18:47 UTC) and before the seat claim became DEBUG only (`798bf109`, 23:31 UTC).
  So 1.0(11) ships the Claim O / Claim X buttons in Release, a hidden seat takeover and a Guideline 2.3.1 risk.
- [ ] Two open device defects from `TESTFLIGHT_PLAN.md` section 9 are not fixed at HEAD, and a reviewer following the notes can hit both.
  `didStartSending` still does not bump `stageGeneration`, so a sent move can come back as a staged one.
  `again()` still sets `freshSession = true` (`MessagesViewController.swift:755`), so the open drawer never hears the reply to an Again invitation; the review notes' step 6 sends the reviewer to Again.
- [ ] After both fixes: `uttt/ios/Tools/ship.sh` for 1.0(12) or later, check three `PrivacyInfo.xcprivacy` in the archive (`APP_STORE.md` section 8), wait for VALID.
- [ ] Then on the version page: Build > Add Build > pick it.
  Export compliance needs no answer: every uttt build reports `usesNonExemptEncryption = false` from `ITSAppUsesNonExemptEncryption = NO`.

## Pricing and availability

- [x] Price: Free (USD base, customer price 0.0), no in-app purchases.
- [x] Availability: 174 territories plus "available in new territories", China mainland excluded, exactly as foolish's iMessage app.
  China needs a game license number to sell a game, so leaving it out is deliberate.

## App Review information (appStoreReviewDetail aa7db900-9ea3-4e9f-810b-1bed4e02f1a2)

- [x] Contact: Alexander Saveliev, 6032194757, alexvsaveliev@gmail.com.
- [x] Sign-in required: No.
- [x] **set 2026-09-26** Notes: the text between the rules of `uttt/docs/APP_REVIEW_NOTES.md` (2,855 characters, no em dash), including the one-device replay link.
  Keep the two in step: edit the doc, then patch `notes` on this row again.
- [ ] Attachment: a two-phone screen recording of a game, the foolish pattern.
  In App Store Connect: version 1.0 > App Review Information > Attachment > upload the video.

## Last step (owner, not before every box above is ticked)

- [ ] Version 1.0 > Add for Review > Submit to App Review.
