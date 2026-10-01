# The layout probe

A Messages app message carries a URL, and the URL is capped at 5,000 characters.
It also carries a layout: one picture and seven strings.
This tool measures whether the extension that READS a message is handed that layout back, and how much of it survives.

The answer on a simulator is yes, all of it, and the picture is by far the widest channel a message has.
On a real send (a phone to this Mac) the picture survives at quality 0.89 but is cut to 1200 px on a side, which is the limit a design has to fit; see "A real send".
Measured so far only between one account's own devices.
No product uses this channel.

## What it found

Measured 2026-10-01 on the iOS 27.0 (24A434) and iOS 26.3 (23D8133) simulators with Xcode 27.0, through the simulator's two stub conversations.
Both runtimes gave the same bytes.

**What the reading side is handed:**

| Where the extension meets the message | Picture | Seven strings |
| --- | --- | --- |
| `didStartSending`, on the sender, as it goes | whole | all nil |
| The sender taps its own bubble (`didSelect`) | whole | whole |
| The other thread taps the received copy (`selectedMessage`) | whole | whole |

The seven strings are `caption`, `subcaption`, `trailingCaption`, `trailingSubcaption`, `imageTitle`, `imageSubtitle` and `summaryText`.
The received copy returns its picture even on iOS 27, where the transcript draws an incoming bubble as a caption pill with no picture in it.

**The picture is re-encoded, never resized.**
Messages turns it into a JPEG before any reader sees it, and `mediaFileURL` on the way back points at that same file.
The encoder is ImageIO at quality 0.50 with 4:2:0 chroma: `sweep --match` on a file taken from the simulator finds that quality's quantisation table exactly.
A lossless PNG handed over through `mediaFileURL` comes back as the very same JPEG (299,364 bytes for the 729 px grey pattern, either way), so there is no lossless route.

**What survives that encoder**, as cells of the pattern that decode wrong:

| Grid | Pixels per cell | Palette | Picture | JPEG bytes | Wrong cells | Worst channel error |
| --- | --- | --- | --- | --- | --- | --- |
| 243 x 243 | 3 | colour | 729 px | 345,122 | 0 of 59,049 | 87 |
| 243 x 243 | 1 | colour | 243 px | 45,013 | 9,790 of 59,049 | 153 |
| 243 x 243 | 1 | grey | 243 px | 41,570 | 0 of 59,049 | 35 |
| 243 x 243 | 2 | grey | 486 px | 142,561 | 0 of 59,049 | 44 |
| 243 x 243 | 3 | grey | 729 px | 299,364 | 0 of 59,049 | 34 |
| 729 x 729 | 1 | grey | 729 px | 347,566 | 0 of 531,441 | 38 |
| 729 x 729 | 2 | grey | 1458 px | 1,257,340 | 0 of 531,441 | 37 |
| 729 x 729 | 3 | grey | 2187 px | 2,673,015 | 0 of 531,441 | 35 |
| 1458 x 1458 | 3 | grey | 4374 px | 10,709,457 | 0 of 2,125,764 | 40 |

Three grey levels survive at one pixel per cell.
Three colours at one pixel per cell do not, because two of them differ mostly in hue and 4:2:0 keeps hue at half resolution.
A cell holds one of three states, so the 729 x 729 grid at one pixel per cell is 531,441 x log2(3) bits, about 105 KB, in one bubble; the URL holds about 3 KB.

**The strings are not capped by the framework.**
All seven came back whole at 40, 1,000, 20,000 and 200,000 characters each.
They are also drawn in the bubble, so a payload in them is visible to the people in the thread.

**The URL is the only field the framework polices.**
4,990 characters is accepted.
5,200 is refused at insert with `com.apple.messages.messagesapp-error` code 8, on the simulator as on a phone.

## Where the JPEG is made

Read out of the iOS 27.0 simulator runtime's Messages framework with a debugger attached to the probe's own extension process (the framework lives in a shared cache, so there is no loose binary to open).

