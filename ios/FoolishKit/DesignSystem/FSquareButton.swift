// FSquareButton.swift — a square wood button (1.0(4)), the icon sibling of
// FButton's wood pills. It backs the Settings/Help pair the message board floats
// on the LEFT, mirroring the right-hand action column: same WoodFill surface,
// same sharp corners, hairline border and onWoodText ink, so the two sides read
// as one family. Square by construction (width == height).

import SwiftUI

public struct FSquareButton: View {
    private let systemImage: String
    private let side: CGFloat
    private let accessibility: String
    private let action: () -> Void
    private let holdSeconds: Double
    private let onHold: (() -> Void)?
    /// The hold already fired for the touch still on screen, so the tap that
    /// ends it must be swallowed.
    ///
    /// A long press ENDS while the finger is still down (that is what makes it
    /// feel like a hold), but the Button's own action fires later, on release -
    /// so without this a hold on the gear both opened the diagnostics
    /// AND opened Settings on top of them. The alternative, replacing the Button
    /// with raw tap/press gestures, would cost the button's press styling and
    /// accessibility traits for a one-line problem.
    @State private var holdFired = false

    /// `side` is BOTH the width and the height — the board sizes it to the action
    /// buttons' height so the two sides match (FActionBar's compact pills are 40pt
    /// tall).
    ///
    /// `onHold` is an optional SECOND action on the same square, reached by
    /// holding it for `holdSeconds`. Deliberately undiscoverable: the one thing
    /// it drives today is the diagnostics dump (round 12), which is for the
    /// owner and not for players.
    public init(systemImage: String, side: CGFloat = 40, accessibility: String = "",
                holdSeconds: Double = 5, onHold: (() -> Void)? = nil,
                action: @escaping () -> Void) {
        self.systemImage = systemImage
        self.side = side
        self.accessibility = accessibility
        self.holdSeconds = holdSeconds
        self.onHold = onHold
        self.action = action
    }

    public var body: some View {
        Button(action: {
            if holdFired { holdFired = false; return }
            Haptics.fire(.drop); action()
        }) {
            Image(systemName: systemImage)
                .font(.system(size: side * 0.44, weight: .heavy))
                .onWoodText()                                  // white ink + drop shadow, as the wood pills
                .frame(width: side, height: side)
                .background(WoodFill())
                .overlay(Rectangle().strokeBorder(Color.black.opacity(0.35), lineWidth: 1))
                .clipShape(Rectangle())                        // sharp corners — wood is a plank, not a pill
        }
        .accessibilityLabel(accessibility)
        // Simultaneous, not `.onLongPressGesture`: the Button must keep owning
        // the tap (its press highlight and accessibility action come with it).
        // The hold reports at `holdSeconds` with a haptic, so you know it took
        // without having to look.
        .simultaneousGesture(
            LongPressGesture(minimumDuration: holdSeconds)
                .onEnded { _ in
                    // No handler: the gesture is inert and `holdFired` stays
                    // false, so an ordinary square behaves exactly as before
                    // however long you rest a finger on it.
                    guard let onHold else { return }
                    holdFired = true
                    Haptics.fire(.drop)
                    onHold()
                }
        )
    }
}

/// The Settings (gear) + Rulebook (book) pair — ONE layout for every surface
/// that floats it bottom-left: the board, the New-game setup and the lobby
/// (durak-rules-redesign put it on the latter two). Two 40pt squares with a
/// 16pt gap, so the pair spans one action-button width (40 + 16 + 40 = 96 =
/// FActionBar width) and mirrors the right-hand action column wherever both
/// appear; the inner horizontal padding is FActionBar's own inset, so the pair
/// lands on the same edge line on every screen. Round-9 (owner): the help "?"
/// became a little book - it opens the rulebook, so it looks like one.
public struct SettingsHelpSquares: View {
    /// Re-render this view when a setting changes (see FPrefs). Only the
    /// OBSERVATION matters - the strings still come from FStrings.t and the
    /// table surface still comes from FTextures.
    @ObservedObject private var prefs = FPrefs.shared
    private let onSettings: () -> Void
    private let onHelp: () -> Void
    private let onDiagnostics: (() -> Void)?

    /// `onDiagnostics` (round 12, owner: "if you hold the settings button for 4
    /// seconds, it pops up") is the hidden second action on the GEAR - the
    /// last-message dump, which until now only ever appeared when a bubble
    /// failed to open. Nil on any surface that has nothing to dump.
    public init(onSettings: @escaping () -> Void, onHelp: @escaping () -> Void,
                onDiagnostics: (() -> Void)? = nil) {
        self.onSettings = onSettings
        self.onHelp = onHelp
        self.onDiagnostics = onDiagnostics
    }

    public var body: some View {
        HStack(spacing: FSpace.l) {   // 40 + 16 + 40 = 96 = FActionBar width
            FSquareButton(systemImage: "gearshape.fill", side: 40,
                          accessibility: FStrings.t("ios.settings.title"),
                          onHold: onDiagnostics, action: onSettings)
            FSquareButton(systemImage: "book.fill", side: 40,
                          accessibility: FStrings.t("ios.help"), action: onHelp)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, FSpace.m)   // same inner inset as FActionBar
    }
}
