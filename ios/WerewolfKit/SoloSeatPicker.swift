// THE SOLO RIG. One operator, one simulator, one conversation thread, and a whole
// seven-player night.
//
// WHY THIS HAS TO EXIST AT ALL. You cannot add participants to the real Messages
// host - it is a signed Apple binary - and a night phase is not demonstrable one
// seat at a time. The night's whole claim is "every living player must send, so
// sending is not a tell", and that claim is only observable if somebody can BE
// every living player in one thread: pick a seat, choose, send, pick the next
// seat, choose, send, and watch the night resolve.
//
// It is also how the two hardest properties get looked at with human eyes rather
// than only asserted in C: that a villager's screen and a wolf's screen are the
// same screen, and that the wolves' channel is not on the villager's.
//
// THE WHOLE FILE IS `#if DEBUG || SOLO_TESTING`, and that is the security
// property. A RELEASE BUILD MUST NEVER OFFER A SEAT PICKER. In the game this was
// forked from, choosing a seat meant choosing whose cards you could see. Here it
// means CHOOSING TO BE THE WOLF - and choosing to read the wolves' channel, and
// choosing to be the seat whose kill vote counts tonight. There is no version of
// that which is acceptable in a shipped build, however well hidden the control
// is, so it is not hidden: it is absent. `ios/scripts/release_gate.sh` asserts
// that, at the source and in the built Release binary.
//
// Release's answer to an unresolved seat is the spectator surface, which is built
// from the same masked view every other surface is - so it is public-safe by
// construction rather than by review.
#if DEBUG || SOLO_TESTING
import SwiftUI

/// Every seat, named where the roster knows one. Offers seats a real Release
/// build would have resolved automatically, which is exactly the point: on one
/// simulator both ends of the thread share one App Group and one participant
/// identity, so a received bubble always resolves to the SENDER's seat and the
/// receiver can never be looked at.
public struct SoloSeatPicker: View {
    /// Whether this control may be shown at all. Guarded through `DevFlags.flag`
    /// with the SHIPPING value as its argument, so there is one knob and its
    /// default is what ships: false, meaning no picker.
    public static var offered: Bool { DevFlags.flag("solo.seatpicker", shipping: false) }

    private let seatCount: Int
    private let current: Int
    private let onPick: (Int) -> Void

    public init(seatCount: Int, current: Int, onPick: @escaping (Int) -> Void) {
        self.seatCount = seatCount
        self.current = current
        self.onPick = onPick
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("SOLO RIG - not in a release build")
                .font(.system(size: 11, weight: .semibold))
                .tracking(1.2)
                .foregroundStyle(Night.chosen)
            Text("Play as")
                .font(.system(size: 13))
                .foregroundStyle(Night.quiet)
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 96), spacing: 8)], spacing: 8) {
                ForEach(0..<seatCount, id: \.self) { seat in
                    Button { onPick(seat) } label: {
                        VStack(spacing: 2) {
                            Text(Self.name(seat))
                                .font(.system(size: 13, weight: .semibold))
                                .lineLimit(1)
                            // The role IS shown here, and only here. A rig that
                            // hid it would not be a rig: the operator is every
                            // player at once and already knows everything, and
                            // what they are checking is whether the SEAT's own
                            // screen knows too much.
                            Text(Self.roleWord(seat))
                                .font(.system(size: 10))
                                .foregroundStyle(Night.quiet)
                            Text(Self.sentWord(seat))
                                .font(.system(size: 10))
                                .foregroundStyle(Night.quiet)
                        }
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 8)
                    }
                    .buttonStyle(.plain)
                    .background(RoundedRectangle(cornerRadius: 10)
                        .fill(seat == current ? Night.chosen.opacity(0.22) : Night.raised))
                    .overlay(RoundedRectangle(cornerRadius: 10)
                        .stroke(seat == current ? Night.chosen : Night.edge, lineWidth: 1))
                    .accessibilityIdentifier("solo.seat.\(seat)")
                }
            }
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: Night.corner).fill(Night.chosen.opacity(0.08)))
        .overlay(RoundedRectangle(cornerRadius: Night.corner)
            .stroke(Night.chosen.opacity(0.4), lineWidth: 1))
        .accessibilityIdentifier("solo.picker")
    }

    /// The rig reads the UNMASKED truth by asking each seat about itself, which is
    /// the only view that carries its own role. It does not need a back door into
    /// the kernel, and it does not get one: a bridge entry that answered "what is
    /// seat N really" would be an entry a shipped build also has.
    private static func name(_ seat: Int) -> String {
        let k = Kernel.shared
        for i in 0..<k.rosterCount where k.rosterSeat(i) == seat {
            if let n = k.rosterName(i) { return n }
        }
        return "Seat \(seat + 1)"
    }

    private static func roleWord(_ seat: Int) -> String {
        switch Kernel.shared.role(seat, of: seat) {
        case .wolf:     return "wolf"
        case .seer:     return "seer"
        case .villager: return "villager"
        case .unknown:  return "?"
        }
    }

    private static func sentWord(_ seat: Int) -> String {
        guard Kernel.shared.isAlive(seat, seat) else { return "out" }
        switch Kernel.shared.sent(seat, seat) {
        case .no:      return "to send"
        case .yes:     return "sent"
        case .carried: return "passed"
        }
    }
}
#endif
