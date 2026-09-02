// RulesView.swift — the scrollable "How to play" page, opened from the Settings/
// Help squares on the board, the New-game setup and the lobby. Rewritten to the
// owner's own rules text (durak-rules-redesign): eleven sections in playing
// order — goal, setup, start, attacking, defending, covering, throwing in,
// picking up, round end, passing, game end — each with the worked examples the
// owner spelled out. Every illustration REUSES the real board furniture (FCard,
// FSword/FShield, FSeatBadge's mini hand, the wood Pickup pill), so the cards a
// player learns from are the cards they play with (never a second, drifting
// card art).
//
// Text is localized (ios.rules.*). The examples are static illustrations, not a
// live game — FCard renders them straight, no kernel involved. Hearts are the
// power suit in every example on this page; the setup section's caption says so
// once, and everything below relies on it.

import SwiftUI

public struct RulesView: View {
    /// Re-render this view when a setting changes (see FPrefs). Only the
    /// OBSERVATION matters - the strings still come from FStrings.t and the
    /// table surface still comes from FTextures.
    @ObservedObject private var prefs = FPrefs.shared
    /// Round-9 (owner): which page the rulebook opens as.
    /// - `.full`: the complete how-to-play, opened from the board.
    /// - `.lobby`: the simpler page the setup/lobby screens open - just how
    ///   the lobby works (add your name to join, then anyone can press Start,
    ///   after that no one can join) and the goal section as a quick
    ///   description of the game.
    public enum Scope { case full, lobby }

    private let onClose: () -> Void
    private let scope: Scope
    /// Does THIS game allow the transfer (perevodnoy)? A podkidnoy table's
    /// rulebook does not mention passing anywhere - not a section about it, not
    /// an "(if allowed)" aside in the defending section (owner: "simply don't
    /// mention passing if passing is not enabled"). A rule you cannot use is
    /// not a rule of your game, and half-telling it is worse than either.
    ///
    /// It is passed IN rather than read from the kernel here because this page
    /// is also opened where there is no game at all (the setup screen), and
    /// because a static illustration page should not depend on engine state.
    private let passing: Bool
    public init(scope: Scope = .full, passing: Bool = true,
                onClose: @escaping () -> Void = {}) {
        self.scope = scope
        self.passing = passing
        self.onClose = onClose
    }

    /// The power suit for every illustration on the page (declared to the
    /// reader in the setup caption).
    private static let power: Suit = .hearts

    /// Kernel values by face label (CardRank.label's inverse), so the example
    /// cards below read like cards ("9 of clubs") and not like off-by-one rank
    /// indices — value 5 is a SIX (Models.swift's own warning).
    private static func c(_ rank: String, _ suit: Suit) -> Card {
        let v: Int
        switch rank {
        case "A": v = 13
        case "K": v = 12
        case "Q": v = 11
        case "J": v = 10
        case "10": v = 9
        default: v = (Int(rank) ?? 7) - 1   // "6"->5 … "9"->8
        }
        return Card(s: suit.rawValue, v: v)
    }

