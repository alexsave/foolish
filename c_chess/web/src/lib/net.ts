// net.ts - the page's one connection: the account (kept between visits), the /ws socket, and the
// kernel's view of it, as a store React subscribes to. The bytes go between the socket and the
// kernel untouched; what they mean is the kernel's.
import { CC_EV_EVAL, CC_EV_GAME, CC_EV_LOBBY, CC_EV_READ, CC_EV_TV, CW_AUTH_SIGNIN, CW_AUTH_SIGNUP } from '@/gen/web_layout';
import type { CvEval_Snap, CvLobby_Snap, CvMoves_Snap, CvTv_Snap, CvView_Snap } from '@/gen/web_layout';
import { Kernel, loadKernel } from './kernel';

const SESSION_KEY = 'cc.session';
const GUEST_KEY = 'cc.guest';

export type Phase = 'loading' | 'signing-in' | 'connecting' | 'online' | 'offline';

export interface Snapshot {
  phase: Phase;
  view: CvView_Snap | null;
  moves: CvMoves_Snap | null;
  lobby: CvLobby_Snap | null;
  tv: CvTv_Snap | null;
  eval: CvEval_Snap | null;
  /** events since the store last published, OR-ed (cleared by `consume`) */
  events: number;
}

const server = (): string => process.env.NEXT_PUBLIC_SERVER ?? (typeof location !== 'undefined' ? location.origin : '');

const b64 = (b: Uint8Array): string => btoa(String.fromCharCode(...b));
const unb64 = (s: string): Uint8Array => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

function store(key: string, value: string | null): void {
  try {
    if (value === null) localStorage.removeItem(key);
    else localStorage.setItem(key, value);
  } catch { /* private mode: the account lasts the visit */ }
}
function load(key: string): string | null {
  try { return localStorage.getItem(key); } catch { return null; }
}

class Net {
  k: Kernel | null = null;
  private ws: WebSocket | null = null;
  private listeners = new Set<() => void>();
  private snap: Snapshot = { phase: 'loading', view: null, moves: null, lobby: null, tv: null, eval: null, events: 0 };
  private started = false;
  private wantLobby = false;
  private wantGame = 0;
  private backoff = 250;
  private lastPly = -1;

  subscribe = (fn: () => void): (() => void) => {
    this.listeners.add(fn);
    if (!this.started) { this.started = true; void this.boot(); }
    return () => this.listeners.delete(fn);
  };
  get = (): Snapshot => this.snap;
  /** Takes the events gathered since the last call. */
  consume(): number {
    const e = this.snap.events;
    if (e) this.snap = { ...this.snap, events: 0 };
    return e;
  }

  private publish(patch: Partial<Snapshot>): void {
    this.snap = { ...this.snap, ...patch };
    for (const fn of this.listeners) fn();
  }

  /** Reads the kernel's view (and what changed with it) into the store. */
  refresh(): void {
    if (!this.k) return;
    const view = this.k.view(performance.now());
    const patch: Partial<Snapshot> = { view, events: this.snap.events | view.events };
    if (view.ply !== this.lastPly || (view.events & CC_EV_GAME)) {
      this.lastPly = view.ply;
      patch.moves = this.k.moves();
    }
    if (view.events & CC_EV_LOBBY) patch.lobby = this.k.lobbyView();
    if (view.events & CC_EV_TV) patch.tv = this.k.tv(performance.now());
    if (view.events & CC_EV_EVAL) patch.eval = this.k.evaluation();
    if (view.events & CC_EV_READ) {
      const done = this.reads.get(view.readTag);
      if (done) { this.reads.delete(view.readTag); done({ status: view.readStatus, body: this.k.readTake() }); }
    }
    this.publish(patch);
  }

  /** The live games' clocks, read again (the lobby's once-a-second tick). */
  tickTv(): void {
    if (this.k && this.snap.tv?.n) this.publish({ tv: this.k.tv(performance.now()) });
  }

  /** Only the clocks moved: a cheap view for the animation frame. */
  tick(): void {
    if (!this.k || !this.snap.view) return;
    const view = this.k.view(performance.now());
    if (view.events) { this.snap = { ...this.snap, events: this.snap.events | view.events }; this.refresh(); return; }
    this.publish({ view });
  }

