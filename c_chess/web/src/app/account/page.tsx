'use client';
// Sign in, or make an account (a guest's games stay with the guest).
import { useState } from 'react';
import { useRouter } from 'next/navigation';
import { CW_E_BAD_LOGIN, CW_E_BAD_NAME, CW_E_NAME_TAKEN, CW_E_TOO_MANY, CW_E_WEAK_PASSWORD } from '@/gen/web_layout';
import { net } from '@/lib/net';

const WHY: Record<number, string> = {
  [CW_E_NAME_TAKEN]: 'That name is taken.',
  [CW_E_BAD_LOGIN]: 'Wrong name or password.',
  [CW_E_WEAK_PASSWORD]: 'Use a password of at least 8 characters that is not too common.',
  [CW_E_BAD_NAME]: 'A name is 3 to 20 letters, digits, - or _.',
  [CW_E_TOO_MANY]: 'Too many attempts. Wait a little and try again.',
};

export default function Account() {
  const [name, setName] = useState('');
  const [pw, setPw] = useState('');
  const [err, setErr] = useState('');
  const [busy, setBusy] = useState(false);
  const router = useRouter();
  const go = async (signup: boolean) => {
    setBusy(true);
    setErr('');
    const r = await net.account(name.trim(), pw, signup);
    setBusy(false);
    if (r.status === 200) router.push('/');
    else setErr(WHY[r.code] ?? 'Something went wrong. Try again.');
  };
  return (
    <form className="panel form" onSubmit={(e) => { e.preventDefault(); void go(false); }}>
      <h1>Sign in</h1>
      <input placeholder="Name" autoComplete="username" value={name} onChange={(e) => setName(e.target.value)} />
      <input placeholder="Password" type="password" autoComplete="current-password" value={pw} onChange={(e) => setPw(e.target.value)} />
      {err && <div className="err">{err}</div>}
      <button className="btn primary" disabled={busy} type="submit">Sign in</button>
      <button className="btn" disabled={busy} type="button" onClick={() => void go(true)}>Create this account</button>
    </form>
  );
}
