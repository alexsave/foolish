'use client';

import { cardIsHidden, cardIsNone, rankText, SUIT_GLYPH, SUIT_RED, type WCard } from '../api';

interface Props {
  card: WCard;
  size?: 'xs' | 'sm' | 'md';
  trump?: boolean;   // the card is of the trump suit: a gold rim
  className?: string;
}

// One playing card: rank and suit, red for hearts and diamonds; a hidden card is a back, none is
// an empty slot. Sizes: xs on a table card, sm in a hand, md in the detail's battles.
export function PlayingCard({ card, size = 'sm', trump = false, className = '' }: Props) {
  const cls = `pc pc-${size}${trump ? ' pc-trump' : ''}${className ? ' ' + className : ''}`;
  if (cardIsNone(card)) return <span className={`${cls} pc-none`} aria-hidden="true" />;
  if (cardIsHidden(card)) return <span className={`${cls} pc-back`} aria-label="face down" />;
  const red = SUIT_RED[card.suit] ?? false;
  const glyph = SUIT_GLYPH[card.suit] ?? '?';
  return (
    <span className={`${cls}${red ? ' red' : ''}`} aria-label={`${rankText(card.value)}${glyph}`}>
      <span className="pc-rank">{rankText(card.value)}</span>
      <span className="pc-suit">{glyph}</span>
    </span>
  );
}

export function SuitBadge({ suit, className = '' }: { suit: number; className?: string }) {
  if (suit < 0) return null;
  const red = SUIT_RED[suit] ?? false;
  return <span className={`suit-badge${red ? ' red' : ''}${className ? ' ' + className : ''}`}>{SUIT_GLYPH[suit]}</span>;
}
