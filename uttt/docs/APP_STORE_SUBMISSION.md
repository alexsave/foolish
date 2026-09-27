# Ultimate Tic-Tac-Toe - App Store Connect record, submission checklist

The live state of App Store Connect record 6815039449 (`cards.uttt.msg`), version 1.0, audited against the API on 2026-09-26.
`uttt/docs/APP_STORE.md` holds the reasoning and the drafted copy; this file is what is actually filed and what is left.
Legend: [x] already set, [x] **set 2026-09-26** (by the API audit), [ ] owner step.
Version 1.0 was submitted for review on 2026-09-26 through the API.
Review submission id `99f85253-ceba-4e1c-b7a9-ace6fdcb389c`, version id `31311df3-71e8-4d94-9273-fb5405294875`.
The version state is `WAITING_FOR_REVIEW`.

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
- [x] **set 2026-09-26** Screenshots: the six final frames uploaded to en-US, both display types.
  `IMESSAGE_APP_IPHONE_67` set `fd4211b9-d010-4a72-84c9-ecb888fa5fd0` and `APP_IPHONE_67` set `eba82b38-f352-4dbe-b01a-062d40fb776f`, each with `01_hero_dark.png` through `06_empty_dark.png` in that order, every asset reached `COMPLETE`.
  Other locales fall back to these en-US screenshots; no per-locale upload was done.

## Build

- [x] **set 2026-09-26** Build: 1.0(13) attached to version 1.0 (`PATCH appStoreVersions/relationships/build`), the latest VALID build.
  Export compliance needs no answer: build 13 reports `usesNonExemptEncryption = false`.
- [ ] The two open device defects from `TESTFLIGHT_PLAN.md` section 9 (the `didStartSending`/`stageGeneration` staged-move bug and `again()` setting `freshSession = true`) were not re-verified as fixed in build 13 by this pass.
  The owner explicitly authorized shipping 1.0(13) as the build to submit; check `TESTFLIGHT_PLAN.md` section 9 against HEAD if these need to be confirmed fixed after the fact.

## Pricing and availability

- [x] Price: Free (USD base, customer price 0.0), no in-app purchases.
- [x] Availability: 174 territories plus "available in new territories", China mainland excluded, exactly as foolish's iMessage app.
  China needs a game license number to sell a game, so leaving it out is deliberate.

## App Review information (appStoreReviewDetail aa7db900-9ea3-4e9f-810b-1bed4e02f1a2)

- [x] Contact: Alexander Saveliev, 6032194757, alexvsaveliev@gmail.com.
- [x] Sign-in required: No.
- [x] **set 2026-09-26** Notes: the text between the rules of `uttt/docs/APP_REVIEW_NOTES.md` (2,855 characters, no em dash), including the one-device replay link.
  Keep the two in step: edit the doc, then patch `notes` on this row again.
- [ ] Attachment: skipped, it is optional and was not attached for this submission.

## Last step

- [x] **set 2026-09-26** Submitted to App Review through the API.
  `POST /v1/reviewSubmissions` (id `99f85253-ceba-4e1c-b7a9-ace6fdcb389c`), `POST /v1/reviewSubmissionItems` for version `31311df3-71e8-4d94-9273-fb5405294875` (no associated errors), then `PATCH submitted:true`.
  Version state confirmed `WAITING_FOR_REVIEW`.
