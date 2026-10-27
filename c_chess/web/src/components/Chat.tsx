'use client';
// Chat.tsx - a room's chat ("g<id>", "a<id>", "lobby"): its lines with who said them and when, an
// input that says a line on Enter, why a line will not go (the kernel's check before it is sent,
// the server's refusal after), and "Sign in to chat" for a guest. The list stays pinned to its
// newest line unless the reader has scrolled up to read.
import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import Link from 'next/link';
import { net } from '@/lib/net';
import { useNet } from '@/lib/use';
import { CHAT_CHECK, chatRefusal, clockTime } from '@/lib/words';

export default function Chat({ room, title = 'Chat', className = '' }: { room: string; title?: string; className?: string }) {
  const s = useNet();
  useEffect(() => net.chatWatch(room), [room]);
  // read again when a line arrives (s.chat), and when the kernel has loaded or the reader changed
  const chat = useMemo(() => net.chatView(room), [room, s.chat, s.phase, s.view?.me]);
  const [text, setText] = useState('');
  const [err, setErr] = useState('');
  const [busy, setBusy] = useState(false);
  const list = useRef<HTMLDivElement>(null);
  const pinned = useRef(true);
  useLayoutEffect(() => {
    const el = list.current;
    if (el && pinned.current) el.scrollTop = el.scrollHeight;
  }, [chat]);
  useEffect(() => { setErr(''); setText(''); }, [room]);

  const lines = chat ? chat.lines.slice(0, chat.n) : [];
  const guest = chat?.guest === 1;
  const check = text ? net.chatCheck(text) : 0;
  const warn = check ? CHAT_CHECK[check] : '';
  const send = async () => {
    if (busy || !text.trim() || check) return;
    setBusy(true);
    const r = await net.chatSay(room, text);
    setBusy(false);
    if (r.status === 200) { setText(''); setErr(''); pinned.current = true; }
    else setErr(r.err ? chatRefusal(r.err.code, r.err.why, r.err.waitS) : 'The line did not go. Try again.');
  };
  const note = warn || err;
  return (
    <div className={`chat ${className}`}>
      <div className="chat-h">{title}</div>
      <div
        className="chat-lines" ref={list} aria-live="polite"
        onScroll={(e) => { const el = e.currentTarget; pinned.current = el.scrollHeight - el.scrollTop - el.clientHeight < 24; }}
      >
        {lines.map((l, i) => (
          <div key={`${l.at}-${i}`} className={`chat-line ${l.mine ? 'mine' : ''}`}>
            <span className="t">{clockTime(l.at)}</span>
            <span className="w">
              <Link prefetch={false} className="n" href={`/user?name=${encodeURIComponent(l.name)}`}>{l.name}</Link>
              {l.text}
            </span>
          </div>
        ))}
        {chat && !lines.length && <div className="chat-empty muted">No one has said anything yet.</div>}
      </div>
      {guest ? (
        <div className="chat-in guest"><Link prefetch={false} href="/account">Sign in to chat</Link></div>
      ) : (
        <form className="chat-in" onSubmit={(e) => { e.preventDefault(); void send(); }}>
          <input
            value={text} placeholder="Say something" aria-label={`${title}: say something`} enterKeyHint="send"
            aria-invalid={note ? true : undefined} disabled={s.phase !== 'online'}
            onChange={(e) => { setText(e.target.value); setErr(''); }}
          />
        </form>
      )}
      {note && !guest && <div className="chat-note err" role="alert">{note}</div>}
    </div>
  );
}
