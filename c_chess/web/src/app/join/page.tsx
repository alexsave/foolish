'use client';
// A challenge link: accepting it starts the game.
import { Suspense, useEffect, useState } from 'react';
import { useRouter, useSearchParams } from 'next/navigation';
import { CC_EV_ERROR, CC_EV_START, CW_E_SELF } from '@/gen/web_layout';
import { net } from '@/lib/net';
import { useNet } from '@/lib/use';

function Join() {
  const id = Number(useSearchParams().get('c') ?? 0);
  const s = useNet();
  const router = useRouter();
  const [msg, setMsg] = useState('Joining the game...');
  const online = s.phase === 'online';
  useEffect(() => { if (id && online) net.run((k) => k.accept(id)); }, [id, online]);
  useEffect(() => {
    const e = net.consume();
    if ((e & CC_EV_START) && s.view?.startGame) router.replace(`/game?id=${s.view.startGame}`);
    else if (e & CC_EV_ERROR) setMsg(s.view?.errCode === CW_E_SELF ? 'This is your own link. Send it to the person you want to play; the game starts when they open it.' : 'This challenge has been taken or has expired.');
  }, [s.events, s.view, router]);
  return <div className="panel form"><h1>Challenge</h1><p className="muted">{msg}</p></div>;
}

export default function Page() { return <Suspense><Join /></Suspense>; }
