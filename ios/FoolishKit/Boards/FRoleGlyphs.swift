// FRoleGlyphs — the three marks a seat can wear, their ink and their sizes:
// sword (attacking), shield (defending), check (said good).
//
// Design-system shapes, drawn from paths and nothing else. They are worn by
// FSeatBadge, by FRoleCoin, by the board's own self-mark and by
// ComponentSnapshotTests, which is the one place their family resemblance is
// checked - and none of that has anything to do with MessageTableView, which is
// merely where they were written.

import SwiftUI


/// THE ink every role mark is drawn in: a WHITE body with a BLACK outline.
///
/// The three marks used to be three different colour schemes - a near-black
/// sword that flipped to steel in dark mode, a mid-gray shield with a darker
/// edge, a saturated green check - so at a glance the board carried three
/// unrelated objects, and each one had to fight the weave on its own terms
/// (the sword's near-black vanished on the walnut wool, which is why it had a
/// dark-mode special case at all). White-on-black is the one pairing that
/// carries on BOTH weaves without a per-scheme branch: the white body is the
/// silhouette, the black outline is what separates it from a light table.
///
/// Owner, this round: "Bigger sword and shield and good icons. Maybe unify
/// them to white fill + black stroke to stand out?"
enum FRoleInk {
    static let fill = Color.white
    static let line = Color(hex: 0x101014)
    /// Outline weight, in GRID units (each mark draws on the same 24x24 grid and
    /// scales by `size / 24`), so the outline thickens with the mark instead of
    /// turning into a hairline at 40pt and a blob at 20pt.
    static let stroke: CGFloat = 1.6
    /// The "said good" green. Lives here beside the shared ink so the one mark
    /// that is NOT white is still declared in the same place as the rest.
    static let good = Color(hex: 0x2E9E4F)
    /// ROUND 20, the FIRST ATTACKER's sword: "maybe make the first attacker
    /// sword have a slight dark red tint to make it a bit special." White pulled
    /// 30% of the way toward the card edge's `deepRed` (0x8B1A1A) - far enough
    /// that the seat opening the bout is obviously not wearing the same sword as
    /// the throw-in attackers, and not so far that it stops reading as a light
    /// glyph against the wool. It keeps `line` as its outline, so the two swords
    /// are one drawing with two fills.
    static let lead = Color(hex: 0xDCBABA)
}

/// How big each role mark is drawn, everywhere it is drawn (the board's own
/// `selfRoleIndicator` and every opponent's `FSeatBadge.roleRow`).
///
/// Owner, this round: "Bigger sword and shield and good icons" - each up ~25%
/// on the round-5/7 numbers (check 20/22 -> 26, shield 26 -> 33, sword 32 -> 40).
/// The SWORD stays the largest of the three on purpose: it draws on the shared
/// 24x24 grid and is then rotated 45 degrees, so its blade spans only ~70% of
/// the box it is given, and a sword and a shield at the same nominal size do
/// not read the same size on screen.
enum FRoleMark {
    static let check: CGFloat = 26
    static let shield: CGFloat = 33
    static let sword: CGFloat = 40
    /// A role row must be at least this tall or it clips the sword's corners.
    static let rowHeight: CGFloat = sword
}

/// The first-attacker sword — a hand-built UPRIGHT sword on a 24x24 grid that
/// actually reads as a sword: a pointed blade, a wide crossguard, a grip, and a
/// round pommel. Marks "you open this bout".
///
/// Drawn as ONE closed outline rather than four filled pieces: overlapping
/// filled parts each carrying their own stroke would draw internal seams where
/// the blade meets the guard, which at these sizes reads as a crack down the
/// middle of the sword.
struct FSword: View {
    var size: CGFloat = 24
    /// Round 20: the first attacker's sword is this same drawing in a tint
    /// (`FRoleInk.lead`). A parameter rather than a second view, so the blade
    /// geometry can never drift between the two.
    var fill: Color = FRoleInk.fill
    var body: some View {
        Canvas { ctx, sz in
            let s = sz.width / 24
            func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x * s, y: y * s) }
            var sword = Path()
            sword.move(to: P(12, 1.2))            // tip
            sword.addLine(to: P(13.6, 5.5))       // right edge of the blade
            sword.addLine(to: P(13.6, 14.3))
            sword.addLine(to: P(18, 14.3))        // right arm of the crossguard
            sword.addLine(to: P(18, 16.6))
            sword.addLine(to: P(13.1, 16.6))
            sword.addLine(to: P(13.1, 19.6))      // grip, right side
            sword.addLine(to: P(10.9, 19.6))
            sword.addLine(to: P(10.9, 16.6))      // grip, left side
            sword.addLine(to: P(6, 16.6))         // left arm of the crossguard
            sword.addLine(to: P(6, 14.3))
            sword.addLine(to: P(10.4, 14.3))
            sword.addLine(to: P(10.4, 5.5))       // left edge of the blade
            sword.closeSubpath()
            ctx.fill(sword, with: .color(fill))
            ctx.stroke(sword, with: .color(FRoleInk.line),
                       style: StrokeStyle(lineWidth: FRoleInk.stroke * s, lineJoin: .round))
            // Pommel: a round knob at the base of the grip, drawn last so its
            // own outline sits on top of the grip's.
            let r = 2.0 * s
            let knob = Path(ellipseIn: CGRect(x: 12 * s - r, y: 20.6 * s - r,
                                              width: 2 * r, height: 2 * r))
            ctx.fill(knob, with: .color(fill))
            ctx.stroke(knob, with: .color(FRoleInk.line),
                       style: StrokeStyle(lineWidth: FRoleInk.stroke * s))
        }
        .frame(width: size, height: size)
        .rotationEffect(.degrees(45))   // point it up-and-to-the-right
        // Round-5 m2: this was a hard-coded English literal while every visible
        // string on the board goes through FStrings — a ru/ko VoiceOver user
        // got an English board here even though the label was otherwise well
        // chosen. The key already exists (en/ru/ko all carry it).
        .accessibilityLabel(Text(FStrings.t("ios.a11y.attackfirst")))
    }
}

