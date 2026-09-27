// InsertStaging.swift - the thin Swift face of shared/c/msg_stage/msg_stage.h:
// when an insert may go, what an unanswered one means, and whether a bubble
// handed to didReceive is an arrival or this device's own bubble coming back.
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

    /// How many tries the compact drawer gets before the door.
    public static var attempts: Int { Int(MS_INSERT_ATTEMPTS) }

    /// What an insert's silence means on try `attempt` (1-based).
    public enum Silence { case listen, retry, door }
    public static func silence(attempt: Int, compact: Bool) -> Silence {
        switch ms_insert_silence(Int32(attempt), compact ? 1 : 0) {
        case MS_INSERT_RETRY: return .retry
        case MS_INSERT_DOOR:  return .door
        default:              return .listen
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
