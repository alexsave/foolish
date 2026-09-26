// InsertStaging.swift - the thin Swift face of shared/c/msg_stage/msg_stage.h:
// when an insert may go, what one stage's insert loop does on silence, an
// error, a collapse or a tap on the door, and whether a bubble handed to
// didReceive is an arrival or this device's own bubble coming back.
//
// Every number and every rule is the C header's (module CMsgStage), and the
// evidence for each is shared/c/msg_stage/INSERT_GATING.md. What a product
// does with a verdict - its watchdog, its door, its settlement - stays in the
// product; this only answers the questions.

import CMsgStage

public enum InsertStaging {
    /// Is the extension view a drawer yet (ms_drawer_up)? The + drawer's
    /// window-sized first appearance is not; an expanded drawer short of the
    /// window is; a compact one must be well short of it.
    public static func drawerUp(window: Double, view: Double, expanded: Bool) -> Bool {
        ms_drawer_up(Float(window), Float(view), expanded ? 1 : 0) != 0
    }

    /// How long an insert may go unanswered before its silence means something.
    public static var silenceSeconds: Double { Double(MS_INSERT_SILENCE_MS) / 1000 }

    /// The beat before an insert refused with an error is asked again.
    public static var errorBeatSeconds: Double { Double(MS_INSERT_ERROR_MS) / 1000 }

    /// ONE STAGE'S INSERT LOOP (ms_stage). The host inserts, runs the two
    /// timers, reports the drawer going compact and the door being tapped;
    /// the machine says what each of those means and keeps the budgets.
    /// Every timer and every insert carries the try it was started for and
    /// hands it back, so a timer of a try that has been overtaken is moot.
    /// The host still decides WHICH STAGE is current (its generation): the
    /// machine only knows one stage at a time, so a stale stage's timer must
    /// never reach it.
    public struct Loop {
        private var m = ms_stage()
        public init() {}

        public enum Action { case none, insert, insertLater, park, arm, door, landed, revert }

        /// The try the last action was handed for (1-based).
        public var tryNumber: Int { Int(m.try_no) }
        /// A door is up.
        public var atDoor: Bool { m.state == MS_STAGE_DOOR }

        /// A new stage, a send or a cancel: nothing of this loop goes again.
        public mutating func reset() { ms_stage_reset(&m) }
        /// The first try; always `.insert`.
        public mutating func first() -> Action { Self.action(ms_stage_first(&m)) }
        /// Try `try` went `silenceSeconds` unanswered; `compact` is the drawer now.
        public mutating func silence(try t: Int, compact: Bool) -> Action {
            Self.action(ms_stage_silence(&m, Int32(t), compact ? 1 : 0))
        }
        /// Try `try` answered, `ok` being a nil error.
        public mutating func answer(try t: Int, ok: Bool) -> Action {
            Self.action(ms_stage_answer(&m, Int32(t), ok ? 1 : 0))
        }
        /// The beat after try `try`'s error has passed.
        public mutating func due(try t: Int) -> Action { Self.action(ms_stage_due(&m, Int32(t))) }
        /// The drawer is compact (didTransition).
        public mutating func compact() -> Action { Self.action(ms_stage_compact(&m)) }
        /// The door was tapped.
        public mutating func doorTapped() -> Action { Self.action(ms_stage_door_tap(&m)) }

        private static func action(_ a: Int32) -> Action {
            switch a {
            case MS_ACT_INSERT:       return .insert
            case MS_ACT_INSERT_LATER: return .insertLater
            case MS_ACT_PARK:         return .park
            case MS_ACT_ARM:          return .arm
            case MS_ACT_DOOR:         return .door
            case MS_ACT_LANDED:       return .landed
            case MS_ACT_REVERT:       return .revert
            default:                  return .none
            }
        }
    }

    /// What a bubble handed to didReceive is, given the product's own answers
    /// to "is it mine" (the draft, the staged bubble or the last one sent) and
    /// "is it the one staged".
    public enum Receipt { case arrival, echo, echoOfStaged }
    public static func receive(mine: Bool, staged: Bool) -> Receipt {
        switch ms_receive(mine ? 1 : 0, staged ? 1 : 0) {
        case MS_RECV_ECHO:      return .echo
        case MS_RECV_ECHO_SENT: return .echoOfStaged
        default:                return .arrival
        }
    }
}
