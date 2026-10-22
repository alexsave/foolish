'use client';

import { dateText, int, km, miles, points, TYPE_LABEL, USER_KIND, type UserDetail, type WUser } from '../api';

interface Props {
  user: WUser | null;
  detail: UserDetail | null;
}

export function StatsCard({ user, detail }: Props) {
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
