# What killed UYES, and what not to repeat

This game is the shape of UNO. UNO is Mattel's, and Mattel enforces.
This file is the research, not a lawyer's opinion - get one before shipping.

## The case worth studying

**UYES** was an ad-free, offline UNO-alike on Google Play.
By the developer's own account it was removed because *"the copyright owner of UNO!, Mattel, has filed a DMCA takedown request on the Google Play store."*

Four things in that sentence matter, and each is a lesson.

### 1. It was a DMCA, which means COPYRIGHT, which means the art

A DMCA notice is a copyright instrument. Mattel did not need a trademark argument to make the app disappear - it asserted that expression had been copied, and the expression in a card game is the **art**.

**Consequence: original art is not a nice-to-have, it decides which weapon can be pointed at you.** A trademark dispute is slow and argued. A DMCA is ex parte and fast: the app is gone first, a counter-notice takes ten to fourteen business days, and filing one hands the claimant your name and an invitation to sue.

The practical rule is not "would we win." It is **never be close enough that a DMCA is plausible.**

### 2. Success is the trigger, so "we are small" is a schedule, not a defence

UYES had **over 100,000 downloads, 26,000 concurrent, and 4.2 to 4.5 stars.**
Nobody files against a two-hundred-download app. Being unnoticed is a phase that ends precisely when the thing starts working.

Design for the day it works, because that is the day the notice arrives.

### 3. The name pointed at the mark

**UYES is a riff on UNO.** One syllable plus a vowel, and the joke is the *yes/no* inversion. It is clever, and the cleverness is the problem - a name that winks at the original is evidence that you had the original in mind.

A neutral name that shares nothing is safer than a smart name that shares a joke.

### 4. You do not control what other people call it

UYES still circulates on third-party sites as **"UNO Offline: UYES"** - a name its developer did not choose. The closer your thing is to a drop-in substitute, the more the ecosystem will file it under the trademark on your behalf, and that association becomes evidence.

## The other case: they will file over card layout

In 2021 Mattel opposed **ShootHit's** card design. The element it went after was *"numbers, objects or figures represented in reduced size in the upper left corners of each card"* - corner indices, which are functional and on every playing card ever printed.

ShootHit's answer was that this is absurdly generic. They also said they would **redesign anyway** to unblock their Kickstarter.

**That is the actual outcome, and the lesson.** The question is not whether an objection would succeed. It is that responding costs more than complying, so you comply. Mattel has also taken **$425,000** in statutory damages from counterfeiters on a willfulness finding, which is the other end of the same posture.

## The list

**Never:**

1. The word in the app name, subtitle, **keywords**, store description, URL slug or screenshots. Metadata is the easiest thing to get caught on and the thing you most control.
2. A name that riffs on it phonetically or as a joke.
3. Red, yellow, green and blue as the four suits.
4. Colour as the *only* suit channel.
5. The black oval on the card face.
6. Their action glyphs - the circle-and-slash skip, the two opposed curved arrows.
7. Their card back.
8. The call-out word.
9. "Like UNO" in any marketing copy, ever. Nominative comparison is legal and it is still an invitation.

**Always:**

10. **Write down why each design decision was made.** `docs/UI.html` and this file are that record. Willfulness is what turns a takedown into damages, and a contemporaneous account of independent choices is the cheapest evidence against it.

## What this design already does

- **Suits are shape AND colour** - circle/teal, triangle/amber, square/violet, diamond/slate. Off the palette, off the colour-only trade dress, and readable by a colourblind player.
- **No oval, no black card face.** Cards are matte paper with a procedural grain.
- **Skip is two pause bars**, not a barred circle.
- **Reverse is two opposed solid triangles.** The first draft was two opposed curved arrows, which is Mattel's mark almost exactly - caught while reading the ShootHit opposition, and changed. That one is worth remembering: the mistake was made by reflex, not by decision.
- **The rules are the Crazy Eights family**, which is public domain and predates UNO.

## The name

`Pick 'Em Up` is the current working title and **it has a same-genre collision**: a card game of that name already exists on TheGameCrafter, and it is also a shedding game - players race to play all their cards. There is also *Pick 'Em Up Bitch*. Neither looks like a registered mark, and neither is Mattel, so the risk is common-law and small.

Still a collision, and in the same category. Worth a USPTO search and a decision before it goes anywhere near a store listing.
