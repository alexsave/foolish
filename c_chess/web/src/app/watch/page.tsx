'use client';
// Every game in play the server lists (the highest rated first), live.
import { useEffect } from 'react';
import MiniBoard from '@/components/MiniBoard';
import { net } from '@/lib/net';
import { useNet } from '@/lib/use';

export default function Watch() {
  const s = useNet();
  useEffect(() => { net.lobby(true); return () => net.lobby(false); }, []);
  useEffect(() => { const t = setInterval(() => net.tickTv(), 1000); return () => clearInterval(t); }, []);
  return (
    <section>
      <h1>Games in play</h1>
      {s.tv?.n ? (
        <div className="minis wide">{s.tv.games.slice(0, s.tv.n).map((g) => <MiniBoard key={g.game} g={g} />)}</div>
      ) : (
        <div className="panel muted" style={{ padding: 32, textAlign: 'center' }}>No games in play right now.</div>
      )}
    </section>
  );
}
