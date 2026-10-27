'use client';
// arenas.ts - the arenas list (GET /c/arenas, read by the kernel) as React state, for the arenas page
// and the lobby's panel.
import { useEffect, useState } from 'react';
import type { CvArenaList_Snap } from '@/gen/web_layout';
import { net } from './net';

/** Loads the arenas now and every 15 s. */
export function useArenas(): CvArenaList_Snap | null {
  const [list, setList] = useState<CvArenaList_Snap | null>(null);
  useEffect(() => {
    let gone = false;
    const load = async () => {
      const r = await net.read('/c/arenas');
      if (!gone && r.status === 200) setList(net.k!.arenaList(r.body));
    };
    void load();
    const t = setInterval(load, 15000);
    return () => { gone = true; clearInterval(t); };
  }, []);
  return list;
}

