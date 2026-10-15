// session.ts - who is signed in, as this browser remembers it, without the kernel.
//
// The session itself (its tokens, their refresh) is the kernel's client's, kept
// by backend.ts. Every page draws the signed-in name, and the root providers
// must not load the kernel (e2e/validation/kernel_gate_validation.test.ts), so
// backend.ts also keeps the user's id and name here, as plain text, whenever
// the session changes; the providers read only this.

const USER_KEY = 'foolish.user';

export interface SignedInUser { id: string; username: string }

type Listener = () => void;
const listeners = new Set<Listener>();
let watching = false;

function storage(): Storage | null {
    try { return typeof localStorage === 'undefined' ? null : localStorage; } catch { return null; }
}

/** The user this browser holds a session for, or null. */
export function signedInUser(): SignedInUser | null {
    const kept = storage()?.getItem(USER_KEY);
    if (!kept) return null;
    const cut = kept.indexOf('\n');
    return cut > 0 ? { id: kept.slice(0, cut), username: kept.slice(cut + 1) } : null;
}

/** backend.ts: the session changed to `user` (null: signed out). */
export function rememberUser(user: SignedInUser | null): void {
    try {
        if (user) storage()?.setItem(USER_KEY, `${user.id}\n${user.username}`);
        else storage()?.removeItem(USER_KEY);
    } catch { /* private mode: this page still knows, through the listeners */ }
    for (const l of listeners) l();
}

/** Called whenever the signed-in user may have changed, here or in another tab. */
export function onUserChange(l: Listener): () => void {
    if (!watching && typeof window !== 'undefined') {
        watching = true;
        window.addEventListener('storage', (e) => { if (e.key === USER_KEY) for (const x of listeners) x(); });
    }
    listeners.add(l);
    return () => listeners.delete(l);
}
