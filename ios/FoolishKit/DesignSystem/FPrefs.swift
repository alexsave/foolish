// FPrefs.swift — the player's settings, as something SwiftUI can WATCH.
//
// Why this type exists at all. Both settings here are read through STATIC
// accessors deep in the view tree (`FStrings.t` for language, `FTextures
// .Variant` for the table material), so nothing in the tree depends on either
// one as a value: changing a setting changed every future lookup but invalidated
// no view, and the board behind the sheet kept its old words and its old surface
// until some unrelated state change happened to rebuild it. That is the owner's
// round-12 report, "changing locale doesn't seem to change text immediately. It
// should" - and the table material would have landed with exactly the same bug.
//
// One `@Published` per setting fixes it without touching a single call site: a
// view that renders localized text or a table surface declares
//
//     @ObservedObject private var prefs = FPrefs.shared
//
// and SwiftUI re-evaluates its body the instant either setting moves. The
// OBSERVATION is the only thing such a view needs - it never reads `prefs`,
// because `FStrings.t` and `FTextures` already know the answers.
//
// Deliberately NOT a `.id()` rebuild of the surface: that would work too, but it
// throws away every piece of board `@State` (selection, the animator, the veil)
// to change some button captions, and a rebuilt board re-runs its open-replay.
//
// UserDefaults remains the store of record for both, so a cold process reads the
// same answers without this object ever being touched.

import SwiftUI

/// Which material the table is made of (owner, round 12: "Distracting wool??
/// Have felt texture green casino table option in settings").
///
/// A type rather than a Bool so a third surface is an added case, matching how
/// `FTextures.Variant` treats looks.
public enum TableSurface: String, CaseIterable, Sendable {
    case wool, felt

    /// The settings row's label key (FStrings).
    public var labelKey: String {
        switch self {
        case .wool: return "ios.settings.table.wool"
        case .felt: return "ios.settings.table.felt"
        }
    }
}

@MainActor
public final class FPrefs: ObservableObject {
    public static let shared = FPrefs()
    private init() {}

    // MARK: language

    /// Published so views re-render; `FStrings.override` stays the store of
    /// record, so a fresh process resolves the same language without this.
    @Published public private(set) var language: AppLanguage = FStrings.override

    public func setLanguage(_ lang: AppLanguage) {
        FStrings.override = lang
        language = lang
    }

    // MARK: table surface

    private static let tableKey = "ios.table.surface"

    /// Published for the same reason as `language`. The default is `.wool`:
    /// felt is the option, not the new baseline.
    @Published public private(set) var table: TableSurface = {
        let raw = UserDefaults.standard.string(forKey: FPrefs.tableKey)
        return raw.flatMap(TableSurface.init(rawValue:)) ?? .wool
    }()

    public func setTable(_ surface: TableSurface) {
        UserDefaults.standard.set(surface.rawValue, forKey: Self.tableKey)
        table = surface
    }

    /// The stored choice, readable WITHOUT observing - for `FTextures.Variant`,
    /// which is a plain value type constructed inside `body` and has nowhere to
    /// hold an observation. Views get their invalidation from `@ObservedObject`
    /// above; this is just the current answer.
    ///
    /// nonisolated so it can be read from the same non-isolated contexts
    /// `FStrings.t` is read from; UserDefaults is thread-safe.
    public nonisolated static var storedTable: TableSurface {
        let raw = UserDefaults.standard.string(forKey: FPrefs.tableKey)
        return raw.flatMap(TableSurface.init(rawValue:)) ?? .wool
    }
}
