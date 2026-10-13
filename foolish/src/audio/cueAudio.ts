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
// THE ORDER THE FILES ARRIVE IN. Every sound is loaded, but not all at once and
// not in the table's order: the kernel says which cues could be the NEXT thing
// heard from the board on screen (c/src/anim_plan.h anim_cues_next), the page
// passes that on as `want`, and those files go to the front of the queue. A few
// fetches run at a time so that the front of the queue means something; one
// already on its way is never restarted, and none is asked for twice.
//
//   BEFORE THE FIRST GESTURE only the wanted files are fetched, and only as
//   bytes: a browser will not let a page make sound unasked, but it will let it
//   download, and the first thing a player hears is usually caused by the very
//   tap that unlocks audio. With the bytes already here that tap only has to
//   decode them (a few milliseconds), where fetching from cold loses the first
//   card of a deal. It is a few kilobytes, and nothing at all for a player who
//   is muted.
//
//   A STRIKE WHOSE FILE IS STILL ON ITS WAY WAITS FOR IT, briefly: it is
//   started the moment the file decodes if that is no more than LATE_S behind
//   the time it was aimed at, and dropped otherwise. A sound that is 20ms late
//   is a sound; one that is half a second late is a mistake.
//
// A REVERT IS ANOTHER CUE, BACKWARDS. The one cue id this file knows by name:
// ANIM_CUE_REVERT has no sound of its own, the kernel says which cue it undoes,
// and that cue's buffer is played reversed (reversed once, and kept).
//
// WHEN IT IS SILENT, and each is a deliberate no-op rather than an error:
//   - before the first tap or key. Browsers refuse to start audio a user did
//     not ask for, so the context is not even created until a gesture; the
//     sounds of a deal that plays before one are simply not heard.
//   - while muted. Nothing is fetched or decoded for a player who has it off.
//     (Before a gesture the wanted files' bytes are fetched, as above; the
//     context itself still waits.)
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

import { ANIM_CUE_REVERT } from '@sdk/ts/gen/kernel_consts.bots.ts';
import { CUE_SOUNDS, CUE_SOUND_ROOT, type CueSound } from './cueSounds';

const MUTED_KEY = 'foolish_sound_muted';
const VOLUME_KEY = 'foolish_sound_volume';
const DEFAULT_VOLUME = 0.7;
/** How many files are fetched at once. Enough to fill a connection, few enough
 *  that the order of the queue decides what arrives first. */
const MAX_FETCHES = 3;
/** How far behind its aim a strike that waited for its file may still start. */
const LATE_S = 0.08;

