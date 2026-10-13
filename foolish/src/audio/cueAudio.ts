// cueAudio.ts - the web's sound engine: a cue id in, a sound out, on time.
//
// WHAT THIS OWNS: the AudioContext and when it may exist, the decoded files,
// the master level and the mute, and turning "strike cue K in D milliseconds"
// into a source started at an exact time on the audio clock.
//
// WHAT IT DOES NOT: which cue a beat makes or when - the kernel's
// (c/src/anim_plan.h "sound cues") - and which file a cue plays - the table's
// (src/audio/cueSounds.ts). There is no event type, no card and no game in this
// file, and there must not be: it is the playback half of a boundary, and a
// second sound design starts the day it learns what an attack is.
//
// THE CLOCK. A strike is scheduled, not played: `start(ctx.currentTime + D)`.
// The audio clock is driven by the sound card and does not drift against the
// samples it is timing, where a setTimeout fires when the main thread gets
// round to it. The frame loop hands cues over a little early
// (src/audio/cueSchedule.ts) precisely so that D is never zero for a sound that
// has to land on a card.
//
// WHEN IT IS SILENT, and each is a deliberate no-op rather than an error:
//   - before the first tap or key. Browsers refuse to start audio a user did
//     not ask for, so the context is not even created until a gesture; the
//     sounds of a deal that plays before one are simply not heard.
//   - while muted. Nothing is fetched or decoded for a player who has it off.
//   - while the tab is hidden. A game that makes noise from a background tab
//     is the tab you close.
//   - where there is no Web Audio at all (Node, jsdom, an old browser).
//   - when a file did not load. That cue is silent and the rest play.
//
// NOT TIED TO prefers-reduced-motion, on purpose. That setting is about
// vestibular triggers - things sliding and zooming on a screen - and a sound is
// not one. If anything it is the reverse: a player who has motion turned down
// is leaning on the other channels to follow the table. The control for sound
// is the mute, which is one tap and remembered.

import { CUE_SOUNDS, CUE_SOUND_ROOT, type CueSound } from './cueSounds';

const MUTED_KEY = 'foolish_sound_muted';
const VOLUME_KEY = 'foolish_sound_volume';
const DEFAULT_VOLUME = 0.7;

/** What a screen draws the sound control from. One object per change, so it
 *  compares by identity (useSyncExternalStore). */
export interface CueAudioState { muted: boolean; volume: number }

const SERVER_STATE: CueAudioState = { muted: false, volume: DEFAULT_VOLUME };

type ContextCtor = new (options?: AudioContextOptions) => AudioContext;

/** The browser's AudioContext, or null where there is none. Read each time it
 *  is wanted rather than at import: this module is imported on the server. */
const contextCtor = (): ContextCtor | null => {
    const g = globalThis as { AudioContext?: ContextCtor; webkitAudioContext?: ContextCtor };
    return g.AudioContext ?? g.webkitAudioContext ?? null;
};

const stored = (key: string): string | null => {
    try { return typeof localStorage === 'undefined' ? null : localStorage.getItem(key); } catch { return null; }
};
const store = (key: string, value: string): void => {
    try { if (typeof localStorage !== 'undefined') localStorage.setItem(key, value); } catch { /* private mode */ }
};

export class CueAudio {
    private ctx: AudioContext | null = null;
    private master: GainNode | null = null;
    private readonly buffers = new Map<string, AudioBuffer>();
    private readonly asked = new Set<string>();
    private readonly live = new Set<AudioBufferSourceNode>();
    private readonly lastVariant = new Map<number, number>();
    private readonly listeners = new Set<() => void>();
    private state: CueAudioState | null = null;
    private armed = false;
    /** [0, 1). Replaceable so a test can hear the same thing twice. */
    random: () => number = Math.random;

    // ---- the setting ---------------------------------------------------------

    /** The mute and the level, read from this browser the first time they are
     *  asked for. Two plain keys: a setting is two scalars. */
    private settings(): CueAudioState {
        if (!this.state) {
            const v = Number.parseFloat(stored(VOLUME_KEY) ?? '');
            this.state = {
                muted: stored(MUTED_KEY) === '1',
                volume: Number.isFinite(v) ? Math.min(1, Math.max(0, v)) : DEFAULT_VOLUME,
            };
        }
        return this.state;
    }

    readonly subscribe = (onChange: () => void): (() => void) => {
        this.listeners.add(onChange);
        return () => { this.listeners.delete(onChange); };
    };
    readonly getState = (): CueAudioState => this.settings();
    /** What the server renders: the default, so the first client paint matches. */
    readonly getServerState = (): CueAudioState => SERVER_STATE;

    private change(next: CueAudioState): void {
        this.state = next;
        store(MUTED_KEY, next.muted ? '1' : '0');
        store(VOLUME_KEY, String(next.volume));
        if (this.master && this.ctx) {
            this.master.gain.setValueAtTime(next.muted ? 0 : next.volume, this.ctx.currentTime);
        }
        for (const l of this.listeners) l();
    }

