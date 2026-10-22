'use client';

import {
  ago,
  EVENT_WORD,
  PHASE_WORD,
  usToText,
  type WTable,
} from '../api';
import { BE_PHASE_GONE, BE_PHASE_OVER, BE_PHASE_PLAYING, BE_SEAT_BOT, BE_SEAT_FOOL, BE_SEAT_GOOD, BE_SEAT_OUT, BE_SEAT_TO_MOVE } from '../gen/wire';
import { PlayingCard, SuitBadge } from './PlayingCard';

interface Props {
  table: WTable;
  now: number;
  selected: boolean;
  onSelect: (gid: string) => void;
}

function Seat({ s, index, table }: { s: WTable['seats'][number]; index: number; table: WTable }) {
  const toMove = (s.flags & BE_SEAT_TO_MOVE) !== 0 && table.phase === BE_PHASE_PLAYING;
  const out = (s.flags & BE_SEAT_OUT) !== 0;
  const fool = (s.flags & BE_SEAT_FOOL) !== 0;
  const bot = (s.flags & BE_SEAT_BOT) !== 0;
  const good = (s.flags & BE_SEAT_GOOD) !== 0;
  const defender = index === table.defender && table.phase === BE_PHASE_PLAYING;
  const lead = index === table.firstAttacker && !defender && table.phase === BE_PHASE_PLAYING;
  const cls = `seat${toMove ? ' to-move' : ''}${out ? ' out' : ''}${fool ? ' fool' : ''}`;
  const stack = Math.min(s.handCount, 8);
  return (
    <div className={cls} title={`${s.name}${bot ? ` (${s.brain || 'bot'})` : ''}: ${s.handCount} cards`}>
      <span className="seat-name">
        {bot && <BotGlyph />}
        <span className="seat-text">{s.name.replace(/^%/, '')}</span>
      </span>
      <span className="seat-hand num" aria-label={`${s.handCount} cards`}>
        <span className="seat-stack" aria-hidden="true">
          {Array.from({ length: stack }, (_, i) => (
            <i key={i} style={{ left: i * 3 }} />
          ))}
        </span>
        <b>{s.handCount}</b>
      </span>
      <span className="seat-marks">
        {lead && <Sword />}
        {defender && <Shield />}
        {good && <span className="mark mark-good" title="said Good">good</span>}
        {fool && <span className="mark mark-fool" title="the fool">fool</span>}
        {out && !fool && <span className="mark mark-out">out</span>}
      </span>
    </div>
  );
}

// The lead's sword and the defender's shield: small marks, drawn rather than typed, so they read
// the same in every font.
export function Sword() {
  return (
    <svg className="mark-icon sword" width="13" height="13" viewBox="0 0 13 13" aria-label="leads the attack">
      <path d="M2 11 L9.5 3.5 M8 2 L11 5 M3 8 L5 10 M1.5 11.5 L2.5 10.5" stroke="currentColor" strokeWidth="1.4" strokeLinecap="round" fill="none" />
    </svg>
  );
}

export function Shield() {
  return (
    <svg className="mark-icon shield" width="13" height="13" viewBox="0 0 13 13" aria-label="defends">
      <path d="M6.5 1.5 L11 3.2 C11 7.5 9.3 10.2 6.5 11.7 C3.7 10.2 2 7.5 2 3.2 Z" stroke="currentColor" strokeWidth="1.3" strokeLinejoin="round" fill="currentColor" fillOpacity="0.18" />
    </svg>
  );
}

function BotGlyph() {
  return (
    <svg className="bot-glyph" width="12" height="12" viewBox="0 0 12 12" aria-label="bot">
      <rect x="2" y="3.5" width="8" height="6" rx="1.5" fill="none" stroke="currentColor" strokeWidth="1.2" />
      <circle cx="4.6" cy="6.5" r="0.9" fill="currentColor" />
      <circle cx="7.4" cy="6.5" r="0.9" fill="currentColor" />
      <path d="M6 1.5 V3.5" stroke="currentColor" strokeWidth="1.2" strokeLinecap="round" />
    </svg>
  );
}

function lastLine(t: WTable): string {
  if (!t.lastKind) return PHASE_WORD[t.phase] ?? '';
  const who = t.lastSeat >= 0 && t.lastSeat < t.seats.length ? t.seats[t.lastSeat].name.replace(/^%/, '') : '';
  const word = EVENT_WORD[t.lastKind] ?? '';
  return who ? `${who} ${word}` : word;
}

export function TableCard({ table: t, now, selected, onSelect }: Props) {
  const phase = PHASE_WORD[t.phase] ?? '';
  const pcls = t.phase === BE_PHASE_PLAYING ? 'pill-green' : t.phase === BE_PHASE_OVER ? 'pill-amber' : t.phase === BE_PHASE_GONE ? 'pill-grey' : 'pill-blue';
  const stale = t.phase === BE_PHASE_PLAYING && now - Number(t.changedMs) > 20000;
  const trumpSuit = t.powerSuit;
  return (
    <button
      type="button"
      className={`tcard${selected ? ' selected' : ''}${t.phase === BE_PHASE_GONE ? ' gone' : ''}`}
      onClick={() => onSelect(t.gid)}
      aria-label={`table ${t.gid}`}
    >
      <div className="tcard-head">
        <span className={`pill ${pcls}`}>{phase}</span>
        <span className="pill pill-grey num">{t.nSeats}p</span>
        {t.podkidnoy ? <span className="pill pill-purple">podkidnoy</span> : null}
        <span className="tcard-gid num">{t.gid}</span>
      </div>
      <div className="felt">
        <div className="felt-side">
          <div className="stock" title={`${t.deckCount} in the stock`}>
            {t.deckCount > 0 ? (
              <>
                <span className="stock-pile" aria-hidden="true">
                  <i />
                  <i />
                  <i />
                </span>
                <span className="stock-count num">{t.deckCount}</span>
              </>
            ) : (
              <span className="stock-empty num">0</span>
            )}
          </div>
          {t.hasFlipped ? (
            <PlayingCard card={t.flipped} size="xs" trump className="flipped" />
          ) : trumpSuit >= 0 ? (
            <SuitBadge suit={trumpSuit} className="trump-only" />
          ) : null}
        </div>
        <div className="battles">
          {t.attack.length === 0 ? (
            <span className="battles-empty">{t.phase === BE_PHASE_PLAYING ? (stale ? 'quiet' : 'new bout') : phase}</span>
          ) : (
            t.attack.map((a, i) => (
              <span className="battle" key={i}>
                <PlayingCard card={a} size="xs" trump={a.suit === trumpSuit} />
                <PlayingCard card={t.defense[i]} size="xs" trump={t.defense[i].suit === trumpSuit} className="defense" />
              </span>
            ))
          )}
        </div>
        <div className="felt-side right">
          <span className="discard num" title={`${t.discardCount} discarded`}>
            {t.discardCount}
          </span>
        </div>
      </div>
      <div className="seats">
        {t.seats.map((s, i) => (
          <Seat key={i} s={s} index={i} table={t} />
        ))}
      </div>
      <div className="tcard-foot">
        <span className="tcard-last">{lastLine(t)}</span>
        <span className="tcard-nums num" title={`${t.moves} moves this game`}>
          {t.lastActUs > 0 && <span title="the last move's request-to-reply latency">{usToText(t.lastActUs)}</span>}
          <span>{ago(Number(t.changedMs), now)}</span>
        </span>
      </div>
    </button>
  );
}