- **It is made on the sending phone, inside the extension's own process.** `-[MSMessage _pluginPayloadWithAppIconData:appName:adamID:allowDataPayloads:]` takes the layout's `image`, loads the constant 0.5 and calls `UIImageJPEGRepresentation`. That is the whole conversion: no resize, no other branch for a picture.
- **The file it writes is the bubble's attachment.** `-[_MSTempFileManager writeTemporaryFileWithData:type:]` puts the bytes at `ms-XXXXXX.jpeg` in the extension's temporary directory through `mkstemps`, and the payload's `setAttachments:` takes that URL. This is the `mediaFileURL` a reader is handed.
- **A media file that is an image goes the same way.** `-[MSMessageTemplateLayout image]` reads `mediaFileURL`, and when the file's type conforms to an image type it decodes it into `image`, so the payload builder sees a picture and re-encodes it. That is why a PNG handed over as a file came back as the same JPEG.
- **A media file that is NOT an image is not re-encoded there.** With no `image`, the payload builder attaches the file at `mediaFileURL` as it is. A video is the documented case. Whether its bytes then arrive untouched is not measured.
- **The strings travel in the payload, not in the attachment.** `-[MSMessage _payloadDataFromAppIconData:appName:adamID:allowDataPayloads:]` puts the URL, the summary text and the six layout strings in a dictionary and keyed-archives it. Nothing in that function compares a length.
- **The 5,000-character URL limit is not in this framework.** No 5,000 appears in its code, so the refusal comes from the host side.
- **A second encoder exists downstream, and JPEG is on its list.** The transport's shared code has an outgoing transcode step with a low-quality-mode setting, and `IMSupportedImageUTITypesForOutgoingTranscode()` returns `public.jpeg` among its types. Whether that step runs on a bubble's attachment is exactly what the two-phone test has to answer. The host app refuses a debugger, so this was not followed further.

### The transport's transcoder

Read from the simulator runtime's `IMTranscoderAgent` (a loose binary, 306 KB, with a bundled Core ML model `Image_Estimator_HEIF`) and from `IMDaemonCore`, with `otool` and no debugger.
The classes are `IMTranscoder_Image`, `IMTranscoderImageSizeEstimator`, `IMTranscoderImageQualityEstimator` and `IMEmbeddedHardwareJPEGTranscoder`.

- **When it acts**, from `shouldTranscodeTransfer:...fileSizeLimit:` and its own log strings: never on a sticker or Genmoji; always for MMS; for an image that is wide-gamut, WebP, or HEIF that the recipients do not want; and otherwise only when "That wasn't enough, let's look at filesize too" finds the file over `fileSizeLimit`. An image of a supported type under the limit is not touched by this step.
- **What it does to an image that is over**: `_writeImage:...withMaxByteSize:maxDimension:startingLengthIndex:usedLengthIndex:` walks a ladder of sizes ("Trying maxSize = %lu (index: %d/%d)"), estimates the output size, and keeps the first that fits. With low-quality mode on, a Core ML model predicts a quality factor and falls back to the older estimator when it comes out too low.
- **App messages have their own entry points** in `IMDaemonCore`: `transcodeLocalTransferPayloadData:balloonBundleID:completionBlock:` and `transcodeFallbackFileTransferPayloadData:balloonBundleID:attachments:completionBlock:`, with the log line "Received transcoded output from balloon bundle id %@ path %@". A bubble's attachment is handled on purpose on a real send; the simulator's loopback is not evidence about it.
- **The byte limits** come from `IMiMessageSizeLimitsForTransferType(NSString *uti, unsigned long *big, unsigned long *small, id)`, logged as "Server bag File Size Limits". Called live in the iOS 27.0 simulator (where the server bag is not reachable, so these are the built-in defaults and a phone may differ):

  | Type | big | small |
  | --- | --- | --- |
  | `public.jpeg`, `public.png`, `public.heic`, `com.compuserve.gif`, `public.data`, `public.item` | 10,485,760 B (10 MiB) | 4,194,304 B (4 MiB) |
  | `public.mpeg-4`, `public.audio` | 41,943,040 B (40 MiB) | 4,194,304 B (4 MiB) |

  A user default `TranscodeSizeLimitsKB` overrides them ("Overriding Transcode sizes limits due to default TranscodeSizeLimitsKB").
- **What that says about the measurements above**: every picture in the table except the last row is under 4 MiB, so the transcoder has no reason to touch them. The 4374 px picture is 10,709,457 bytes, which is OVER the 10 MiB big limit, and the simulator still returned it whole, which is one more sign that the loopback does not run the transcoder. A real send of a picture over a limit is the case that would be resized.

`make sweep` already runs the first encoder as Messages runs it: `UIImageJPEGRepresentation` is ImageIO, and the q0.50 column is that call.

## A real send: the phone to this Mac (2026-10-01)

The first measurement through a real transport. An iPhone 15 Pro Max on iOS 27.0 sent probe bubbles in the thread to its own Apple ID, and this Mac, signed into the same account, received them through Apple's servers.
The Mac keeps the bubble's picture as a file under `~/Library/Messages/Attachments/` and its strings in the message row's `payload_data` in `chat.db` (the terminal needs Full Disk Access to read either).
`sweep --judge FILE --cells N` decodes such a file as the probe pattern; `sweep --match FILE` names its JPEG quality.

