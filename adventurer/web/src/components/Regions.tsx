'use client';

import { useEffect, useMemo, useRef, useState } from 'react';
import {
  ACH_PCT,
  getLeaders,
  km,
  pct,
  TYPE_LABEL,
  WireMismatch,
  type Leaders,
  type TypeIdx,
  type WLeader,
  type WRegion,
  type WRegionProgress,
  type WUser,
} from '../api';

const TOP_N = 12;

interface Props {
  type: TypeIdx;
  onType: (t: TypeIdx) => void;
  progress: WRegionProgress[] | null;
  regionsById: Map<number, WRegion>;
  usersById: Map<number, WUser>;
  onFly: (r: WRegion) => void;
  onFatal: (e: Error) => void;
  // The composite's rows: what anyone has per region; no achievements or ranks to light up.
  everyone?: boolean;
}

export function Regions({ type, onType, progress, regionsById, usersById, onFly, onFatal, everyone }: Props) {
  const [all, setAll] = useState(false);
  const [openId, setOpenId] = useState<number | null>(null);

  const rows = useMemo(() => {
    const list = (progress ?? []).filter((p) => regionsById.has(p.region));
    list.sort((a, b) => b.lenM[2] - a.lenM[2]);
    return list;
  }, [progress, regionsById]);
  const shown = all ? rows : rows.slice(0, TOP_N);

  const click = (p: WRegionProgress) => {
    const r = regionsById.get(p.region);
    if (!r) return;
    onFly(r);
    setOpenId((cur) => (cur === p.region ? null : p.region));
  };

  return (
    <section className="card">
      <div className="regions-head">
        <h2 className="card-title">
          Regions <span className="faint" title="Ordered by kilometres covered in the mode shown, most first">by km</span>
        </h2>
        <div className="tabs" role="tablist">
          {[0, 1, 2].map((t) => (
            <button
              key={t}
              type="button"
              role="tab"
              aria-selected={type === t}
              className={`tab${type === t ? ' active' : ''}`}
              onClick={() => onType(t as TypeIdx)}
            >
              {TYPE_LABEL[t]}
            </button>
          ))}
        </div>
      </div>
      {!progress ? (
        <div className="empty">Loading progress...</div>
      ) : rows.length === 0 ? (
        <div className="empty">{everyone ? 'Nobody has been anywhere yet.' : 'No regions covered yet.'}</div>
      ) : (
        <div className="region-list">
          {shown.map((p) => {
            const r = regionsById.get(p.region)!;
            const percent = pct(p.lenM[type], r.lenM[type]);
            const ach = p.achievements[type];
            return (
              <div
                key={p.region}
                className={`region${openId === p.region ? ' selected' : ''}`}
                role="button"
                tabIndex={0}
                onClick={() => click(p)}
                onKeyDown={(e) => {
                  if (e.key === 'Enter' || e.key === ' ') {
                    e.preventDefault();
                    click(p);
                  }
                }}
              >
                <div className="region-top">
                  <span className="region-name">{r.name}</span>
                  <span className="region-pct num">{percent.toFixed(1)}%</span>
                </div>
                <div className={`bar t${type}`}>
                  <i style={{ width: `${Math.max(0, Math.min(100, percent))}%` }} />
                </div>
                <div className="region-meta">
                  <span className="len num">
                    {km(p.lenM[type])} / {km(r.lenM[type])} km
                  </span>
                  {p.rankMonth[type] >= 1 && p.rankMonth[type] <= 3 && (
                    <span className="chip">#{p.rankMonth[type]} this month</span>
                  )}
                  {p.rankYear[type] >= 1 && p.rankYear[type] <= 3 && (
                    <span className="chip">#{p.rankYear[type]} this year</span>
                  )}
                  {!everyone && (
                    <span className="ach" title="Achievements: 25 / 50 / 75 / 90 / 99 percent">
                      {ACH_PCT.map((v, i) => (
                        <b key={v} className={ach & (1 << i) ? 'lit' : ''}>
                          {v}
                        </b>
                      ))}
                    </span>
                  )}
                </div>
                {openId === p.region && (
                  <LeadersPopover
                    region={r}
                    type={type}
                    usersById={usersById}
                    onClose={() => setOpenId(null)}
                    onFatal={onFatal}
                  />
                )}
              </div>
            );
          })}
          {rows.length > TOP_N && (
            <button type="button" className="showall" onClick={() => setAll((a) => !a)}>
              {all ? 'Show top 12' : `Show all ${rows.length}`}
            </button>
          )}
        </div>
      )}
    </section>
  );
}

interface PopProps {
  region: WRegion;
  type: TypeIdx;
  usersById: Map<number, WUser>;
  onClose: () => void;
  onFatal: (e: Error) => void;
}

function LeadersPopover({ region, type, usersById, onClose, onFatal }: PopProps) {
  const [data, setData] = useState<Leaders | null>(null);
  const [error, setError] = useState<string | null>(null);
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const ac = new AbortController();
    setData(null);
    setError(null);
    getLeaders(region.id, ac.signal)
      .then(setData)
      .catch((e: unknown) => {
        if (ac.signal.aborted) return;
        if (e instanceof WireMismatch) onFatal(e);
        else setError('leaders unavailable');
      });
    return () => ac.abort();
  }, [region.id, onFatal]);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onClose();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [onClose]);

  const name = (id: number) => usersById.get(id)?.name ?? `user ${id}`;
  const list = (slots: WLeader[]) => {
    const filled = slots.filter((s) => s.user !== 0);
    if (filled.length === 0) return <li className="faint">nobody yet</li>;
    return filled.map((s, i) => (
      <li key={`${s.user}-${i}`}>
        <span className="rank num">{i + 1}</span>
        <span className="who">{name(s.user)}</span>
        <span className="num muted">{km(s.m)} km</span>
      </li>
    ));
  };

  return (
    <div
      className="popover"
      ref={ref}
      onClick={(e) => e.stopPropagation()}
      onKeyDown={(e) => e.stopPropagation()}
      role="dialog"
      aria-label={`${region.name} leaders`}
    >
      <button type="button" className="close" onClick={onClose} aria-label="Close">
        x
      </button>
      <h4>
        {region.name} <span className="muted">{TYPE_LABEL[type].toLowerCase()} leaders</span>
      </h4>
      {error ? (
        <div className="muted">{error}</div>
      ) : !data ? (
        <div className="muted">Loading...</div>
      ) : (
        <div className="leaders">
          <div>
            <h5>This month</h5>
            <ol>{list(data.month[type])}</ol>
          </div>
          <div>
            <h5>This year</h5>
            <ol>{list(data.year[type])}</ol>
          </div>
        </div>
      )}
    </div>
  );
}