/// The defender shield — a hand-built heraldic shield in the shared role ink.
///
/// Round-5 m4 asked for pointed upper corners; round-7 settled the silhouette on
/// the heater / crusader shield below (a raised point at the top centre, rounded
/// shoulders as the widest span, curving to a point at the bottom). This round
/// only changes what it is PAINTED in: the old mid-gray-on-darker-gray became
/// white-on-black with the sword and the check (FRoleInk).
struct FShield: View {
    var size: CGFloat = 24
    var body: some View {
        Canvas { ctx, sz in
            let s = sz.width / 24
            func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x * s, y: y * s) }
            var shield = Path()
            shield.move(to: P(12, 1.5))                                   // top-centre peak
            shield.addQuadCurve(to: P(21, 5.5),  control: P(15.5, 5))     // peak -> right shoulder
            shield.addQuadCurve(to: P(12, 22.5), control: P(21, 15.5))    // right side -> bottom point
            shield.addQuadCurve(to: P(3, 5.5),   control: P(3, 15.5))     // bottom point -> left shoulder
            shield.addQuadCurve(to: P(12, 1.5),  control: P(8.5, 5))      // left shoulder -> peak
            shield.closeSubpath()
            // The two top edges are CONCAVE to the shield (owner's nudge): the
            // control points sit BELOW the straight peak->shoulder line (y=5 vs
            // the line's ~3.5 midpoint), so the edge bows inward/down toward the
            // centre rather than bulging out - the crusader-shield sweep up to
            // the point, not a balloon.
            ctx.fill(shield, with: .color(FRoleInk.fill))
            ctx.stroke(shield, with: .color(FRoleInk.line),
                       style: StrokeStyle(lineWidth: FRoleInk.stroke * s, lineJoin: .round))
        }
        .frame(width: size, height: size)
        // Round-5 m2: was a hard-coded English literal (see FSword's).
        .accessibilityLabel(Text(FStrings.t("ios.a11y.defending")))
    }
}

/// The "said good" mark — a hand-built check on the same 24x24 grid as
/// FSword/FShield. Hand-built for the same reason: SF Symbols (previously
/// `checkmark.seal.fill`) are unreliable under ImageRenderer bubble snapshots.
///
/// A check is a STROKE, not a filled body, so "white fill + black stroke" is
/// drawn here as two passes of the same path: a fat black one, then a thinner
/// white one on top. That is the same silhouette-plus-outline the other two
/// marks get, achieved the only way an open path can.
struct FCheck: View {
    var size: CGFloat = 24
    /// The check's body colour, GREEN by default.
    ///
    /// Deliberately NOT the shared white the sword and shield wear. A round-12
    /// pass unified all three on white and the owner pulled the check back out:
    /// "Keep it green but add a distinct stroke like the other type." Which is
    /// right - the sword and the shield say WHICH ROLE YOU HAVE and want to read
    /// as one family, while a check says something happened, and green is what
    /// carries that at a glance. What it takes from the other two is the BLACK
    /// RIM, so it still looks drawn by the same hand.
    var tint: Color = FRoleInk.good
    var body: some View {
        Canvas { ctx, sz in
            let s = sz.width / 24
            func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x * s, y: y * s) }
            var check = Path()
            check.move(to: P(4, 12.5))
            check.addLine(to: P(9.5, 18.5))
            check.addLine(to: P(20, 5))
            // Outline first, body second. The widths differ by 2x the outline
            // weight so the black shows as an even rim on both sides of the
            // white, exactly like the closed marks' 1.6-unit stroke.
            ctx.stroke(check, with: .color(FRoleInk.line),
                       style: StrokeStyle(lineWidth: (3.4 + 2 * FRoleInk.stroke) * s,
                                          lineCap: .round, lineJoin: .round))
            ctx.stroke(check, with: .color(tint),
                       style: StrokeStyle(lineWidth: 3.4 * s, lineCap: .round, lineJoin: .round))
        }
        .frame(width: size, height: size)
        // Round-5 m2: was a hard-coded English literal (see FSword's). "good"
        // (no `ios.` prefix) is the same key the action bar's Good button uses.
        .accessibilityLabel(Text(FStrings.t("good")))
    }
}
