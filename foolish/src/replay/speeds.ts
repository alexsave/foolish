// speeds.ts - the replay's transport dial over RECORDED wall-clock gaps.
//
// Deliberately NOT the kernel's (c/src/anim_plan.h says what is): a step's
// duration and the gap between two steps are choreography and belong to C,
// but these numbers are a presentation of times that already happened - a
// product feature of this screen, with no counterpart on any other client.
// Split out of src/components/ReplayScreen.tsx (docs/C_GAME_SHAPE_MIGRATION.md
// Phase 9 step 4). A PURE MOVE: not a character of the markup or the rules
// changed, which is what the 16 DOM goldens and the 21 animation traces prove.

/* Playback speeds. 'AUTO' is the condensed default: recorded gaps clamped to
 * short beats. The ×N stops replay the RECORDED timing divided by N - at 1× a
 * three-day sulk between moves really takes three days (the countdown keeps
 * the screen honest), and for simulation games with nanosecond gaps the same
 * dial generates SLOW-MOTION stops (mult < 1) instead. Stops are derived from
 * the game's median gap: every power of ten that plays the median between
 * 0.2 s and 60 s, plus 1× always. */
interface SpeedStop {
    label: string;
    mult: number | null;
}

const fmtMult = (m: number): string => {
    if (m >= 1) return m >= 1e4 ? `1e${Math.round(Math.log10(m))}×` : `${m}×`;
    const exp = Math.round(Math.log10(m));
    return exp >= -2 ? `${m}×` : `1e${exp}×`;
};

const buildSpeeds = (times: (number | null)[]): SpeedStop[] => {
    const stops: SpeedStop[] = [{ label: 'AUTO', mult: null }];
    const gaps: number[] = [];
    for (let i = 1; i < times.length; i++) {
        const a = times[i - 1];
        const b = times[i];
        if (a !== null && b !== null && b > a) gaps.push(b - a);
    }
    if (gaps.length === 0) return stops; // no timing data: ⚡ only
    gaps.sort((x, y) => x - y);
    const median = gaps[Math.floor(gaps.length / 2)];

    const mults = new Set<number>([1]);
    for (let k = -12; k <= 12; k++) {
        const m = Math.pow(10, k);
        const beat = median / m;
        if (beat >= 0.2 && beat <= 60) mults.add(m);
    }
    [...mults]
        .sort((x, y) => x - y)
        .forEach((m) => stops.push({ label: fmtMult(m), mult: m }));
    return stops;
};

const fmtDuration = (ms: number): string => {
    const total = Math.max(0, Math.ceil(ms / 1000));
    const d = Math.floor(total / 86400);
    const h = Math.floor((total % 86400) / 3600);
    const m = Math.floor((total % 3600) / 60);
    const sec = total % 60;
    const hh = String(h).padStart(2, '0');
    const mm = String(m).padStart(2, '0');
    const ss = String(sec).padStart(2, '0');
    if (d > 0) return `${d}d ${hh}:${mm}:${ss}`;
    if (h > 0) return `${hh}:${mm}:${ss}`;
    return `${mm}:${ss}`;
};

export type { SpeedStop };
export { fmtMult, buildSpeeds, fmtDuration };
