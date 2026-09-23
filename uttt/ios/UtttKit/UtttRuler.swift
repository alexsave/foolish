// UtttRuler.swift - where the motion ruler's marks go on this game's sheet.
//
// The generic half (the palette, the square, the edge bars and the banded
// strip, the dev-file reader) is shared/swift/MotionRuler.swift. This file is
// only the placement: which element carries which square, so a filmed drawer
// transition can be scored element by element (uttt/ios/Tools/track.py).
//
//   red bar / green bar   the sheet's top and bottom edge (the resizing box)
//   magenta               the board's centre
//   cyan x4               the board's four corners, so a scale shows
//   orange                the "you are" mark
//   yellow                the headline
//   lime                  the subline
//   blue                  the rulebook door
//   violet                the Again door, and the moving pen stroke (never
//                         on screen together: a finished game draws no stroke)
//   pink                  the travelling highlighter
//
// OFF unless the rig writes `dev.ruler` into the App Group, and absent from a
// Release build: `on` is a constant false there, so the file name never ships.

import SwiftUI

enum UtttRuler {
    static var on: Bool {
#if DEBUG
        UtttDev.ruler
#else
        false
#endif
    }
}
