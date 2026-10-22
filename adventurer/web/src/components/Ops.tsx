'use client';

import { int, mb, uptime, usToText, type WOps } from '../api';

interface Props {
  ops: WOps | null;
  spark: number[];
}

function Tile({ label, value, unit, title }: { label: string; value: string; unit?: string; title?: string }) {
  return (
    <div className="tile" title={title}>
      <div className="label">{label}</div>
      <div className="value num">
        {value}
        {unit && <small>{unit}</small>}
      </div>
    </div>
  );
}

// THE SIMULATED TIME the uploads so far stand for: the generator posts activities far faster than
// people ride, so the map after an hour of it is months of a real city. A person who rides, runs
// or walks three times a week (Strava's typical active user) makes 156 activities a year.
const ACTS_PER_USER_YEAR = 156;
function simulated(activities: number, users: number): string {
  if (users <= 0) return '-';
  const days = (activities / (users * ACTS_PER_USER_YEAR)) * 365;
  if (days < 14) return `${days.toFixed(days < 2 ? 1 : 0)} days`;
  if (days < 61) return `${(days / 7).toFixed(0)} weeks`;
  if (days < 730) return `${(days / 30.4).toFixed(0)} months`;
  return `${(days / 365).toFixed(1)} years`;
}

// The last 60 s of uploads/s as a filled line; the baseline is zero so bursts read as area.
function Sparkline({ values }: { values: number[] }) {
  const w = 120, h = 28;
  if (values.length < 2) return <svg className="spark" width={w} height={h} aria-hidden="true" />;
  const max = Math.max(1e-6, ...values);
  const n = values.length;
  const pts = values.map((v, i) => {
    const x = (i / (n - 1)) * (w - 2) + 1;
    const y = h - 2 - (v / max) * (h - 5);
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  });
  const area = `M1,${h - 1} L${pts.join(' L')} L${w - 1},${h - 1} Z`;
  return (
    <svg className="spark" width={w} height={h} viewBox={`0 0 ${w} ${h}`} aria-label="uploads per second, last 60 s">
      <path d={area} fill="#2f6fe4" opacity="0.12" />
      <polyline points={pts.join(' ')} fill="none" stroke="#2f6fe4" strokeWidth="1.5" strokeLinejoin="round" />
    </svg>
  );
}

export function Ops({ ops, spark }: Props) {
  return (
    <section className="card">
      <div className="ops-head">
        <h2 className="card-title">Server</h2>
        <Sparkline values={spark} />
      </div>
      {!ops ? (
        <div className="empty">Waiting for the server...</div>
      ) : (
        <div className="tiles">
          <Tile label="Uploads/s" value={ops.actPerS.toFixed(1)} />
          <Tile label="Tiles/s" value={ops.tilesPerS.toFixed(1)} />
          <Tile label="Queued live / backfill" value={`${int(ops.queueLive)} / ${int(ops.queueBackfill)}`} />
          <Tile label="Activity CPU p50" value={usToText(ops.actCpuP50Us)} />
          <Tile label="Activity CPU p99" value={usToText(ops.actCpuP99Us)} />
          <Tile label="Accuracy avg" value={`${(ops.accuracyAvg * 100).toFixed(1)}`} unit="%" />
          <Tile label="Tile p50" value={usToText(ops.tileP50Us)} />
          <Tile label="Tile p99" value={usToText(ops.tileP99Us)} />
          <Tile label="RSS" value={int(ops.rssMb)} unit="MB" />
          <Tile label="End-to-end p50" value={usToText(ops.e2eP50Us)} />
          <Tile label="End-to-end p99" value={usToText(ops.e2eP99Us)} />
          <Tile label="Journal" value={mb(ops.journalBytes)} unit="MB" />
          <Tile label="Users" value={int(ops.users)} />
          <Tile label="Activities" value={int(ops.activities)} />
          <Tile
            label="Simulated time"
            value={simulated(ops.activities, ops.users)}
            title={`The activities so far are what ${int(ops.users)} people make in this long, each active three times a week: the generator runs far faster than life, so the map is not an hour of a real city.`}
          />
          <Tile label="Rejected" value={int(ops.rejected)} />
          <Tile label="Feed / HTTP" value={`${int(ops.feedConns)} / ${int(ops.httpConns)}`} />
          <Tile label="Threads" value={int(ops.threads)} />
          <Tile label="Graph segments" value={int(ops.segs)} />
          <Tile label="Graph km" value={int(Math.round(ops.totalLenKm[2]))} />
          <Tile label="Regions" value={int(ops.regions)} />
          <Tile label="Uptime" value={uptime(ops.uptimeS)} />
          <Tile label="Replayed at start" value={int(ops.recovered)} />
          <Tile label="Bike / foot km" value={`${(ops.totalLenKm[0] / 1000).toFixed(0)}k / ${(ops.totalLenKm[1] / 1000).toFixed(0)}k`} />
        </div>
      )}
    </section>
  );
}