  flush(): void {
    if (!this.k || !this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    for (const f of this.k.out()) this.ws.send(f);
  }

  private async post(path: string, body: Uint8Array): Promise<{ status: number; body: Uint8Array }> {
    const res = await fetch(server() + path, { method: 'POST', body: body as BodyInit, headers: { 'content-type': 'application/octet-stream' } });
    return { status: res.status, body: new Uint8Array(await res.arrayBuffer()) };
  }

  private async auth(name: string, password: string, mode: number): Promise<number> {
    const k = this.k!;
    const r = await this.post('/c/auth', k.authBody(name, password, mode));
    k.authAnswer(r.status, r.body);
    if (r.status === 200) store(SESSION_KEY, b64(r.body));
    return r.status;
  }

  /** A sign-in or sign-up by the person (the account page): 200, or the refusal's status. */
  async account(name: string, password: string, signup: boolean): Promise<{ status: number; code: number }> {
    if (!this.k) await this.ready();
    const status = await this.auth(name, password, signup ? CW_AUTH_SIGNUP : CW_AUTH_SIGNIN);
    const code = this.k!.view(performance.now()).authCode;
    if (status === 200) {
      store(GUEST_KEY, null);
      this.reconnect();
    }
    return { status, code };
  }

  signOut(): void {
    store(SESSION_KEY, null);
    store(GUEST_KEY, null);
    location.href = '/';
  }

  private readyWaiters: (() => void)[] = [];
  private ready(): Promise<void> {
    return new Promise((r) => (this.k ? r() : this.readyWaiters.push(r)));
  }

  private async boot(): Promise<void> {
    this.k = await loadKernel();
    for (const r of this.readyWaiters.splice(0)) r();
    this.publish({ phase: 'signing-in' });
    const saved = load(SESSION_KEY);
    if (saved) this.k.authAnswer(200, unb64(saved));
    else await this.guest();
    this.connect();
  }

  /** A first visit plays at once as a guest: an account with a random name and password, kept as
   * the kernel's opaque record (base64 in localStorage). */
  private async guest(): Promise<void> {
    const k = this.k!;
    let g: Uint8Array | null = null;
    try { const kept = load(GUEST_KEY); if (kept) g = unb64(kept); } catch { g = null; }   // an older format: a new guest
    let fresh = false;
    if (!g || !k.isGuest(g)) { g = k.guestNew(); store(GUEST_KEY, b64(g)); fresh = true; }
    const auth = async (mode: number) => {
      const r = await this.post('/c/auth', k.guestAuthBody(g, mode));
      k.authAnswer(r.status, r.body);
      if (r.status === 200) store(SESSION_KEY, b64(r.body));
      return r.status;
    };
    // a guest just made has no account yet: sign up first (a sign-in would be refused first)
    let st = await auth(fresh ? CW_AUTH_SIGNUP : CW_AUTH_SIGNIN);
    if (st !== 200) st = await auth(fresh ? CW_AUTH_SIGNIN : CW_AUTH_SIGNUP);
    if (st !== 200) {   // the name was taken meanwhile: another
      store(GUEST_KEY, null);
      await this.guest();
    }
  }

  /** The access token was refused: a new one from the refresh token, or sign in again. */
  private async renew(): Promise<void> {
    const k = this.k!;
    const r = await this.post('/c/refresh', k.refreshBody());
    k.authAnswer(r.status, r.body);
    if (r.status === 200) { store(SESSION_KEY, b64(r.body)); return; }
    store(SESSION_KEY, null);
    await this.guest();
  }

  private connect(): void {
    this.publish({ phase: 'connecting' });
    const url = new URL('/ws', server());
    url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
    const ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    let accepted = false;
    ws.onopen = () => {
      this.k!.hello();
      if (this.wantLobby) this.k!.lobby();
      if (this.wantGame) this.k!.open(this.wantGame);
      this.flush();
    };
    ws.onmessage = (e) => {
      this.k!.in(new Uint8Array(e.data as ArrayBuffer), performance.now());
      this.flush();
      const before = this.snap.view?.ready;
      this.refresh();
      if (!before && this.snap.view?.ready) { accepted = true; this.backoff = 250; this.publish({ phase: 'online' }); }
    };
    ws.onclose = () => {
      if (this.ws !== ws) return;
      this.ws = null;
      this.publish({ phase: 'offline' });
      const again = async () => {
        if (!accepted) await this.renew();   // closed before READY: the token was refused
        this.connect();
      };
      setTimeout(() => void again(), accepted ? this.backoff : 50);
      this.backoff = Math.min(this.backoff * 2, 5000);
    };
  }

  private reconnect(): void {
    const ws = this.ws;
    this.ws = null;
    ws?.close();
    this.connect();
  }

  lobby(on: boolean): void {
    this.wantLobby = on;
    if (!this.k) return;
    if (on) this.k.lobby(); else this.k.lobbyLeave();
    this.flush();
  }

  open(game: number): void {
    this.wantGame = game;
    if (!this.k) return;
    this.k.open(game);
    this.flush();
    this.refresh();
  }

  close(): void {
    this.wantGame = 0;
    this.k?.close();
    this.flush();
  }

  private reads = new Map<number, (r: { status: number; body: Uint8Array }) => void>();
  private nextRead = 1;

  /** A GET over the socket (the one sign-in, the one TLS handshake): its status and bytes. Waits
   * for the socket when it is not up yet. */
  async read(path: string): Promise<{ status: number; body: Uint8Array }> {
    if (!this.k) await this.ready();
    while (this.snap.phase !== 'online') await new Promise((r) => setTimeout(r, 50));
    const tag = this.nextRead++;
    return new Promise((done) => {
      this.reads.set(tag, done);
      this.run((k) => k.read(path, tag));
    });
  }

  /** Runs fn against the kernel, then sends what it queued and publishes what changed. */
  run(fn: (k: Kernel) => void): void {
    if (!this.k) return;
    fn(this.k);
    this.flush();
    this.refresh();
  }
}

export const net = new Net();
