'use client';

import { useEffect, useState } from 'react';
import {
  ago,
  getDetail,
  EVENT_WORD,
  HOSTILE_WORD,
  PHASE_WORD,
  secondsText,
  usToText,
  type WDetail,
  type WEvent,
  type WTable,
} from '../api';
import { BE_EV_HOSTILE, BE_EV_OVER, BE_PHASE_GONE, BE_PHASE_OVER, BE_PHASE_PLAYING, BE_SEAT_BOT, BE_SEAT_FOOL, BE_SEAT_GOOD, BE_SEAT_OUT, BE_SEAT_TO_MOVE } from '../gen/wire';
import { PlayingCard, SuitBadge } from './PlayingCard';
import { Shield, Sword } from './TableCard';

interface Props {
  gid: string;
  live: WTable | null;        // the row as the stream keeps it, newest
  version: number;            // bumps on every change of this table (refetches the detail)
  events: WEvent[];           // this table's events from the stream, newest first
  now: number;
  onBack: () => void;
}

export function eventText(e: WEvent, seats: WTable['seats'] | undefined): string {
  const who = e.seat >= 0 && seats && e.seat < seats.length ? seats[e.seat].name.replace(/^%/, '') : e.seat >= 0 ? `seat ${e.seat}` : '';
  if (e.kind === BE_EV_HOSTILE) {
    const kind = e.cards.length ? e.cards[0].value : 0;
    return `${who} sent ${HOSTILE_WORD[kind] ?? 'a hostile move'}: ${e.refused ? 'refused' : 'ACCEPTED'}`;
  }
  if (e.kind === BE_EV_OVER) return who ? `game over, ${who} is the fool` : 'game over, a draw';
  const word = EVENT_WORD[e.kind] ?? '';
  return who ? `${who} ${word}` : word;
}