/** Where a file is between "not asked for" (absent) and decoded (in `buffers`). */
type Progress = 'fetching' | 'held' | 'decoding' | 'failed';

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
    private readonly live = new Set<AudioBufferSourceNode>();
    private readonly lastVariant = new Map<number, number>();
    private readonly listeners = new Set<() => void>();
    private state: CueAudioState | null = null;
    private armed = false;
    /** The cues that could sound next, one bit per ANIM_CUE_* (anim_cues_next). */
    private wanted = 0;
    private readonly progress = new Map<string, Progress>();
    /** Bytes fetched before there was a context to decode them with. */
    private readonly held = new Map<string, ArrayBuffer>();
    private fetching = 0;
    /** Strikes whose file had not decoded yet, with the time each was aimed at. */
    private waiting: { file: string; sound: CueSound; at: number; nCards: number; backwards: boolean }[] = [];
    /** A decoded file, sample for sample back to front, made the first time a
     *  revert needs it. */
    private readonly reversed = new Map<AudioBuffer, AudioBuffer>();
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
        // The master carries the LEVEL only. Mute is not a level of zero: it is
        // strike() refusing and cancel() stopping what was scheduled, so there
        // is one place that decides whether a muted player hears anything.
        if (this.master && this.ctx) this.master.gain.setValueAtTime(next.volume, this.ctx.currentTime);
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
            // A player who muted mid-load and came back: carry on where it stopped.
            this.pump();
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
        // What arrived before the gesture can be decoded now, and everything
        // else may be fetched.
        for (const [file, bytes] of this.held) this.decode(file, bytes);
        this.held.clear();
        this.pump();
    }

    // ---- loading ---------------------------------------------------------------

    /**
     * The cues that could sound next from the board on screen, one bit per
     * ANIM_CUE_* - the kernel's answer (anim_cues_next), passed on whenever the
     * board changes. Their files jump the queue. Nothing already fetching is
     * disturbed, and a cue that drops out of the set keeps what it has.
     */
    want(mask: number): void {
        if (mask === this.wanted) return;
        this.wanted = mask;
        this.pump();
    }

    /** The next file to ask for: a wanted cue's, in the table's order, and then
     *  - once there is a context to decode into - anything left. */
    private next(): string | null {
        let later: string | null = null;
        for (const [kind, sound] of Object.entries(CUE_SOUNDS)) {
            for (const file of sound?.files ?? []) {
                if (this.progress.has(file) || this.buffers.has(file)) continue;
                if (this.wanted & (1 << Number(kind))) return file;
                later ??= file;
            }
        }
        return this.ctx ? later : null;
    }

    /** Start as many fetches as there is room for. Called whenever something
     *  that decides the queue changes: a want, a gesture, a fetch finishing. */
    private pump(): void {
        if (this.settings().muted || typeof fetch === 'undefined' || !contextCtor()) return;
        for (let file = this.next(); file !== null && this.fetching < MAX_FETCHES; file = this.next()) {
            this.fetchFile(file);
        }
    }

    private fetchFile(file: string): void {
        this.progress.set(file, 'fetching');
        this.fetching++;
        fetch(CUE_SOUND_ROOT + file)
            .then((r) => (r.ok ? r.arrayBuffer() : Promise.reject(new Error(String(r.status)))))
            .then((bytes) => {
                this.fetching--;
                if (this.ctx) this.decode(file, bytes);
                else { this.held.set(file, bytes); this.progress.set(file, 'held'); }
            }, () => {
                this.fetching--;
                this.fail(file);
            })
            .then(() => this.pump());
    }

    private decode(file: string, bytes: ArrayBuffer): void {
        const ctx = this.ctx;
        if (!ctx) return;
        this.progress.set(file, 'decoding');
        // The callback form: older Safari has no promise from it.
        ctx.decodeAudioData(bytes, (buffer) => {
            this.buffers.set(file, buffer);
            this.progress.delete(file);
            // The strikes that were waiting on this file: now, or not at all.
            this.waiting = this.waiting.filter((w) => {
                if (w.file !== file) return true;
                if (ctx.currentTime <= w.at + LATE_S) {
                    this.start(w.sound, buffer, Math.max(w.at, ctx.currentTime), w.nCards, w.backwards);
                }
                return false;
            });
        }, () => this.fail(file));
    }

    /** A file that did not arrive or would not decode: its cue is silent. */
    private fail(file: string): void {
        this.progress.set(file, 'failed');
        this.waiting = this.waiting.filter((w) => w.file !== file);
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
     * `nCards` cards. `undoes` is the plan's word for a REVERT: the cue whose
     * sound it plays backwards. Returns whether a sound was scheduled, or is
     * waiting on a file that is on its way; false is every silent case in this
     * file's header and is never an error.
     */
    strike(kind: number, delayMs = 0, nCards = 1, undoes = 0): boolean {
        const ctx = this.ctx, master = this.master;
        if (!ctx || !master || this.settings().muted || ctx.state !== 'running') return false;
        if (typeof document !== 'undefined' && document.hidden) return false;
        const backwards = kind === ANIM_CUE_REVERT;
        const voiced = backwards ? undoes : kind;
        const sound = CUE_SOUNDS[voiced];
        if (!sound) return false;
        const file = this.variant(voiced, sound);
        const at = ctx.currentTime + Math.max(0, delayMs) / 1000;
        const buffer = this.buffers.get(file);
        if (buffer) {
            this.start(sound, buffer, at, nCards, backwards);
            return true;
        }
        if (this.progress.get(file) === 'failed') return false;
        // It is being played, so it is wanted, whatever the board said: to the
        // front of the queue with it, and the strike waits for the file.
        this.wanted |= 1 << voiced;
        this.pump();
        this.waiting.push({ file, sound, at, nCards, backwards });
        return true;
    }

    /** `buffer` back to front. Made once a file, on the first revert that
     *  wants it: most files are never undone. */
    private backwards(ctx: AudioContext, buffer: AudioBuffer): AudioBuffer {
        let out = this.reversed.get(buffer);
        if (!out) {
            out = ctx.createBuffer(buffer.numberOfChannels, buffer.length, buffer.sampleRate);
            for (let ch = 0; ch < buffer.numberOfChannels; ch++) {
                out.getChannelData(ch).set(buffer.getChannelData(ch).slice().reverse());
            }
            this.reversed.set(buffer, out);
        }
        return out;
    }

    private start(sound: CueSound, forwards: AudioBuffer, at: number, nCards: number, backwards: boolean): void {
        const ctx = this.ctx, master = this.master;
        if (!ctx || !master) return;
        const buffer = backwards ? this.backwards(ctx, forwards) : forwards;
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
        source.start(at);
    }

    /** Stop everything scheduled or sounding: a replay was scrubbed, the tab
     *  went away, the player hit mute. */
    cancel(): void {
        for (const source of this.live) {
            try { source.stop(); } catch { /* never started */ }
        }
        this.live.clear();
        this.waiting = [];
    }
}

/** The one engine a page has. */
export const cueAudio = new CueAudio();
