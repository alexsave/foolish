import { useEffect, useState, useSyncExternalStore } from 'react';
import Link from 'next/link';
import { animBuildPlan, ANIM_EVT, ANIM_LOC, type AnimPlanEventIn } from '@sdk/ts/wasm/bots.ts';
import { ANIM_CUE_NAMES, ANIM_N_CUES, ANIM_SOUND_STAGGER_MS } from '@sdk/ts/gen/kernel_consts.bots.ts';
import { cueAudio } from '../audio/cueAudio';
import { CUE_SOUNDS } from '../audio/cueSounds';
import { TexturedSurface } from './TexturedSurface';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';

// /sounds - hear every sound cue on its own, and a few moves in context.
//
// A WORKBENCH, not a screen of the game, and English only for that reason. It
// plays through the same engine and the same table the board does
// (src/audio/cueAudio.ts, src/audio/cueSounds.ts), so what is heard here is
// what ships: swap a file in the table, reload, and this page is the new sound.
//
// The "in context" rows do not play a recording of a move. They hand the
// kernel a small stream and schedule the cues its PLAN comes back with
// (c/src/anim_plan.h AnimPlan.cues), which is the same answer the board's
// frame loop schedules from - so the gaps between the sounds, the spread of a
// three-card attack and the rest before a sweep are the real ones.

const card = (suit: number, value: number) => ({ suit, value });
const lay = (type: number, seat: number, n: number): AnimPlanEventIn => ({
    type, seat, from: ANIM_LOC.hand, to: ANIM_LOC.table,
    cards: Array.from({ length: n }, (_, i) => card((seat + i) % 4, 6 + i)),
});
const draw = (type: number, seat: number, n: number): AnimPlanEventIn => ({
    type, seat, from: ANIM_LOC.deck, to: ANIM_LOC.hand,
    cards: Array.from({ length: n }, (_, i) => card(i % 4, 2 + i)),
});
const sweep = (type: number, n: number, to: number, seat?: number): AnimPlanEventIn => ({
    type, seat, from: ANIM_LOC.table, to,
    cards: Array.from({ length: n }, (_, i) => card(i % 4, 6 + i)),
});

const MOVES: readonly { name: string; says: string; events: () => AnimPlanEventIn[] }[] = [
    { name: 'An attack', says: 'one card: lift, then place', events: () => [lay(ANIM_EVT.attack_pass, 0, 1)] },
    { name: 'A three-card attack', says: 'one lift, three strikes spread from the landing',
      events: () => [lay(ANIM_EVT.attack_pass, 0, 3)] },
    { name: 'Attack, then cover', says: 'two beats, a flight and a gap apart',
      events: () => [lay(ANIM_EVT.attack_pass, 0, 1), lay(ANIM_EVT.cover, 1, 1)] },
    { name: 'A bout ends', says: 'a two-card cover, the rest, the sweep to the pile, both refills',
      events: () => [lay(ANIM_EVT.cover, 1, 1), lay(ANIM_EVT.cover, 1, 1),
                     sweep(ANIM_EVT.discard, 4, ANIM_LOC.discard),
                     draw(ANIM_EVT.refill, 0, 2), draw(ANIM_EVT.refill, 1, 2)] },
    { name: 'A pickup', says: 'the table taken into a hand, then the attacker refills',
      events: () => [sweep(ANIM_EVT.pickup, 5, ANIM_LOC.hand, 1), draw(ANIM_EVT.refill, 0, 3)] },
    { name: 'A last card', says: 'the cover that empties a hand, and the seat going out with it',
      events: () => [lay(ANIM_EVT.cover, 1, 1), { type: ANIM_EVT.out, seat: 1 },
                     sweep(ANIM_EVT.discard, 2, ANIM_LOC.discard)] },
    { name: 'The opening deal', says: 'twelve cards round the table, then the trump turned up',
      events: () => [...Array.from({ length: 12 }, (_, i) => draw(ANIM_EVT.deal, i % 2, 1)),
                     { type: ANIM_EVT.flipped, from: ANIM_LOC.deck, to: ANIM_LOC.flipped, cards: [card(2, 12)] }] },
];

/** Run `strike` now, or as soon as the files it needs have arrived: the first
 *  tap on this page is also the gesture that lets them be fetched. */
const whenLoaded = (strike: () => boolean, tries = 20): void => {
    cueAudio.unlock();
    if (strike() || tries <= 0) return;
    setTimeout(() => whenLoaded(strike, tries - 1), 150);
};

const playMove = (events: AnimPlanEventIn[]): void => {
    const plan = animBuildPlan(events, undefined);
    whenLoaded(() => {
        let any = false;
        for (const cue of plan.cues) {
            for (let h = 0; h < cue.hits; h++) {
                any = cueAudio.strike(cue.kind, cue.atMs + h * ANIM_SOUND_STAGGER_MS, cue.nCards) || any;
            }
        }
        return any;
    });
};

