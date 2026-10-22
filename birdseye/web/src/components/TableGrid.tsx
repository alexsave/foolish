'use client';

// The table grid as columns of cards (masonry): a card of eight seats is three times the height
// of a card of two, so a row-aligned grid leaves white bands under the short ones. Each card keeps
// the column it was first put in (the shortest at that moment, by its measured height), so a live
// grid never shuffles cards sideways; within a column the cards keep the grid's order. A change in
// the number of columns (a resize, compact on or off) lays everything out again.

import { useLayoutEffect, useMemo, useRef, useState } from 'react';
import type { WTable } from '../api';
import { TableCard } from './TableCard';

interface Props {
  tables: WTable[];
  compact: boolean;
  now: number;
  onSelect: (gid: string) => void;
}

const GAP = 12, GAP_COMPACT = 8;
const MIN_COL = 250, MIN_COL_COMPACT = 196;

// Before a card has been measured: its height from its seats.
function guess(t: WTable, compact: boolean): number {
  const seats = Math.max(t.nSeats, t.seats.length, 2);
  return compact ? 96 + seats * 22 : 130 + seats * 22;
}

export function TableGrid({ tables, compact, now, onSelect }: Props) {
  const wrap = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(0);
  const heights = useRef(new Map<string, number>());
  const placed = useRef(new Map<string, number>());
  const placedCols = useRef(0);
  const [, setMeasured] = useState(0);

  useLayoutEffect(() => {
    const el = wrap.current;
    if (!el) return;
    const ro = new ResizeObserver(() => setWidth(el.clientWidth));
    ro.observe(el);
    setWidth(el.clientWidth);
    return () => ro.disconnect();
  }, []);

  const gap = compact ? GAP_COMPACT : GAP;
  const cols = Math.max(1, Math.floor((width + gap) / ((compact ? MIN_COL_COMPACT : MIN_COL) + gap)));

  const columns = useMemo(() => {
    if (placedCols.current !== cols) {
      placed.current.clear();
      placedCols.current = cols;
    }
    const live = new Set(tables.map((t) => t.gid));
    for (const gid of placed.current.keys()) if (!live.has(gid)) placed.current.delete(gid);
    const fill = new Array<number>(cols).fill(0);
    const out: WTable[][] = Array.from({ length: cols }, () => []);
    // The cards that already have a column count toward its height first, in order.
    for (const t of tables) {
      const c = placed.current.get(t.gid);
      if (c === undefined) continue;
      out[c].push(t);
      fill[c] += (heights.current.get(t.gid) ?? guess(t, compact)) + gap;
    }
    for (const t of tables) {
      if (placed.current.has(t.gid)) continue;
      let c = 0;
      for (let k = 1; k < cols; k++) if (fill[k] < fill[c]) c = k;
      placed.current.set(t.gid, c);
      out[c].push(t);
      fill[c] += (heights.current.get(t.gid) ?? guess(t, compact)) + gap;
    }
    // Each column in the grid's own order.
    const rank = new Map(tables.map((t, i) => [t.gid, i]));
    for (const col of out) col.sort((a, b) => (rank.get(a.gid) ?? 0) - (rank.get(b.gid) ?? 0));
    return out;
  }, [tables, cols, compact, gap]);

  // The cards' real heights, for placing the next ones.
  useLayoutEffect(() => {
    const el = wrap.current;
    if (!el) return;
    let changed = false;
    el.querySelectorAll<HTMLElement>('[data-gid]').forEach((n) => {
      const gid = n.dataset.gid!;
      const h = n.offsetHeight;
      if (heights.current.get(gid) !== h) {
        heights.current.set(gid, h);
        changed = true;
      }
    });
    if (changed) setMeasured((m) => m + 1);
  });

  return (
    <div ref={wrap} className={`mgrid${compact ? ' compact' : ''}`} style={{ gap }}>
      {columns.map((col, i) => (
        <div className="mcol" key={i} style={{ gap }}>
          {col.map((t) => (
            <div key={t.gid} data-gid={t.gid}>
              <TableCard table={t} now={now} selected={false} onSelect={onSelect} />
            </div>
          ))}
        </div>
      ))}
    </div>
  );
}
