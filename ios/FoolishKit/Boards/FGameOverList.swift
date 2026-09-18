// FGameOverList - the ranked results screen, first-out down to the fool, with
// New game and the replay link under it (web WinScreen parity).
//
// The board gives way to this when the game ends, and that swap is the only
// thing it and MessageTableView have to say to each other: it takes finished
// rows and three closures, never a controller. Out of that file it is a screen
// that can be built from a literal array of FinishRows and looked at.

import SwiftUI
import UIKit   // UIPasteboard, for the replay link's refused-to-open fallback

/// One row of the ranked end screen (web WinScreen parity, minus ELO). `place` is
/// 1-based: rank 1 is the first player out (best); the fool is `place == total`.
struct FinishRow: Identifiable {
    let place: Int
    let total: Int
    let name: String
    let isYou: Bool
    var id: Int { place }
    var isFool: Bool { place == total }
}

/// The game-over results, replacing the board when the fool is decided (design:
/// mirror the web WinScreen). Players are listed in finishing order - first out
/// (rank 1) down to the fool - with New game at the bottom so any player, out or
/// not, can deal the next one. No ELO in the iMessage game, and no emoji: the rank
/// is a colored number (brass for 1st, red for the fool) with a "Fool" tag.
struct FGameOverList: View {
    /// Re-render on a settings change (see FPrefs), the same reason
    /// `SendHintReminder` grew one: "Fool", "You", "New game" and the game-over
    /// heading are all resolved by `FStrings.t` at body-eval time, and nothing
    /// else about this view changes when the language does. The end screen is
    /// also the one place a player is likely to sit and read, so a caption
    /// stuck in the previous language has all the time in the world to be seen.
    @ObservedObject private var prefs = FPrefs.shared
    private static let rowH: CGFloat = 34   // fixed per-row height (plank scales with player count)
    let rows: [FinishRow]
    let onNewGame: () -> Void
    /// Is the way out offered yet? False while THIS player's own game-ending
    /// move is staged and unsent (MessageTableView) - starting a rematch would
    /// replace that bubble in the input field with a lobby, and the result the
    /// ranks describe would never reach anyone else. Everyone who RECEIVED the
    /// ending move, and the sender once it has gone, gets the button.
    var showNewGame: Bool = true
    /// The finished game on the website (`foolish.cards/<code>`), or nil when
    /// the kernel could not encode one - in which case no link is offered at
    /// all, rather than one that lands on a broken page.
    var replayURL: URL? = nil
    /// How to leave the extension with it. An iMessage extension has no
    /// `UIApplication`, so the host hands its `extensionContext.open` down.
    ///
    /// ROUND 20 made it ANSWER (owner: "tapping reply code on end screen doesn't
    /// do anything at all. NOTHING"). `NSExtensionContext.open` is documented as
    /// available to iMessage apps, but what it will actually open is the
    /// CONTAINING APP's own URL scheme - opening an arbitrary https link from an
    /// extension has been refused by the system since iOS 10 beta 5, and was
    /// tightened again in iOS 13 as a side effect of the keyboard-extension
    /// crackdown. The documented workaround is "hand the URL to your parent app
    /// and let IT open Safari", which this product cannot use: the iMessage app
    /// ships as its own App Store record with a CODELESS container (see the §9.1
    /// reversal), so there is no parent app to hand anything to.
    ///
    /// So the call is still made - it costs nothing, and on any OS where it
    /// works the tap does exactly what the arrow promises - but its answer is
    /// now believed, and a refusal falls back to putting the link on the
    /// pasteboard where the player can use it. The one thing a tap may not do
    /// is nothing.
    var onOpenURL: (URL) async -> Bool = { _ in false }
    /// Set when the system refused and the link went to the pasteboard instead,
    /// so the row can say so where the player is already looking.
    @State private var linkCopied = false

    private var plankHeight: CGFloat { CGFloat(rows.count) * Self.rowH }

