# Tallybones - what is borrowed, and what is not

This is research for a proof of concept, not a lawyer's opinion.
Get one before any store listing.

## The mechanic is public domain

Roll five dice up to three times, keep some, reroll the rest, and score the result into one category of a card: that is the Yacht family.
Yacht was published in 1938, and the family around it is older and folk: Generala in Latin America, Poker Dice in the English-speaking world, Yamb in the Balkans.
Game mechanics are not protected by copyright, and this one predates every brand built on it.

## The marks are not

- **Yahtzee** is Hasbro's trademark.
- **Kniffel** is Schmidt Spiele's trademark.
- **Farkle** is a registered trademark of Legendary Games, and a different game besides: push-your-luck, not category scoring.

None of those words appears in the product.
This file and `docs/DECISIONS.md` are the only places in `tallybones/` that write them.

## Trade dress to stay clear of

What a branded dice game owns, beyond its name, is how it looks and what its card says:

- the scorecard's exact layout (one tall column of categories with a "how to score" column beside it) and its exact words: "Small Straight", "Large Straight", "Chance", the shout word, and the bonus and joker rules for a second shout;
- the logo, the dice-cup art, and the red and yellow palette.

What this build does instead: the card's two halves stand side by side as two columns on bone paper, with no how-to-score column (T52); the categories are named in generic dice-poker words (T4); the shout word is the game's own, "Tallybones!"; there is no second-shout bonus and no joker rule; the table is foolish's green felt and wood; and the dice are drawn pips on bone squares with no cup (T10).

## What is safe, and the one item worth a second look

- **Generic poker terms** - full house, three of a kind (here "Three Alike"), four of a kind, a run - are the vocabulary of every dice and card game and belong to nobody.
- **Thirteen categories in two halves** is the structure of the whole family, described generically: a numbers half (the sum of each face) and a combinations half.
- **The 63 / 35 bonus** (a numbers half of 63 or more earns 35) is a number, and a number is not protectable by copyright or as a mark.
  Honestly, though, it is the single most recognisable number of the branded game, so it is the one item that reads as borrowed even though it is free to use.
  Flagged for the owner: changing it (for example to a bonus the card computes differently) costs one row in the kernel.

## The list

**Never:**

1. Any of the three marks in the app name, subtitle, keywords, store description, URL, screenshots, captions or code.
2. A name that riffs on one of them.
3. "Small Straight", "Large Straight", "Chance", or the branded shout word.
4. The second-shout bonus or the joker rule.
5. A one-column card with a how-to-score column.
6. A dice cup, the branded logo, or a red and yellow palette.
7. "Like Yahtzee" in any marketing copy, ever.

**Always:**

8. Draw the dice (pips as circles, no art to license).
9. Write down why each design decision was made (`docs/DECISIONS.md`): a contemporaneous record of independent choices is the cheapest evidence against a willfulness claim.
10. Search USPTO and the App Store for "Tallybones" before any listing (DECISIONS T1, BLOCKED).