    public var body: some View {
        // The wool is a `.background`, NOT a ZStack sibling: a sibling
        // `.ignoresSafeArea()` grows the stack into the safe area and the header
        // ("How to play") clips under the notch / the sheet's rounded corner
        // (owner's screenshot). As a background the wool fills the edges while the
        // content stays inside the safe area (the same fix MessagesRootView uses
        // for the board).
        VStack(spacing: 0) {
            header
            ScrollView {
                VStack(alignment: .leading, spacing: FSpace.xl) {
                    if scope == .lobby {
                        // The pre-game page: how this lobby works, then the
                        // goal section as the quick what-is-this-game.
                        section("ios.rules.lobby.h", "ios.rules.lobby.b")
                        // WHAT THE CHECKBOX MEANS. It used to explain itself
                        // with a line under the label; the owner took that out
                        // (1.0(17)) and this is where the explanation went - a
                        // lobby has its rulebook one tap away, and a control
                        // that has to be captioned is a control in the wrong
                        // place. Both games are described, because the reader
                        // is choosing between them.
                        section("ios.rules.variant.h", "ios.rules.variant.b")
                        // The fool's penalty belongs on the LOBBY page too, and
                        // not only in the full rulebook: the lobby is where a
                        // rematch is sitting when the rule is about to change
                        // who opens, so it is the one moment a player needs to
                        // be told (owner: "this should also go into the rule
                        // book for the lobby").
                        section("ios.rules.fool.h", "ios.rules.fool.b")
                        section("ios.rules.goal.h", "ios.rules.goal.b")
                    } else {
                        section("ios.rules.goal.h", "ios.rules.goal.b")
                        setupSection
                        section("ios.rules.start.h", "ios.rules.start.b")
                        // …and directly after "Game start" in the full book,
                        // because it is the exception to the rule that section
                        // just stated.
                        section("ios.rules.fool.h", "ios.rules.fool.b")
                        attackSection
                        section("ios.rules.defend.h",
                                passing ? "ios.rules.defend.b" : "ios.rules.defend.b.nopass")
                        coverSection
                        throwSection
                        pickupSection
                        section("ios.rules.round.h", "ios.rules.round.b")
                        // Podkidnoy: no passing section, and nothing in its
                        // place. The page simply teaches the game this table is
                        // playing.
                        if passing { passSection }
                        section("ios.rules.end.h", "ios.rules.end.b")
                    }
                }
                .padding(FSpace.xl)
                .padding(.bottom, FSpace.xxl)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(TableBackground().ignoresSafeArea())
    }

    private var header: some View {
        // No Done button: swiping the sheet down dismisses it (owner). Centered.
        Text(FStrings.t("ios.rules.title"))
            .font(FType.title(22)).onTableText()
            .frame(maxWidth: .infinity, alignment: .center)
            .padding(.horizontal, FSpace.xl)
            .padding(.vertical, FSpace.l)
    }

    // MARK: sections

    @ViewBuilder
    private func section(_ headKey: String, _ bodyKey: String,
                         @ViewBuilder visual: () -> some View = { EmptyView() }) -> some View {
        VStack(alignment: .leading, spacing: FSpace.s) {
            Text(FStrings.t(headKey)).font(FType.title(18)).onTableText()
            Text(FStrings.t(bodyKey)).font(FType.body(15)).onTableText()
                .fixedSize(horizontal: false, vertical: true)
            visual()
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    /// Game setup: the draw pile with the flipped power card tucked under it —
    /// FDeckWell's own stock+flip geometry (46×66 cards, the flip peeking out
    /// below the landscape stack), minus its live frame publishing.
    private var setupSection: some View {
        section("ios.rules.setup.h", "ios.rules.setup.b") {
            HStack(alignment: .top, spacing: FSpace.l) {
                ZStack(alignment: .topLeading) {
                    FCard(card: Self.c("6", Self.power), trump: true,
                          size: CGSize(width: 46, height: 66))
                        .offset(x: 18, y: 28)
                    ForEach(0..<3, id: \.self) { i in
                        FCard(card: nil, backSeed: UInt64(42 + i),
                              size: CGSize(width: 46, height: 66))
                            .rotationEffect(.degrees(90))
                            // The same rotation nudge FDeckWell documents: the
                            // bottom card's rotated top-left lands on (8, 8).
                            .offset(x: 18 - CGFloat(i), y: -2 - CGFloat(i * 2))
                    }
                }
                .frame(width: 80, height: 96, alignment: .topLeading)
                caption("ios.rules.setup.cap")
            }
            .padding(.top, FSpace.xs)
        }
    }

    /// Attacking: the role-mark legend, the owner's 6-card hand, and the three
    /// verdicts on it (the Queen alone / both Kings / never the 7 with the 10).
    private var attackSection: some View {
        section("ios.rules.attack.h", "ios.rules.attack.b") {
            // The legend draws the marks at the size the BOARD draws them
            // (FRoleMark), so what the rules teach is what the player then
            // looks for - a shrunken legend copy is a different icon.
            legend("ios.rules.sword") { FSword(size: FRoleMark.sword) }
            legend("ios.rules.shield") { FShield(size: FRoleMark.shield) }
            cardRow([Self.c("6", .clubs), Self.c("7", .diamonds), Self.c("10", .spades),
                     Self.c("Q", .diamonds), Self.c("K", .spades), Self.c("K", .clubs)])
            example(ok: true, "ios.rules.attack.ok1")
            example(ok: true, "ios.rules.attack.ok2")
            example(ok: false, "ios.rules.attack.no1")
        }
    }

    /// Covering: the four battles the owner asked for — higher same suit, power
    /// over a higher value, a too-low power on a power attack, and the classic
    /// higher-but-wrong-suit mistake.
    private var coverSection: some View {
        section("ios.rules.cover.h", "ios.rules.cover.b") {
            coverExample(ok: true, attack: Self.c("7", .clubs), cover: Self.c("9", .clubs),
                         key: "ios.rules.cover.ok1")
            coverExample(ok: true, attack: Self.c("9", .clubs), cover: Self.c("6", Self.power),
                         key: "ios.rules.cover.ok2")
            coverExample(ok: false, attack: Self.c("9", Self.power), cover: Self.c("7", Self.power),
                         key: "ios.rules.cover.no1")
            coverExample(ok: false, attack: Self.c("7", .clubs), cover: Self.c("9", .spades),
                         key: "ios.rules.cover.no2")
        }
    }

    /// Throwing in, kept 2-player (owner): one covered battle on the table (a 6
    /// and a 9 in play), your hand beside it, and the defender's unseen hand as
    /// the same mini card fan + count the board shows (the "little card view").
    private var throwSection: some View {
        let table: [(Card, Card?)] = [(Self.c("6", .spades), Self.c("9", .spades))]
        return section("ios.rules.throw.h", "ios.rules.throw.b") {
            tableExample(ok: true, battles: table, hand: [Self.c("9", .diamonds)],
                         key: "ios.rules.throw.ok1")
            tableExample(ok: true, battles: table,
                         hand: [Self.c("6", Self.power), Self.c("6", .diamonds)],
                         key: "ios.rules.throw.ok2")
            tableExample(ok: true, battles: table,
                         hand: [Self.c("6", Self.power), Self.c("9", .diamonds)],
                         key: "ios.rules.throw.ok3")
            tableExample(ok: false, battles: table,
                         hand: [Self.c("8", .diamonds), Self.c("J", .clubs)],
                         key: "ios.rules.throw.no1")
            tableExample(ok: false,
                         battles: [(Self.c("6", .spades), Self.c("9", .spades)),
                                   (Self.c("9", .clubs), nil)],
                         hand: [Self.c("6", .diamonds)],
                         badgeKey: "ios.rules.defender", badgeCount: 1, badgeShield: true,
                         key: "ios.rules.throw.no2")
        }
    }

    /// Picking up: the half-covered table beside the real wood Pickup pill (an
    /// illustration, not a control), and the note that ALL of it goes.
    private var pickupSection: some View {
        section("ios.rules.pickup.h", "ios.rules.pickup.b") {
            HStack(alignment: .center, spacing: FSpace.l) {
                battle(Self.c("6", .spades), Self.c("9", .spades))
                battle(Self.c("6", .diamonds), nil)
                FButton(FStrings.t("pickup"), kind: .wood, compact: true) {}
                    .allowsHitTesting(false)
                Spacer(minLength: 0)
            }
            .padding(.top, FSpace.xs)
            caption("ios.rules.pickup.cap")
        }
    }

    /// Passing: possible; blocked by a cover; blocked by no matching value;
    /// blocked by the next player's hand being too small for the grown attack.
    private var passSection: some View {
        section("ios.rules.pass.h", "ios.rules.pass.b") {
            tableExample(ok: true, battles: [(Self.c("8", .spades), nil)],
                         hand: [Self.c("8", .diamonds)],
                         key: "ios.rules.pass.ok1")
            tableExample(ok: false, battles: [(Self.c("8", .spades), Self.c("10", .spades))],
                         hand: [Self.c("8", .diamonds)],
                         key: "ios.rules.pass.no1")
            tableExample(ok: false, battles: [(Self.c("8", .spades), nil)],
                         hand: [Self.c("9", .diamonds), Self.c("Q", .clubs)],
                         key: "ios.rules.pass.no2")
            tableExample(ok: false,
                         battles: [(Self.c("8", .spades), nil), (Self.c("8", .clubs), nil)],
                         hand: [Self.c("8", .diamonds)],
                         badgeKey: "ios.rules.nextplayer", badgeCount: 2,
                         key: "ios.rules.pass.no3")
        }
    }

    // MARK: illustration helpers (reusing the real components)

    private static let exSize = CGSize(width: 46, height: 64)

    private func caption(_ key: String) -> some View {
        Text(FStrings.t(key)).font(FType.body(14)).onTableText()
            .fixedSize(horizontal: false, vertical: true)
    }

    /// A role-mark legend line: the board's own glyph beside what it means.
    private func legend(_ key: String, @ViewBuilder icon: () -> some View) -> some View {
        HStack(alignment: .center, spacing: FSpace.s) {
            icon().frame(width: 32)
            caption(key)
        }
    }

    /// A ✓/✗ verdict beside its caption alone (the attack section's hand is
    /// shown once above all three verdicts).
    private func example(ok: Bool, _ key: String) -> some View {
        HStack(alignment: .top, spacing: FSpace.s) {
            verdict(ok)
            caption(key)
        }
        .padding(.top, FSpace.xs)
    }

    /// The yes/no marks beside each example: the same green check the board
    /// uses, against a red cross. Both wear the black rim the role marks do, so
    /// the two families look drawn by the same hand.
    @ViewBuilder private func verdict(_ ok: Bool) -> some View {
        if ok { FCheck(size: 22) } else { FCross(size: 22) }
    }

    private func cardRow(_ cards: [Card]) -> some View {
        HStack(spacing: FSpace.s) {
            ForEach(cards, id: \.identity) { c in
                FCard(card: c, size: Self.exSize)
            }
        }
        .padding(.top, FSpace.xs)
    }

    /// An attack card with its cover laid over it, EXACTLY the way FBattleGrid
    /// draws a covered battle (owner, durak-rules-redesign follow-up: the rules
    /// page used to keep the attack upright under a 6°-offset cover, while the
    /// real board tilts BOTH cards, in opposite directions): both pivot about
    /// their bottom-centre, the attack -coverAngle and the cover +coverAngle —
    /// FBattleGrid's own constant, so the page can never disagree with the
    /// board it teaches. The slot keeps FBattleGrid's headroom proportions
    /// (card 50×70 in 62×84 → +12/+14 here) and its bottom alignment, so an
    /// uncovered attack stands upright on the same bottom line, board-style.
    private func battle(_ attack: Card, _ cover: Card?) -> some View {
        ZStack(alignment: .bottom) {
            FCard(card: attack, size: Self.exSize)
                .rotationEffect(.degrees(cover == nil ? 0 : -FBattleGrid.coverAngle),
                                anchor: .bottom)
            if let cover {
                FCard(card: cover, size: Self.exSize)
                    .rotationEffect(.degrees(FBattleGrid.coverAngle), anchor: .bottom)
            }
        }
        .frame(width: Self.exSize.width + 12,
               height: Self.exSize.height + 14, alignment: .bottom)
    }

    /// A covering verdict: ✓/✗, the battle, the reason.
    private func coverExample(ok: Bool, attack: Card, cover: Card, key: String) -> some View {
        HStack(alignment: .top, spacing: FSpace.m) {
            verdict(ok).padding(.top, FSpace.m)
            battle(attack, cover)
            caption(key)
            Spacer(minLength: 0)
        }
        .padding(.top, FSpace.xs)
    }

    /// A labelled group of cards ("on the table" / "your hand") for the
    /// throwing-in and passing scenes.
    private func group(_ labelKey: String, @ViewBuilder cards: () -> some View) -> some View {
        VStack(alignment: .leading, spacing: FSpace.xs) {
            Text(FStrings.t(labelKey)).font(FType.body(11)).onTableText(dimmed: true)
            HStack(alignment: .top, spacing: FSpace.s) { cards() }
        }
    }

    /// One 2-player table scene: verdict, the table's battles, your hand, and —
    /// when a hand size matters (throw-in capacity, pass capacity) — the other
    /// player's unseen hand as the board's own mini fan + count (FSeatBadge).
    private func tableExample(ok: Bool, battles: [(Card, Card?)], hand: [Card],
                              badgeKey: String? = nil, badgeCount: Int = 0,
                              badgeShield: Bool = false, key: String) -> some View {
        VStack(alignment: .leading, spacing: FSpace.xs) {
            HStack(alignment: .top, spacing: FSpace.l) {
                verdict(ok).padding(.top, FSpace.l)
                group("ios.rules.lbl.table") {
                    ForEach(Array(battles.enumerated()), id: \.offset) { _, b in
                        battle(b.0, b.1)
                    }
                }
                group("ios.rules.lbl.hand") {
                    ForEach(hand, id: \.identity) { c in
                        FCard(card: c, size: Self.exSize)
                    }
                }
                if let badgeKey {
                    FSeatBadge(name: FStrings.t(badgeKey), handCount: badgeCount,
                               isDefender: badgeShield)
                }
                Spacer(minLength: 0)
            }
            caption(key)
        }
        .padding(.top, FSpace.xs)
    }
}

/// The "not allowed" mark for the rules examples — a hand-built red cross on the
/// same 24×24 grid and stroke style as FCheck, so the two verdicts read as one
/// family (and for FCheck's own reason: SF Symbols are unreliable under
/// ImageRenderer snapshots, so board glyphs are hand-built throughout).
struct FCross: View {
    var size: CGFloat = 24
    var body: some View {
        Canvas { ctx, sz in
            let s = sz.width / 24
            func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x * s, y: y * s) }
            let red = Color(hex: 0xC0392B)
            var cross = Path()
            cross.move(to: P(5.5, 5.5))
            cross.addLine(to: P(18.5, 18.5))
            cross.move(to: P(18.5, 5.5))
            cross.addLine(to: P(5.5, 18.5))
            // Outline first, body second - the same two-pass rim FCheck uses.
            ctx.stroke(cross, with: .color(FRoleInk.line),
                       style: StrokeStyle(lineWidth: (3.4 + 2 * FRoleInk.stroke) * s, lineCap: .round))
            ctx.stroke(cross, with: .color(red),
                       style: StrokeStyle(lineWidth: 3.4 * s, lineCap: .round))
        }
        .frame(width: size, height: size)
        .accessibilityLabel(Text(FStrings.t("ios.reject")))
    }
}