    /** Mute or unmute. Call it from the tap itself: unmuting is what creates
     *  the context for a player who arrived muted, and that needs a gesture. */
    setMuted(muted: boolean): void {
        this.change({ ...this.settings(), muted });
        if (muted) this.cancel();
        else this.unlock();
    }

    /** The master level, 0..1. */
    setVolume(volume: number): void {
        this.change({ ...this.settings(), volume: Math.min(1, Math.max(0, volume)) });
    }

    // ---- the context -----------------------------------------------------------

    /**
     * Listen for the gesture that lets audio start. Idempotent, and a no-op off
     * the browser. The listeners STAY: a phone suspends the context again after
     * a call or a trip to another app, and only another gesture brings it back.
     */
    arm(): void {
        if (this.armed || typeof window === 'undefined') return;
        this.armed = true;
        const wake = () => this.unlock();
        window.addEventListener('pointerdown', wake, { capture: true, passive: true });
        window.addEventListener('keydown', wake, { capture: true, passive: true });
        document.addEventListener('visibilitychange', () => { if (document.hidden) this.cancel(); });
    }

    /** Create or wake the context. Only does anything inside a user gesture,
     *  which is the browser's rule, not ours. */
    unlock(): void {
        if (this.settings().muted) return;
        if (this.ctx) {
            if (this.ctx.state !== 'running') void this.ctx.resume().catch(() => {});
            return;
        }
        const Ctor = contextCtor();
        if (!Ctor) return;
        try {
            this.ctx = new Ctor({ latencyHint: 'interactive' });
        } catch {
            return;
        }
        this.master = this.ctx.createGain();
        this.master.gain.value = this.settings().volume;
        this.master.connect(this.ctx.destination);
        if (this.ctx.state !== 'running') void this.ctx.resume().catch(() => {});
        this.load();
    }

    /** Fetch and decode every file the table names, once each. A file that
     *  does not arrive leaves its cue silent; nothing waits on this. */
    private load(): void {
        const ctx = this.ctx;
        if (!ctx || typeof fetch === 'undefined') return;
        for (const sound of Object.values(CUE_SOUNDS)) {
            for (const file of sound?.files ?? []) {
                if (this.asked.has(file)) continue;
                this.asked.add(file);
                fetch(CUE_SOUND_ROOT + file)
                    .then((r) => (r.ok ? r.arrayBuffer() : Promise.reject(new Error(String(r.status)))))
                    // The callback form: older Safari has no promise from it.
                    .then((bytes) => new Promise<AudioBuffer>((ok, no) => { ctx.decodeAudioData(bytes, ok, no); }))
                    .then((buffer) => { this.buffers.set(file, buffer); })
                    .catch(() => { /* this cue stays silent */ });
            }
        }
    }

    /** Has this file arrived and decoded? For the audition page, which says
     *  so beside each one rather than playing silence. */
    loaded(file: string): boolean { return this.buffers.has(file); }

    // ---- playing ---------------------------------------------------------------

    /** Which variant to play: any but the one this cue played last. */
    private variant(kind: number, sound: CueSound): string {
        const n = sound.files.length;
        if (n === 1) return sound.files[0];
        const last = this.lastVariant.get(kind) ?? -1;
        const pick = last < 0
            ? Math.floor(this.random() * n)
            : (last + 1 + Math.floor(this.random() * (n - 1))) % n;
        this.lastVariant.set(kind, pick);
        return sound.files[pick];
    }

    /**
     * Strike cue `kind` (an ANIM_CUE_*) `delayMs` from now, standing for
     * `nCards` cards. Returns whether a sound was scheduled; false is every
     * silent case in this file's header and is never an error.
     */
    strike(kind: number, delayMs = 0, nCards = 1): boolean {
        const ctx = this.ctx, master = this.master;
        if (!ctx || !master || this.settings().muted || ctx.state !== 'running') return false;
        if (typeof document !== 'undefined' && document.hidden) return false;
        const sound = CUE_SOUNDS[kind];
        if (!sound) return false;
        const buffer = this.buffers.get(this.variant(kind, sound));
        if (!buffer) return false;

        const source = ctx.createBufferSource();
        source.buffer = buffer;
        // A little off true each time, so a row of one sample is a row of
        // cards and not a machine.
        source.playbackRate.value = 1 + sound.pitch * (2 * this.random() - 1);
        const gain = ctx.createGain();
        gain.gain.value = sound.gain
            * (1 + sound.level * (2 * this.random() - 1))
            * (1 + Math.min(0.6, sound.perCard * Math.max(0, nCards - 1)));
        source.connect(gain);
        gain.connect(master);
        source.onended = () => {
            this.live.delete(source);
            gain.disconnect();
        };
        this.live.add(source);
        source.start(ctx.currentTime + Math.max(0, delayMs) / 1000);
        return true;
    }

    /** Stop everything scheduled or sounding: a replay was scrubbed, the tab
     *  went away, the player hit mute. */
    cancel(): void {
        for (const source of this.live) {
            try { source.stop(); } catch { /* never started */ }
        }
        this.live.clear();
    }
}

/** The one engine a page has. */
export const cueAudio = new CueAudio();
