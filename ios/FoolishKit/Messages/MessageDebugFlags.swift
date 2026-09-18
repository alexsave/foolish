// MessageDebugFlags — the single-device testing knobs, never in a Release build.

import Foundation


#if DEBUG || SOLO_TESTING
/// DEBUG-only knobs for single-device testing (never compiled into Release —
/// `#if DEBUG || SOLO_TESTING` means a TestFlight/App Store build cannot contain
/// any of this — the shipping Release build defines neither condition; the
/// on-device testing build opts in with SWIFT_ACTIVE_COMPILATION_CONDITIONS,
/// and the CI Release build is what proves it).
public enum MessageDebugFlags {
    /// Force the seat picker on every adopted bubble so both seats are playable
    /// on ONE simulator (which cannot otherwise distinguish sender from receiver).
    /// The FoolishHarness turns this OFF: it gives each fake participant a
    /// distinct identity + its own seat cache, so seat inference resolves
    /// automatically and the picker would be wrong to show.
    public static var pickSeatOnAdopt = true

    /// SOLO PLAY (owner ask, device testing): seat extra players from this one
    /// device so a real chat can reach a startable game with nobody else in it.
    ///
    /// The shipping lobby is deliberately un-startable alone — Start needs 2+
    /// joined, and the only way to a second join is another human on another
    /// device. That is correct for the product and useless for testing the
    /// extension on a phone: you cannot reach a board at all, so none of the
    /// board work can be checked on device. With this on, the lobby offers
    /// "Add player" (claims the next free seat with a puppet name) and offers
    /// Start as soon as two seats are filled, bypassing the round-5 M9
    /// authorship gate — that gate exists to stop one human locking others
    /// out, and in solo play there is nobody to lock out.
    ///
    /// Pairs with `pickSeatOnAdopt`: add a puppet, start, then every time you
    /// open a bubble the picker asks which seat you are, so one person plays
    /// every hand in one chat.
    public static var soloSeats = true

    /// ROUND 20: take the RELEASE route when seat identity is ambiguous - the
    /// public spectator board - instead of DEBUG's seat picker.
    ///
    /// That screen is `#if DEBUG`/`#else`, so until now it was the one surface
    /// in the app no rig could reach and no screenshot could be taken of: every
    /// harness build is a debug build. It had therefore never been looked at
    /// with a FINISHED game on it, which is exactly how it came to draw a swept
    /// empty table and the line "spectating - open the game from your own bubble
    /// to play" over a game that was over (owner: "spectators should still be
    /// able to see win screen").
    ///
    /// Off by default, so nothing about the normal harness changes; the
    /// `spectator-over` scenario turns it on.
    public static var spectateWhenAmbiguous = false
}
#endif
