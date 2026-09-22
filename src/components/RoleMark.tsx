// RoleMark - the three marks a seat can wear, their ink and their sizes:
// sword (attacking), shield (defending), check (said good).
//
// THIS IS A PORT, NOT A DESIGN. Every path point, every colour and every size
// below is copied from ios/FoolishKit/Boards/FRoleGlyphs.swift, which is the
// shipped iMessage board's answer after the owner's rounds 5, 7, 12 and 20. The
// Swift file carries the reasoning; this file carries the same drawing in SVG so
// the web board and the iMessage board are one object seen twice, and it repeats
// only as much of that reasoning as a reader needs to avoid "improving" a
// number. Where the two could drift, FRoleGlyphs.swift wins.
//
// What the website drew before: a 16px emoji per mark in the default theme
// (SovietIcon's defaultEmojis), and in the Soviet theme a flat #B32929 SVG that
// is the same red as the Soviet table it sits on. Both of those are exactly the
// failure FRoleInk was written to end.

/** THE ink every role mark is drawn in: a WHITE body with a BLACK outline.
 *
 *  FRoleGlyphs.swift: the three marks used to be three different colour schemes,
 *  "so at a glance the board carried three unrelated objects, and each one had to
 *  fight the weave on its own terms". White-on-black is the one pairing that
 *  carries on both weaves without a per-scheme branch, which is why this has no
 *  theme parameter: the walnut wool and the Soviet red are two more weaves, and
 *  the answer is the same one.
 *
 *  Owner, round 12: "Bigger sword and shield and good icons. Maybe unify them to
 *  white fill + black stroke to stand out?" */
export const RoleInk = {
    fill: '#FFFFFF',
    line: '#101014',
    /** Outline weight, in GRID units (each mark draws on the same 24x24 grid and
     *  scales with it), so the outline thickens with the mark instead of turning
     *  into a hairline at 40px and a blob at 20px. */
    stroke: 1.6,
    /** The "said good" green, the one mark that is not white. Round 12 unified
     *  all three on white and the owner pulled the check back out: "Keep it green
     *  but add a distinct stroke like the other type." */
    good: '#2E9E4F',
    /** ROUND 20, the FIRST ATTACKER's sword: "maybe make the first attacker sword
     *  have a slight dark red tint to make it a bit special." White pulled 30% of
     *  the way toward the card edge's deepRed (0x8B1A1A). It keeps `line` as its
     *  outline, so the two swords are one drawing with two fills. */
    lead: '#DCBABA',
} as const;

/** How big each mark is drawn (FRoleMark). The SWORD is the largest on purpose:
 *  it is drawn on the shared 24x24 grid and then rotated 45 degrees, so its blade
 *  spans only ~70% of the box it is given, and a sword and a shield at the same
 *  nominal size do not read the same size on screen. */
export const RoleMarkSize = {
    check: 26,
    shield: 33,
    sword: 40,
    /** A role row must be at least this tall or it clips the sword's corners. */
    rowHeight: 40,
} as const;

/** The mark a seat is wearing. `leadSword` is the same sword as `sword` in the
 *  round-20 tint, never a second shape: the seat that opens the bout wears the
 *  same object as everyone else, and a different shape would say it was a
 *  different role. */
export type RoleMarkKind = 'shield' | 'sword' | 'leadSword' | 'check';

/** The first-attacker sword: a hand-built UPRIGHT sword on a 24x24 grid, drawn
 *  as ONE closed outline rather than four filled pieces. Overlapping filled parts
 *  each carrying their own stroke would draw internal seams where the blade meets
 *  the guard, which at these sizes reads as a crack down the middle. Then rotated
 *  45 degrees to point it up-and-to-the-right. */
const SWORD_PATH = 'M12 1.2 L13.6 5.5 L13.6 14.3 L18 14.3 L18 16.6 L13.1 16.6 L13.1 19.6'
    + ' L10.9 19.6 L10.9 16.6 L6 16.6 L6 14.3 L10.4 14.3 L10.4 5.5 Z';

