'use client';
// Top.tsx - the bar: the site's name, the pages, who you are and whether the socket is up.
import Link from 'next/link';
import { usePathname } from 'next/navigation';
import { useNet } from '@/lib/use';

export default function Top() {
  const { phase, view } = useNet();
  const path = usePathname();
  const me = view?.me ?? '';
  const on = (p: string) => (path === p ? 'on' : '');
  return (
    <header className="top">
      <Link prefetch={false} href="/" className="logo">c<b>_</b>chess</Link>
      <nav className="nav">
        <Link prefetch={false} href="/" className={on('/')}>Play</Link>
        {me && <Link prefetch={false} href={`/user?name=${encodeURIComponent(me)}`} className={on('/user')}>Profile</Link>}
        <Link prefetch={false} href="/about" className={on('/about')}>About</Link>
      </nav>
      <div className="me">
        <span className={`dotline ${phase === 'online' ? 'on' : ''}`} title={phase} />
        {me ? <span>{me}</span> : <span>{phase === 'online' ? '' : 'connecting'}</span>}
        {me.startsWith('guest-') && <Link prefetch={false} href="/account">Sign in</Link>}
      </div>
    </header>
  );
}