const Button = ({ onClick, children }: { onClick: () => void; children: React.ReactNode }) => (
    <TexturedSurface as="button" seed={0.4} className="btn-wood btn-wood--md" onClick={onClick}
                     style={{ minWidth: 120 }}>
        <span className="btn-wood-text">{children}</span>
    </TexturedSurface>
);

const cell: React.CSSProperties = { padding: '6px 10px', textAlign: 'left', verticalAlign: 'middle' };

export const SoundAudition = () => {
    const { muted, volume } = useSyncExternalStore(cueAudio.subscribe, cueAudio.getState, cueAudio.getServerState);
    // The decoded state of each file is the engine's and has no event of its
    // own; a workbench can afford to look twice a second.
    const [, tick] = useState(0);
    useEffect(() => {
        cueAudio.arm();
        const id = setInterval(() => tick((n) => n + 1), 500);
        return () => clearInterval(id);
    }, []);

    const cues = Array.from({ length: ANIM_N_CUES - 1 }, (_, i) => i + 1);
    return (
        <div className="page page--with-padding" style={{ width: '100vw', overflowY: 'auto' }}>
            <WoolBackgroundLayer />
            <div className="content content--max-width content--gap-lg z-content"
                 style={{ color: 'var(--color-text-light, #f5e6c8)', paddingBottom: 60 }}>
                <h1 className="title title--page">Sounds</h1>
                <p>
                    Every sound the game can make, through the same engine and the same table the
                    board uses. The files marked <code>gen/</code> are synthesised placeholders.
                </p>

                <div style={{ display: 'flex', gap: 16, alignItems: 'center', flexWrap: 'wrap' }}>
                    <Button onClick={() => cueAudio.setMuted(!muted)}>{muted ? 'Sound is off' : 'Sound is on'}</Button>
                    <label style={{ display: 'flex', gap: 10, alignItems: 'center' }}>
                        Master volume
                        <input type="range" min={0} max={1} step={0.05} value={volume} data-sound-volume
                               onChange={(e) => cueAudio.setVolume(Number(e.target.value))} />
                        <span style={{ width: 40 }}>{Math.round(volume * 100)}%</span>
                    </label>
                </div>

                <h2 className="title">Each cue</h2>
                <table style={{ borderCollapse: 'collapse', width: '100%' }}>
                    <thead>
                        <tr>
                            <th style={cell}>Cue</th><th style={cell}>Files</th><th style={cell}>Gain</th>
                            <th style={cell} />
                        </tr>
                    </thead>
                    <tbody>
                        {cues.map((kind) => {
                            const sound = CUE_SOUNDS[kind];
                            const name = (ANIM_CUE_NAMES[kind] ?? String(kind)).toLowerCase();
                            return (
                                <tr key={kind} data-cue={name}>
                                    <td style={cell}><strong>{name}</strong> <span style={{ opacity: 0.6 }}>({kind})</span></td>
                                    <td style={cell}>
                                        {sound ? sound.files.map((f) => (
                                            <div key={f} data-file={f} data-loaded={cueAudio.loaded(f)}>
                                                <code>{f}</code>{cueAudio.loaded(f) ? '' : ' (not loaded)'}
                                            </div>
                                        )) : 'silent'}
                                    </td>
                                    <td style={cell}>{sound ? sound.gain : ''}</td>
                                    <td style={{ ...cell, display: 'flex', gap: 8 }}>
                                        {sound && <>
                                            <Button onClick={() => whenLoaded(() => cueAudio.strike(kind))}>Play</Button>
                                            <Button onClick={() => whenLoaded(() => {
                                                let any = false;
                                                for (let h = 0; h < 4; h++) {
                                                    any = cueAudio.strike(kind, h * ANIM_SOUND_STAGGER_MS) || any;
                                                }
                                                return any;
                                            })}>x4</Button>
                                        </>}
                                    </td>
                                </tr>
                            );
                        })}
                    </tbody>
                </table>

                <h2 className="title">In context</h2>
                <p>Each of these asks the kernel for the plan of a small move and plays its cues at the plan's own times.</p>
                <table style={{ borderCollapse: 'collapse', width: '100%' }}>
                    <tbody>
                        {MOVES.map((m) => (
                            <tr key={m.name}>
                                <td style={cell}><strong>{m.name}</strong><div style={{ opacity: 0.7 }}>{m.says}</div></td>
                                <td style={cell}><Button onClick={() => playMove(m.events())}>Play</Button></td>
                            </tr>
                        ))}
                    </tbody>
                </table>

                <TexturedSurface as={Link} seed={0.5} href="/" className="btn-wood btn-wood--md"
                                 style={{ textDecoration: 'none', display: 'inline-block' }}>
                    <span className="btn-wood-text">Home</span>
                </TexturedSurface>
            </div>
        </div>
    );
};