    var body: some View {
        // Title + ranking sit at the TOP; New game is pinned to the bottom.
        VStack(spacing: 14) {
            // Round-6 #17: this title sits on the plain wool (it is ABOVE the
            // wood plank, not on it), so it takes the wool half of the
            // text-on-a-surface pairing - thick black ink, not the bone/dark-
            // shadow combo it used to carry (that combo was tuned for wood).
            Text(FStrings.t("game_over"))
                .font(.title2)
                .onTableText()
            // THE RANKING SCROLLS; the title and New game do not.
            //
            // The plank is 34pt per player, so eight of them is 272pt before
            // the title, the link and the button are counted - more than the
            // expanded surface has, and far more than the compact drawer's. It
            // already did not fit (rendered at 420pt, an 8-player result had
            // its title clipped off the top AND its New game button pushed off
            // the bottom; at a compact 230pt even a 3-player one lost the
            // button), which is a results screen you cannot leave. Round-16's
            // Replay Link makes the same overflow worse, so it is fixed here
            // rather than added to.
            //
            // A ScrollView with the button OUTSIDE it means the two things a
            // player must always be able to reach - who the fool was, and the
            // way out - are both reachable at every count and every height.
            // When it all fits this is invisible: the content still hugs the
            // top and the button still sits on the bottom edge, exactly as the
            // Spacer used to leave them.
            //
            // Indicators are left ON deliberately. The plank is one block with
            // its own border, so a clipped ranking looks like a COMPLETE one -
            // at eight players the visible last row would read as the fool when
            // the real fool is below the fold. The indicator is what says
            // otherwise. The link travels with the ranking rather than being
            // pinned beside New game because it is about the game just played,
            // and pinned it floated alone in the middle of the wool.
            ScrollView(.vertical) {
                VStack(spacing: 14) {
                    ranking
                    if let replayURL { replayLink(replayURL) }
                }
            }
            .modifier(BounceOnlyWhenTooTall())
            if showNewGame {
                FButton(FStrings.t("ios.msg.newgame"), kind: .wood, action: onNewGame)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }

    /// The wood plank and the finishing order on it.
    private var ranking: some View {
        Group {
            // ONE continuous wood plank behind the whole ranking (no dividers):
            // WoodFill is a ZStack LAYER (not a .background, which over-drew and
            // made the plank too tall), hard-clipped to exactly rows × rowH so it
            // is a single block that scales with the player count.
            ZStack {
                WoodFill()
                VStack(spacing: 0) {
                    ForEach(rows) { row in
                        HStack(spacing: 12) {
                            // The last place reads "Fool" in the rank column itself
                            // (no separate pill); everyone else is "#N".
                            //
                            // EVERY rank is plain white, including the two that
                            // used to be tinted (brass for 1st, red for the
                            // fool). Those tints were an attempt to make the two
                            // rows a player actually cares about stand out, and
                            // on device they did the opposite — mid-value colours
                            // on bright orange wood are the two LEAST legible
                            // things on the screen (the review's own M10 reading).
                            // Owner's call after seeing it: "just make it white
                            // for #1 and Fool. If it's thick enough text, it
                            // looks fine on the wood background." Round-6 #17
                            // promoted that exact treatment to `onWoodText`
                            // (Tokens.swift) — used here for both columns, since
                            // the name column was ALSO plain text on the same
                            // wood plank and used to fight the rank column with
                            // dark instead of light text.
                            Text(row.isFool ? FStrings.t("ios.fool") : "#\(row.place)")
                                .font(.headline).monospacedDigit()
                                .onWoodText()
                                .frame(width: 56, alignment: .leading)
                            Text(row.name + (row.isYou ? " (\(FStrings.t("ios.you")))" : ""))
                                .font(.body)
                                .onWoodText()
                                .lineLimit(1)
                            Spacer(minLength: 0)
                        }
                        .frame(height: Self.rowH)
                        .padding(.horizontal, 12)
                    }
                }
            }
            // Round-5 B2: WoodFill is an aspect-FILL image, so height-only sizing
            // let it propose a width that grew right along with the plank's
            // height — more players -> taller plank -> WIDER plank, overflowing
            // the surface and clipping the rank column first, then the names
            // (worst case: 8 players, the plank goes blank but for a stray `)`).
            // Pinning the width alongside the height is the fix the finding names
            // directly: the plank is now exactly the surface width at every
            // count 2...8, and WoodFill fills THAT box instead of dictating it.
            //
            // Spelled as min/maxHeight rather than `height:` because SwiftUI has
            // no `frame(maxWidth:height:)` overload — the fixed-size and the
            // flexible-size frames are two different modifiers, and mixing one
            // argument from each does not compile.
            .frame(maxWidth: .infinity, minHeight: plankHeight, maxHeight: plankHeight)
            .clipShape(Rectangle())
            .overlay(Rectangle().strokeBorder(.black.opacity(0.4), lineWidth: 1.5))
            // NO extra horizontal inset here (owner, on device: "ranking list
            // should be same width as the new game button"). The plank used to
            // carry .padding(.horizontal, 4) that the button below does not, so
            // the two wooden blocks on this screen were 4pt out of alignment on
            // each side — enough to read as a mistake once both are full width.
            // Both now rely on the VStack's outer .padding() alone.
        }
    }

    /// "Replay Link" + a copy glyph - this game on foolish.cards, for watching
    /// it back outside Messages.
    ///
    /// Named, not spelled out (owner: "don't put the entire long url in the
    /// screen"). It could not be spelled out anyway: the replay code IS the
    /// game - a self-contained base32 payload the site decodes with the same
    /// kernel - so the URL runs to hundreds of characters and carries nothing a
    /// human would read.
    ///
    /// THE GLYPH SAYS COPY (round 21, the owner: "replay link icon should be
    /// copy icon not arrow out of box, if we're not going to be opening links
    /// anyways then whatever"). It was the standard leaves-this-app arrow, which
    /// is what round 20 could not make true: an iMessage extension may only open
    /// its OWN container app's scheme, this product's container is codeless, and
    /// so every tap on a real phone falls through to the pasteboard. An outward
    /// arrow over a row that always copies is a promise the app cannot keep, and
    /// the honest fix is the glyph, not the behaviour.
    ///
    /// The `onOpenURL` attempt STAYS. It costs one line, it is the correct thing
    /// to do the day there is somewhere to hand the link to, and a copy glyph
    /// over a tap that opened Safari would be the smaller of the two lies. See
    /// `onOpenURL` for why there are two outcomes at all.
    @ViewBuilder
    private func replayLink(_ url: URL) -> some View {
        Button {
            Task { @MainActor in
                // Ask first, copy only if refused - see `onOpenURL`. Both
                // outcomes are recorded, because "which of the two happened on
                // your phone" is the one thing a bug report about this row
                // cannot otherwise tell us.
                if await onOpenURL(url) {
                    FlightRecorder.note("replay-link", "opened")
                    return
                }
                UIPasteboard.general.string = url.absoluteString
                FlightRecorder.note("replay-link", "copied")
                Haptics.fire(.drop)
                withAnimation(.easeOut(duration: 0.18)) { linkCopied = true }
            }
        } label: {
            // The glyph is part of the LINE, not a sibling in a stack: written
            // as concatenated `Text` it rides the same baseline as the words
            // and takes the font's own spacing, where an HStack had to guess at
            // a gap and then fight the symbol's side bearing (6pt read as
            // nearly twice that on device). The underline is applied to the
            // words alone, so it stops where they do.
            //
            // Once the link has been COPIED the row says so - same row, same
            // place, no toast sliding over the ranking - and the glyph turns
            // from the offer into the receipt. A tick rather than a second
            // copy-shaped symbol: the pair has to be legible at a glance in a
            // subheadline run, and two documents beside two documents is not.
            (Text(FStrings.t(linkCopied ? "ios.msg.replaylink.copied"
                                        : "ios.msg.replaylink")).underline(!linkCopied)
             + Text(" ")
             + Text(Image(systemName: linkCopied ? "checkmark" : "doc.on.doc")))
            .font(.subheadline)
            // It sits on the plain wool, below the plank, so it takes the wool
            // half of the text-on-a-surface pairing - the same ink as the title
            // above it, not the bone/shadow combo tuned for wood.
            .onTableText()
            // The row is the target, not the glyph: at a subheadline size the
            // text and arrow together are ~18pt tall, so the padding is what
            // clears Apple's 44pt minimum. `contentShape` first, or the gaps
            // between the two labels would not take a tap.
            .padding(.vertical, 8)
            .padding(.horizontal, 12)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(.isLink)
    }
}