/** The defender shield: a heater / crusader shield. The two top edges are
 *  CONCAVE to the shield (the owner's nudge): the control points sit BELOW the
 *  straight peak-to-shoulder line, so the edge bows inward toward the centre
 *  rather than bulging out. */
const SHIELD_PATH = 'M12 1.5 Q15.5 5 21 5.5 Q21 15.5 12 22.5 Q3 15.5 3 5.5 Q8.5 5 12 1.5 Z';

/** The "said good" check, on the same 24x24 grid. A check is a STROKE, not a
 *  filled body, so "white fill + black stroke" is drawn as two passes of the same
 *  path: a fat black one, then a thinner coloured one on top. The widths differ
 *  by 2x the outline weight so the black shows as an even rim on both sides. */
const CHECK_PATH = 'M4 12.5 L9.5 18.5 L20 5';
const CHECK_BODY = 3.4;

interface GlyphProps { size?: number }

export const Sword = ({ size = 24, fill = RoleInk.fill }: GlyphProps & { fill?: string }) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" aria-hidden="true" focusable="false">
        <g transform="rotate(45 12 12)">
            <path d={SWORD_PATH} fill={fill} stroke={RoleInk.line} strokeWidth={RoleInk.stroke} strokeLinejoin="round" />
            {/* Pommel: a round knob at the base of the grip, drawn last so its own
                outline sits on top of the grip's. */}
            <circle cx="12" cy="20.6" r="2" fill={fill} stroke={RoleInk.line} strokeWidth={RoleInk.stroke} />
        </g>
    </svg>
);

export const Shield = ({ size = 24 }: GlyphProps) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" aria-hidden="true" focusable="false">
        <path d={SHIELD_PATH} fill={RoleInk.fill} stroke={RoleInk.line} strokeWidth={RoleInk.stroke} strokeLinejoin="round" />
    </svg>
);

export const Check = ({ size = 24, tint = RoleInk.good }: GlyphProps & { tint?: string }) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" aria-hidden="true" focusable="false">
        <path d={CHECK_PATH} stroke={RoleInk.line} strokeWidth={CHECK_BODY + 2 * RoleInk.stroke}
            strokeLinecap="round" strokeLinejoin="round" />
        <path d={CHECK_PATH} stroke={tint} strokeWidth={CHECK_BODY} strokeLinecap="round" strokeLinejoin="round" />
    </svg>
);

/** One role mark, drawn. The ONE place the kind-to-glyph mapping lives, so a
 *  seat's mark and any other surface that wears one cannot end up drawing three
 *  different swords.
 *
 *  `scale` is every mark at this fraction of its RoleMarkSize. One knob rather
 *  than three sizes, so the family shrinks together and a shield can never come
 *  out bigger than its sword (FSeatTag does the same at 0.6). */
export const RoleMarkView = ({ kind, scale = 1, label }: { kind: RoleMarkKind; scale?: number; label?: string }) => {
    const glyph = kind === 'shield' ? <Shield size={RoleMarkSize.shield * scale} />
        : kind === 'check' ? <Check size={RoleMarkSize.check * scale} />
            : <Sword size={RoleMarkSize.sword * scale} fill={kind === 'leadSword' ? RoleInk.lead : RoleInk.fill} />;
    // NO LABEL MEANS DECORATION, not an unnamed image: a flight ghost is the mark
    // a seat already announces, drawn a second time in the air, and a `role="img"`
    // with no accessible name would put an anonymous graphic in the reading order
    // for every hand-off.
    return label === undefined
        ? <span aria-hidden="true" style={{ display: 'inline-flex', lineHeight: 0 }}>{glyph}</span>
        : <span role="img" aria-label={label} style={{ display: 'inline-flex', lineHeight: 0 }}>{glyph}</span>;
};
