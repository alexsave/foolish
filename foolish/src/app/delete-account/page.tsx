'use client';

// Standalone account-deletion page (docs/ORACLE_MONETIZATION_ENGINEERING.md §4).
// Google Play requires a deletion path that works WITHOUT the app; Apple requires
// an in-app path (that one lives in the native Settings screen). This page lets a
// user sign in with their username + password and delete their account, calling
// the same `delete-account` edge function the apps call.

import { useState } from 'react';
import supabase from '../../backend/Connector';
import { useAuth } from '../../contexts/AuthContext';

type Phase = 'idle' | 'working' | 'done' | 'error';

export default function DeleteAccountPage() {
  const { user_id, username, signIn, signOut } = useAuth();
  const [name, setName] = useState('');
  const [password, setPassword] = useState('');
  const [confirm, setConfirm] = useState(false);
  const [phase, setPhase] = useState<Phase>('idle');
  const [message, setMessage] = useState('');

  const doSignIn = async (e: React.FormEvent) => {
    e.preventDefault();
    setPhase('working');
    setMessage('');
    try {
      await signIn(name, password);
      setPhase('idle');
    } catch (err) {
      setPhase('error');
      setMessage(err instanceof Error ? err.message : 'Sign-in failed.');
    }
  };

  const doDelete = async () => {
    setPhase('working');
    setMessage('');
    try {
      const { error } = await supabase.functions.invoke('delete-account', { method: 'POST' });
      if (error) throw error;
      await signOut();
      setPhase('done');
    } catch (err) {
      setPhase('error');
      setMessage(err instanceof Error ? err.message : 'Deletion failed. Please try again.');
    }
  };

  return (
    <main style={{ maxWidth: 460, margin: '0 auto', padding: '48px 20px', color: '#EDE9DF' }}>
      <h1 style={{ fontSize: 24, marginBottom: 16 }}>Delete your Foolish account</h1>

      {phase === 'done' ? (
        <p>Your account and personal data have been deleted. Game replays are kept
          in anonymized form. You can close this page.</p>
      ) : user_id ? (
        <>
          <p style={{ marginBottom: 16 }}>
            Signed in as <strong>{username}</strong>. Deleting your account is
            permanent: it removes your login and scrubs your name from game
            history. This cannot be undone.
          </p>
          <label style={{ display: 'flex', gap: 8, marginBottom: 16 }}>
            <input type="checkbox" checked={confirm} onChange={(e) => setConfirm(e.target.checked)} />
            <span>I understand this permanently deletes my account.</span>
          </label>
          <button
            onClick={doDelete}
            disabled={!confirm || phase === 'working'}
            style={{ background: '#C82B24', color: '#fff', border: 0, borderRadius: 8,
                     padding: '12px 20px', fontSize: 16, cursor: confirm ? 'pointer' : 'not-allowed',
                     opacity: confirm ? 1 : 0.5 }}
          >
            {phase === 'working' ? 'Deleting…' : 'Delete my account'}
          </button>
        </>
      ) : (
        <>
          <p style={{ marginBottom: 16 }}>Sign in to delete your account.</p>
          <form onSubmit={doSignIn} style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
            <input placeholder="Username" value={name} onChange={(e) => setName(e.target.value)}
                   autoCapitalize="none" style={inputStyle} />
            <input placeholder="Password" type="password" value={password}
                   onChange={(e) => setPassword(e.target.value)} style={inputStyle} />
            <button type="submit" disabled={phase === 'working'}
                    style={{ background: '#1E2A24', color: '#EDE9DF', border: '1px solid #9AA69E',
                             borderRadius: 8, padding: '12px 20px', fontSize: 16 }}>
              {phase === 'working' ? 'Signing in…' : 'Sign in'}
            </button>
          </form>
        </>
      )}

      {phase === 'error' && <p style={{ color: '#C82B24', marginTop: 16 }}>{message}</p>}
    </main>
  );
}

const inputStyle: React.CSSProperties = {
  background: '#14231C', color: '#EDE9DF', border: '1px solid #9AA69E',
  borderRadius: 8, padding: '12px', fontSize: 16,
};