export function TableDetail({ gid, live, version, events, now, onBack }: Props) {
  const [detail, setDetail] = useState<WDetail | null>(null);
  const [error, setError] = useState<string | null>(null);

  // The detail (the hands, the event tail) follows the row: refetched on every change, coalesced.
  useEffect(() => {
    const ac = new AbortController();
    const t = setTimeout(() => {
      getDetail(gid, ac.signal)
        .then((d) => {
          setDetail(d);
          setError(null);
        })
        .catch((e) => {
          if (!ac.signal.aborted) setError(e instanceof Error ? e.message : String(e));
        });
    }, 150);
    return () => {
      clearTimeout(t);
      ac.abort();
    };
  }, [gid, version]);

  const t = live ?? detail?.table ?? null;
  if (!t) {
    return (
      <section className="card detail">
        <div className="detail-head">
          <button type="button" className="btn-sm" onClick={onBack}>
            &larr; All tables
          </button>
          <h2 className="card-title">Table {gid}</h2>
        </div>
        <div className="empty">{error ?? 'Loading...'}</div>
      </section>
    );
  }
  const trump = t.powerSuit;
  const seatsLen = t.seats.length;
  const hands = detail?.hands ?? [];
  const pcls = t.phase === BE_PHASE_PLAYING ? 'pill-green' : t.phase === BE_PHASE_OVER ? 'pill-amber' : t.phase === BE_PHASE_GONE ? 'pill-grey' : 'pill-blue';
  const merged = events.length ? events : (detail?.events ?? []).slice().reverse();
  const length = t.finishedMs > 0n && t.createdMs > 0n ? Number(t.finishedMs - t.createdMs) / 1000 : 0;

  return (
    <section className="card detail">
      <div className="detail-head">
        <button type="button" className="btn-sm" onClick={onBack}>
          &larr; All tables
        </button>
        <h2 className="card-title">Table {gid}</h2>
        <span className={`pill ${pcls}`}>{PHASE_WORD[t.phase]}</span>
        <span className="pill pill-grey num">{t.nSeats} seats</span>
        {t.podkidnoy ? <span className="pill pill-purple">podkidnoy</span> : <span className="pill pill-grey">passing</span>}
        <span className="detail-meta num">
          v{t.version} &middot; {t.moves} moves{length > 0 ? ` · ${secondsText(Math.round(length))}` : ''}
        </span>
      </div>

      <div className="detail-board">
        <div className="detail-stock">
          <div className="label">Stock</div>
          <div className="stock-big num">{t.deckCount}</div>
          <div className="label">Trump</div>
          {t.hasFlipped ? <PlayingCard card={t.flipped} size="md" trump /> : trump >= 0 ? <SuitBadge suit={trump} className="trump-big" /> : <span className="faint">-</span>}
          <div className="label">Discard</div>
          <div className="stock-big num">{t.discardCount}</div>
        </div>
        <div className="detail-table">
          <div className="label">On the table</div>
          {t.attack.length === 0 ? (
            <div className="empty">{t.phase === BE_PHASE_PLAYING ? 'Nothing yet this bout' : PHASE_WORD[t.phase]}</div>
          ) : (
            <div className="detail-battles">
              {t.attack.map((a, i) => (
                <span className="battle-md" key={i}>
                  <PlayingCard card={a} size="md" trump={a.suit === trump} />
                  <PlayingCard card={t.defense[i]} size="md" trump={t.defense[i].suit === trump} className="defense" />
                </span>
              ))}
            </div>
          )}
        </div>
      </div>

      <div className="label">Seats</div>
      <div className="detail-seats">
        {t.seats.map((s, i) => {
          const bot = (s.flags & BE_SEAT_BOT) !== 0;
          const toMove = (s.flags & BE_SEAT_TO_MOVE) !== 0 && t.phase === BE_PHASE_PLAYING;
          const hand = hands[i];
          const known = hand && hand.cards.length > 0;
          return (
            <div className={`dseat${toMove ? ' to-move' : ''}${(s.flags & BE_SEAT_OUT) !== 0 ? ' out' : ''}`} key={i}>
              <div className="dseat-head">
                <span className="dseat-name">
                  {s.name.replace(/^%/, '')}
                  {bot && <span className="pill pill-grey">{s.brain || 'bot'}</span>}
                  {!bot && <span className="pill pill-blue">sim</span>}
                </span>
                <span className="dseat-marks">
                  {i === t.firstAttacker && t.phase === BE_PHASE_PLAYING && <Sword />}
                  {i === t.defender && t.phase === BE_PHASE_PLAYING && <Shield />}
                  {(s.flags & BE_SEAT_GOOD) !== 0 && <span className="mark mark-good">good</span>}
                  {(s.flags & BE_SEAT_FOOL) !== 0 && <span className="mark mark-fool">fool</span>}
                  {(s.flags & BE_SEAT_OUT) !== 0 && (s.flags & BE_SEAT_FOOL) === 0 && <span className="mark mark-out">out</span>}
                  <span className="num faint">{s.handCount} cards</span>
                </span>
              </div>
              <div className="dseat-hand">
                {known
                  ? hand.cards.map((c, k) => <PlayingCard key={k} card={c} size="sm" trump={c.suit === trump} />)
                  : Array.from({ length: Math.min(s.handCount, 36) }, (_, k) => <PlayingCard key={k} card={{ suit: -1, value: -1 }} size="sm" />)}
                {known && hand.cards.length !== s.handCount && <span className="faint num">({s.handCount})</span>}
              </div>
            </div>
          );
        })}
      </div>

      <div className="label">What happened</div>
      <div className="detail-events">
        {merged.length === 0 ? (
          <div className="empty">Nothing yet.</div>
        ) : (
          merged.slice(0, 40).map((e, i) => (
            <div className={`dev-row${e.kind === BE_EV_HOSTILE && !e.refused ? ' bad' : ''}`} key={`${e.version}-${e.kind}-${i}`}>
              <span className="dev-ago num">{ago(Number(e.atMs), now)}</span>
              <span className="dev-text">{eventText(e, t.seats)}</span>
              <span className="dev-cards">
                {e.kind !== BE_EV_HOSTILE && e.cards.map((c, k) => <PlayingCard key={k} card={c} size="xs" trump={c.suit === trump} />)}
              </span>
              <span className="dev-num num">
                {e.latUs > 0 ? usToText(e.latUs) : ''}
                {e.version > 0 ? ` v${e.version}` : ''}
              </span>
            </div>
          ))
        )}
      </div>
      {seatsLen === 0 && <div className="empty">No seats yet.</div>}
    </section>
  );
}
