'use client';

import {
  dateText,
  EVERYONE,
  HIGHWAY_CLASS,
  int,
  km,
  miles,
  pct,
  points,
  TYPE_LABEL,
  USER_KIND,
  type Everyone,
  type TypeIdx,
  type UserDetail,
  type WRegion,
  type WUser,
} from '../api';

interface Props {
  user: WUser | null;
  detail: UserDetail | null;
  // The composite (api.ts EVERYONE) when it is selected: totals per type and the top roads.
  everyone: Everyone | null;
  type: TypeIdx;
  regionsById: Map<number, WRegion>;
  onFly: (lat: number, lon: number) => void;
}

// Everyone's card: how much of the city anyone has done, per type, and the most-travelled
// roads of the type in hand (by distinct users).
function EveryoneCard({ everyone, type, regionsById, onFly }: Omit<Props, 'user' | 'detail'>) {
  const s = everyone?.stats ?? null;
  const top = everyone ? everyone.top.filter((r) => r.type === type) : [];
  return (
    <section className="card">
      <div className="stats-head">
        <div className="stats-name">Everyone</div>
        <span className="pill pill-blue">{s ? `${int(s.users)} users` : 'all users'}</span>
      </div>
      <div className="bignums">
        {[0, 1, 2].map((t) => (
          <div className="bignum" key={t}>
            <div className="label">{TYPE_LABEL[t]}</div>
            <div className="value num">
              {s ? km(s.traveledM[t]) : '-'}
              <small>km</small>
            </div>
            <div className="sub num">{s ? `${pct(s.traveledM[t], s.totalM[t]).toFixed(1)}% of ${km(s.totalM[t], 0)} km` : ''}</div>
            {t === 2 && s && <div className="points num">{int(s.segs[t])} of {int(s.segsTotal[t])} segs</div>}
          </div>
        ))}
      </div>
      <h3 className="card-sub">Most travelled, {TYPE_LABEL[type].toLowerCase()}</h3>
      {!everyone ? (
        <div className="empty">Loading everyone...</div>
      ) : top.length === 0 ? (
        <div className="empty">Nobody has been anywhere yet.</div>
      ) : (
        <ol className="toproads">
          {top.map((r, i) => (
            <li key={`${r.seg}-${i}`}>
              <button type="button" className="toproad" onClick={() => onFly(r.cLat, r.cLon)} title="Go there">
                <span className="rank num">{i + 1}</span>
                <span className="who">
                  {r.name || <i>{HIGHWAY_CLASS[r.hclass] ?? 'road'}</i>}
                  <span className="faint"> {regionsById.get(r.region)?.name ?? ''}</span>
                </span>
                <span className="num muted">{int(r.users)} {r.users === 1 ? 'user' : 'users'}</span>
              </button>
            </li>
          ))}
        </ol>
      )}
    </section>
  );
}

export function StatsCard({ user, detail, everyone, type, regionsById, onFly }: Props) {
  if (user?.id === EVERYONE || (user === null && everyone)) return <EveryoneCard everyone={everyone} type={type} regionsById={regionsById} onFly={onFly} />;
  if (!user) {
    return (
      <section className="card">
        <h2 className="card-title">Rider</h2>
        <div className="empty">Waiting for the users list...</div>
      </section>
    );
  }
  // The list carries the unique lengths too, so the card paints before /adv/user answers.
  const unique = detail ? detail.stats.uniqueM : user.uniqueM;
  const s = detail?.stats ?? null;
  return (
    <section className="card">
      <div className="stats-head">
        <div className="stats-name">{user.name}</div>
        <span className="pill pill-blue">{USER_KIND[user.kind] ?? 'user'}</span>
      </div>
      <div className="bignums">
        {[0, 1, 2].map((t) => (
          <div className="bignum" key={t}>
            <div className="label">{TYPE_LABEL[t]}</div>
            <div className="value num">
              {km(unique[t])}
              <small>km</small>
            </div>
            <div className="sub num">{miles(unique[t])} mi</div>
            {t === 2 && <div className="points num">{points(unique[t])} pts</div>}
          </div>
        ))}
      </div>
      <dl className="kv">
        <dt>Activities</dt>
        <dd className="num">{int(s ? s.nActivities : user.nActivities)}</dd>
        <dt>Super-unique</dt>
        <dd className="num">
          {s ? `${km(s.superUniqueM)} km` : '-'}
          {s && s.nSuperUniqueSegs > 0 && <span className="faint"> ({int(s.nSuperUniqueSegs)} segs)</span>}
        </dd>
        <dt>First activity</dt>
        <dd className="num">{s ? dateText(s.firstActivityMs) : '-'}</dd>
        <dt>Last activity</dt>
        <dd className="num">{s ? dateText(s.lastActivityMs) : '-'}</dd>
      </dl>
    </section>
  );
}
