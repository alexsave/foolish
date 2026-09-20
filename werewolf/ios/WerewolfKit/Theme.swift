// The night's palette, in one place.
//
// One theme and no light variant, deliberately: the screen is a night, the
// iMessage drawer sits on the keyboard's dark chrome anyway, and a game whose
// whole subject is not being able to see should not flip to white paper because
// the phone is in Light mode.
import SwiftUI

public enum Night {
    public static let ground   = Color(red: 0.063, green: 0.078, blue: 0.149)
    public static let raised   = Color(red: 0.102, green: 0.125, blue: 0.220)
    public static let edge     = Color(red: 0.200, green: 0.235, blue: 0.376)
    public static let ink      = Color(red: 0.933, green: 0.925, blue: 0.878)
    public static let quiet    = Color(red: 0.561, green: 0.596, blue: 0.706)
    public static let moon     = Color(red: 0.933, green: 0.925, blue: 0.878)
    /// The pick. Warm against everything else, so the one seat you chose reads at
    /// a glance on a screen that is otherwise all one temperature.
    public static let chosen   = Color(red: 0.906, green: 0.678, blue: 0.325)
    /// The wolves' channel. A deep red that is unmistakably not the pick colour -
    /// a wolf glancing down must never mistake the channel for the board.
    public static let pack     = Color(red: 0.545, green: 0.102, blue: 0.102)

    public static let corner: CGFloat = 14
    public static let gutter: CGFloat = 16
}
