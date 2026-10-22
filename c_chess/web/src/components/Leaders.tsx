'use client';
// Leaders.tsx - the leaderboard (GET /c/top), read by the kernel; refreshed every 30 s.
import { useEffect, useState } from 'react';
import Link from 'next/link';
import type { CvTop_Snap } from '@/gen/web_layout';
import { net } from '@/lib/net';

export default function Leaders({ limit }: { limit: number }) {
  const [top, setTop] = useState<CvTop_Snap | null>(null);
  useEffect(() => {
    let gone = false;
    const load = async () => {
      const r = await net.read('/c/top');
      if (!gone && r.status === 200) setTop(net.k!.top(r.body));
    };
    void load();
    const t = setInterval(load, 30000);
    return () => { gone = true; clearInterval(t); };
  }, []);
  const rows = top ? top.rows.slice(0, Math.min(limit, top.n)) : [];
  return (
    <table className="seeks leaders">
      <thead><tr><th>#</th><th>Leaderboard</th><th>Rating</th><th>Games</th></tr></thead>
      <tbody>
        {rows.map((r, i) => (
          <tr key={r.name}>
            <td className="muted">{i + 1}</td>
            <td><Link prefetch={false} href={`/user?name=${encodeURIComponent(r.name)}`}>{r.name}</Link></td>
            <td>{r.rating}{r.rd >= 110 ? '?' : ''}</td>
            <td className="muted">{r.games}</td>
          </tr>
        ))}
        {top && !top.n && <tr><td colSpan={4} className="muted" style={{ padding: 24, textAlign: 'center' }}>No rated games yet.</td></tr>}
      </tbody>
    </table>
  );
}
