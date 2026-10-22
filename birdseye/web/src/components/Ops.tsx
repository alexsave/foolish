'use client';

import { int, uptime, usToText, secondsText, type WOps } from '../api';

interface Props {
  ops: WOps | null;
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
            <Tile label="Moves/s" value={ops.movesPerS.toFixed(1)} />
            <Tile label="Games/min" value={(ops.gamesPerS * 60).toFixed(1)} />
            <Tile label="Pushes/s" value={ops.pushesPerS.toFixed(0)} />
            <Tile label="Move ack p50" value={usToText(ops.actP50Us)} />
            <Tile label="Move ack p99" value={usToText(ops.actP99Us)} />
            <Tile label="Move ack max" value={usToText(ops.actMaxUs)} />
            <Tile label="Move to push p50" value={usToText(ops.pushP50Us)} />
            <Tile label="Move to push p99" value={usToText(ops.pushP99Us)} />
            <Tile label="Cast decode p99" value={usToText(ops.castP99Us)} />
            <Tile label="Other req p50" value={usToText(ops.reqP50Us)} />
            <Tile label="Other req p99" value={usToText(ops.reqP99Us)} />
            <Tile label="Push bytes/s" value={(ops.pushBytesPerS / 1024).toFixed(0)} unit="KB" />
            <Tile label="Server RSS" value={int(ops.serverRssMb)} unit="MB" />
            <Tile label="Server CPU" value={(ops.serverCpuCpct / 100).toFixed(2)} unit="% of a core" />
            <Tile label="Sim RSS" value={int(ops.selfRssMb)} unit="MB" />
            <Tile label="Tables live" value={int(ops.tablesLive)} />
            <Tile label="Playing / lobby" value={`${int(ops.tablesPlaying)} / ${int(ops.tablesLobby)}`} />
            <Tile label="Games done" value={int(ops.gamesTotal)} />
            <Tile label="Players seated" value={`${int(ops.playersSeated)} / ${int(ops.players)}`} />
            <Tile label="Bots seated" value={int(ops.botsSeated)} />
            <Tile label="Sockets" value={int(ops.sockets)} />
            <Tile label="Moves" value={int(ops.movesTotal)} />
            <Tile label="Refused" value={int(ops.refusedTotal)} bad={ops.refusedTotal > 0} />
            <Tile label="Hostile / taken" value={`${int(ops.hostileTotal)} / ${int(ops.hostileAccepted)}`} bad={ops.hostileAccepted > 0} />
            <Tile label="Resyncs" value={int(ops.resyncsTotal)} />
            <Tile label="Stalls" value={int(ops.stallsTotal)} bad={ops.stallsTotal > 0} />
            <Tile label="Game p50 / p99" value={`${secondsText(ops.gameP50S)} / ${secondsText(ops.gameP99S)}`} />
            <Tile label="Uptime" value={uptime(ops.uptimeS)} />
          </div>
          <div className="ops-note">
            Latencies are measured by the fake humans: a move&apos;s request out to its reply in, and to the mover&apos;s own cast of it; {int(ops.actN)} moves so far.
          </div>
        </>
      )}
    </section>
  );
}