| Sent as | The phone's extension saw (quality 0.50) | The file on the Mac | Wrong cells on the Mac |
| --- | --- | --- | --- |
| 243 cells, 1 px each, grey (243 px) | 243 x 243, 41,570 B | 243 x 243, 72,040 B, quality 0.89 | 0 of 59,049 (worst channel error 37) |
| 243 cells, 3 px each, grey, strings of 20,000 (729 px) | 729 x 729, 299,364 B | 729 x 729, 537,637 B, quality 0.89 | 0 of 59,049 (worst channel error 35) |
| 729 cells, 3 px each, grey (2187 px) | 2187 x 2187, 2,673,015 B | **1200 x 1200**, 1,435,899 B, quality 0.89 | 12,150 of 531,441 (2.3%) |
| 1458 cells, 3 px each, grey (4374 px, 10.7 MB) | 4374 x 4374, 10,709,457 B | **1200 x 1200**, 1,354,537 B, quality 0.89 | 982,142 of 2,125,764 (46%, noise) |
| 243 cells, 3 px each, strings of 200,000 (729 px) | 729 x 729, 299,364 B | 729 x 729, 537,637 B, quality 0.89 | 0 of 59,049 |

- **A second encoder runs after the extension's.** Every received picture is a different JPEG from the one the extension made: quality 0.89 where the extension's was 0.50, still 4:2:0, and bigger on disk because it was decoded and written again at a higher quality. The sender's own synced copy on the Mac is the same file as the received copy, so the re-encode happens on the sending phone, after `didStartSending` and before upload. This is the transport's transcoder (see "The transport's transcoder" above).
- **A picture is cut to 1200 px on a side.** Two pictures over 1200 px, 2187 px and 4374 px, both came back 1200 x 1200. The 243 px and 729 px pictures kept their size. The cap does not depend on the byte size: the 2.67 MB picture, well under the 10 MiB limit, was cut to the same 1200.
- **The resize is what costs cells.** Pictures that kept their size decode with no wrong cell. The 729-cell grid, shrunk to 1.65 px per cell, lost 2.3% of its cells; the 1458-cell grid, shrunk to 0.82 px per cell, is noise.
- **All seven strings come through, even at 200,000 characters each** (a 1.4 MB row), although the phone labels that bubble "Not Delivered". The synced rows carry every string at its sent length. Not using them is a design choice, not a limit: a caption that long fills the bubble with text.
- **Delivery is the practical limit, and it is erratic.** Measured from the attachment's `created_date` against the message's send time in `chat.db`: the 243 px picture (72,040 B) and the 729 px and 1200 px pictures were on the Mac within about two minutes of sending (the first listing after each round already had them); the 4374 px bubble's picture (1.35 MB, after the cut) and the 200,000-character-strings bubble arrived 106 to 109 s after sending. A round of ten pictures sent afterwards (972 to 2916 px, 1,000 to 1,600 px grids, none with strings) had not arrived after more than six minutes. Sizes alone do not explain it (the delivered 1200 px picture was 1.4 MB, the ten were of similar size), so the stall may be the "Not Delivered" 200,000-character bubble ahead of them in the thread's queue; that is not tested. A design should not count on a bubble arriving within seconds, and it should keep a picture small: the 72 KB, 243 px picture is the one that has arrived every time.

## What it did not find

- **A second Apple ID.** The real send above is one account's phone and Mac. A message to a different person may take a different path, and a phone receiving it is a different reader; the numbers are for the sender's transcoder, which is the same either way, but nothing here measured the receiving phone's own handling.
- **Where below 1200 px a picture starts to be cut.** 972 and 1000 px are in the ladder run.
- **Why the 200,000-character send shows "Not Delivered" on the phone** while the Mac received it in full.

## How much harsher an encoder the pattern would survive

`make sweep` re-encodes the 243 x 243 pattern on a Mac with the same encoder and counts the wrong cells.
Messages' own setting is the q0.50 column.

