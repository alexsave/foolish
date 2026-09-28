# What is safe to build here, and what is not

This is the research for a proof of concept, not a lawyer's opinion.
Get one before anything goes near a store.

## The game is a folk game

Liar's Dice is a public-domain folk bluffing game with no author and no owner.
In China and Taiwan it is played in bars as Chui Niu (吹牛, "bragging"), with a cup and five dice each.
The same game is played across South America, and in the West it is best known from the scene in Pirates of the Caribbean: Dead Man's Chest.
Game mechanics are not protected by copyright, so hidden dice, rising bids, wild 1s, calling a bid and losing a die are all free to implement.

## Names to avoid

- **Perudo** is a published commercial product and a registered mark, so it appears nowhere in the product or its metadata.
- **Dudo** is the traditional South American name, but it is also used by specific published products, so it is avoided too.
- **Liar's Dice** is the generic English name, and generic names are usually weak marks, but "usually" is not a search.
  Before it is used as a store name, subtitle or keyword, the owner needs a trademark check on it.
- **Chui Niu** is the owner's choice and the game's own generic Chinese name.
  It still needs the same store-name check, because a generic name in one language can be somebody's registered mark in another market.

Both checks are listed under BLOCKED in `docs/DECISIONS.md`.
The name is one `GAME_NAME` string (O2), so a rename is one line.

## Art and theming to avoid

- **No pirate theming**: no ships, skulls, parrots, cutlasses, treasure or sea-dog captions.
  The best-known depiction of this game is a Disney film, and anything that echoes it invites a complaint that is cheaper to comply with than to argue.
- **No published product's cup, box, dice or logo art.**
  The cup and dice are drawn here from scratch, and no product's colours, pips or packaging are traced or matched.
- A cup and six-sided dice are generic objects, and drawing them is not the problem; copying somebody's specific drawing of them is.

## The rules we implement

The variant is the common bar version: 2 to 6 players, five dice each, 1s wild and counted toward every face, no bid on 1s, and no "spot on" call (R1 to R5 in `docs/DECISIONS.md`).
The rules are written in our own words from the folk game.
No published product's rulebook or rule text was copied or paraphrased.

## What was checked, and what was not

Checked: the owner searched the App Store for "liar's dice", "perudo" and "dudo" and found no Messages game, only an unrelated app called "Bluffs"; GamePigeon's own catalog has no dice-bidding game (`docs/IMESSAGE_APP_IDEAS.md` section 7).
Not checked: no USPTO or other trademark database search has been run on "Chui Niu", "Liar's Dice" or any other candidate name, and no lawyer has looked at any of this.
That is acceptable for a proof of concept that never reaches App Store Connect, and not acceptable for anything past it.
