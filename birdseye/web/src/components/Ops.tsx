'use client';

import { gauge, int, metric, perS, uptime, usToText, secondsText, type Metrics } from '../api';

interface Props {
  ops: Metrics | null;
  spark: number[];
}

function Tile({ label, value, unit, bad }: { label: string; value: string; unit?: string; bad?: boolean }) {
  return (
    <div className={`tile${bad ? ' bad' : ''}`}>
      <div className="label">{label}</div>
      <div className="value num">
        {value}
        {unit && <small>{unit}</small>}
      </div>
    </div>
  );
}

// A latency's p50 and p99 in one tile ("-" before the first sample).
function pair(ops: Metrics, name: string): string {
  const r = metric(ops, name);
  if (!r || Number(r.total) === 0) return '-';
  return `${usToText(r.p50)} / ${usToText(r.p99)}`;
}

// The last 60 s of moves/s as a filled line; the baseline is zero so bursts read as area.
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
    <svg className="spark" width={w} height={h} viewBox={`0 0 ${w} ${h}`} aria-label="moves per second, last 60 s">
      <path d={area} fill="#2f6fe4" opacity="0.12" />
      <polyline points={pts.join(' ')} fill="none" stroke="#2f6fe4" strokeWidth="1.5" strokeLinejoin="round" />
    </svg>
  );
}

export function Ops({ ops, spark }: Props) {
  return (
    <section className="card">
      <div className="ops-head">
        <h2 className="card-title">Numbers</h2>
        <Sparkline values={spark} />
      </div>
      {!ops ? (
        <div className="empty">Waiting for birdseye...</div>
      ) : (
        <>
          <div className="tiles">
            <Tile label="Moves/s" value={perS(ops, 'moves').toFixed(1)} />
            <Tile label="Games/min" value={(perS(ops, 'games') * 60).toFixed(1)} />
            <Tile label="Pushes/s" value={perS(ops, 'pushes').toFixed(0)} />
            <Tile label="Move ack p50" value={usToText(metric(ops, 'move_ack_us')?.p50 ?? 0)} />
            <Tile label="Move ack p99" value={usToText(metric(ops, 'move_ack_us')?.p99 ?? 0)} />
            <Tile label="Move ack max" value={usToText(Number(metric(ops, 'move_ack_us')?.max ?? 0n))} />
            <Tile label="Move to push p50" value={usToText(metric(ops, 'move_push_us')?.p50 ?? 0)} />
            <Tile label="Move to push p99" value={usToText(metric(ops, 'move_push_us')?.p99 ?? 0)} />
            <Tile label="Cast decode p99" value={usToText(metric(ops, 'cast_decode_us')?.p99 ?? 0)} />
            <Tile label="Create p50 / p99" value={pair(ops, 'req_create_us')} />
            <Tile label="Ask p50 / p99" value={pair(ops, 'req_ask_us')} />
            <Tile label="Read p50 / p99" value={pair(ops, 'req_read_us')} />
            <Tile label="Push bytes/s" value={(perS(ops, 'push_bytes') / 1024).toFixed(0)} unit="KB" />
            <Tile label="Server RSS" value={int(gauge(ops, 'server_rss_mb'))} unit="MB" />
            <Tile label="Server CPU" value={(gauge(ops, 'server_cpu_cpct') / 100).toFixed(2)} unit="% of a core" />
            <Tile label="Sim RSS" value={int(ops.hdr.rssMb)} unit="MB" />
            <Tile label="Tables live" value={int(gauge(ops, 'tables_live'))} />
            <Tile label="Playing / lobby" value={`${int(gauge(ops, 'tables_playing'))} / ${int(gauge(ops, 'tables_lobby'))}`} />
            <Tile label="Games done" value={int(Number(metric(ops, 'games')?.total ?? 0n))} />
            <Tile label="Players seated" value={`${int(gauge(ops, 'players_seated'))} / ${int(gauge(ops, 'players'))}`} />
            <Tile label="Bots seated" value={int(gauge(ops, 'bots_seated'))} />
            <Tile label="Sockets" value={int(gauge(ops, 'sockets'))} />
            <Tile label="Moves" value={int(Number(metric(ops, 'moves')?.total ?? 0n))} />
            <Tile label="Refused" value={int(gauge(ops, 'refused'))} bad={gauge(ops, 'refused') > 0} />
            <Tile label="Hostile / taken" value={`${int(gauge(ops, 'hostile'))} / ${int(gauge(ops, 'hostile_taken'))}`} bad={gauge(ops, 'hostile_taken') > 0} />
            <Tile label="Resyncs" value={int(gauge(ops, 'resyncs'))} />
            <Tile label="Stalls" value={int(gauge(ops, 'stalls'))} bad={gauge(ops, 'stalls') > 0} />
            <Tile label="Game p50 / p99" value={`${secondsText(metric(ops, 'game_s')?.p50 ?? 0)} / ${secondsText(metric(ops, 'game_s')?.p99 ?? 0)}`} />
            <Tile label="Uptime" value={uptime(ops.hdr.uptimeS)} />
          </div>
          <div className="ops-note">
            Latencies are measured by the fake humans: a move&apos;s request out to its reply in, and to the mover&apos;s own cast of it;{' '}
            {int(Number(metric(ops, 'move_ack_us')?.total ?? 0n))} moves so far. The platform&apos;s own rows: {int(Number(metric(ops, 'requests')?.total ?? 0n))} requests,{' '}
            {int(gauge(ops, 'ws_conns'))} stream sockets.
          </div>
        </>
      )}
    </section>
  );
}