```
wrong cells of 59049 after one JPEG at ImageIO quality q, by palette and pixels per cell
          q0.90   q0.80   q0.70   q0.60   q0.50   q0.40   q0.30   q0.20   q0.10
grey   1px 0       0       0       0       0       0       181     2102    4363
grey   2px 0       0       0       0       0       0       9       194     614
grey   3px 0       0       0       0       0       0       4       89      263
grey   4px 0       0       0       0       0       0       0       0       0
colour 1px 8303    8435    8452    8766    9790    11894   13871   15334   16320
colour 2px 0       0       0       26      241     1177    5080    9888    11130
colour 3px 0       0       0       0       0       264     3081    6238    7334
colour 4px 0       0       0       0       0       0       33      452     1364

wrong cells of 59049 after a scale to S px a side, then one JPEG at q0.75 (grey)
          S1458   S1200   S972    S729    S600    S486    S400    S300    S243
grey   2px -       -       -       -       -       -       70      7772    5762
grey   3px -       -       -       -       0       107     1399    10855   11114
grey   4px -       -       -       0       0       1419    2350    12377   13353
grey   6px -       0       0       0       0       4206    3519    13948   15419
```

Recompression is survivable: grey at four pixels per cell decodes whole all the way down to q0.10.
A resize is the real threat.
A picture scaled down to about two and a half pixels per cell still decodes whole; one scaled down to two does not, even though a pattern PAINTED at two pixels per cell does.
The colour 1 px entry at q0.50 (9,790) is the same number the simulator gave, which is the check that this tool and Messages agree.

## Running it

The C builds anywhere; the probe app and the sweep need a Mac.

```bash
make -C shared/tools/layout_probe test     # the pattern and the judge
make -C shared/tools/layout_probe sweep    # the two tables above
shared/tools/layout_probe/build/sweep --samples DIR   # pictures: what came back beside how it decodes
make -C shared/tools/layout_probe app      # xcodegen, then a simulator build
```

On a simulator the probe is driven by the Messages rig, which is the card game's `ios/Tools/rig/rig.sh` (called `$RIG` below).
`rig.env` here is the whole port of the rig to this app.

```bash
source shared/tools/layout_probe/rig.env
export RIG_SIM=$($RIG newsim LayoutProbe | grep -oE '[0-9A-F-]{36}')   # one simulator per task
xcrun simctl install $RIG_SIM "$LAYOUT_PROBE_APP"
$RIG stage
G=$($RIG groupdir)                           # the App Group, where dev files go

echo "name=mine n=243 p=1 grey=1 str=1000" > "$G/dev.probe"
$RIG open          # the drawer; the probe takes the file and stages that message
$RIG turn          # Send; the report gains a [sending] entry
$RIG tapopen       # tap the sent bubble; [selected]
$RIG leave
$RIG tapopen 555   # the received copy, in the other stub thread; [opened]
$RIG leave
$RIG flight        # the whole report
xcrun simctl shutdown $RIG_SIM
```

`dev.probe` is `key=value` pairs, any subset, and a key it does not name keeps its default:

| Key | Default | What |
| --- | --- | --- |
| `name` | `custom` | the label in the report |
| `n` | 243 | cells a side |
| `p` | 3 | pixels per cell |
| `grey` | 1 | 1 for the luminance palette, 0 for the hue-separated one |
| `str` | 0 | characters in each of the seven strings; 0 sends no caption text at all |
| `media` | 0 | 1 to hand the picture over as a PNG file through `mediaFileURL` |
| `url` | 200 | characters in the URL |

On a phone there is no rig: build the same scheme for the device, open Probe from the `+` menu in a thread with a second device, and tap a preset button, then Send.
The report is on the probe's own screen and in the unified log under the subsystem `tools.layoutprobe`; tap the bubble on each phone to get that side's entry.
The first device build registers the probe's bundle id and App Group on the developer account named in `ios/project.yml`.

Three things the rig needs from an app, each of which cost a run here:

- The bubble's app icon must be strongly red. `tapopen` finds the newest bubble by that mark, so the probe's icons are its own pattern in reds.
- A tapped bubble opens the drawer expanded, and an expanded drawer hides the thread from the accessibility tree. The probe steps back to compact once it has reported.
- `simctl get_app_container` cannot find a Messages-only container's App Group. `rig.sh groupdir` can.

## What is here

| Path | What |
| --- | --- |
| `layout_probe.h` | the pattern (`lp_state`, `lp_fill`) and the verdict on what came back (`lp_judge`), header-only, with `module.modulemap` (`CLayoutProbe`) for Swift |
| `layout_probe_test.c` | its test; each claim in it was mutation-checked against a broken copy of the header |
| `sweep.c` | the recompression and resize tables, `--match FILE.jpeg` to name the ImageIO quality that wrote a file, and `--samples DIR` to write a corner of each case as a picture, wrong cells in green |
| `ios/` | the probe app: `project.yml` for xcodegen, a codeless container (`App/`) and the extension (`Ext/`) |
| `rig.env` | the rig's product block for the probe |
