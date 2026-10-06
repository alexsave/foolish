// Buttons.swift - the study's controls (UI.html `.plank`, the Controls tab;
// DECISIONS I27): every button is the plate's plate, 40 tall, its word in
// IM Fell English small caps. Which plate is the caller's: bronze for a move,
// the BLOOD plate for Liar (the call), sunk for a move not allowed now, quiet
// for a step back. The verbs and their words are the kernel's (Kernel.word);
// nothing here names one.
import SwiftUI
import UIKit

public enum CnHaptic { case pickUp, drop, reject }

public enum Haptics {
    public static var isEnabled = true
    public static func fire(_ h: CnHaptic) {
        guard isEnabled else { return }
        switch h {
        case .pickUp: UIImpactFeedbackGenerator(style: .light).impactOccurred()
        case .drop:   UIImpactFeedbackGenerator(style: .medium).impactOccurred()
        case .reject: UIImpactFeedbackGenerator(style: .rigid).impactOccurred()
        }
    }
}

/// `.plank:active`: .96 for 80ms, and no system decoration outside the plate.
public struct FPressStyle: ButtonStyle {
    public init() {}
    public func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.96 : 1)
            .animation(FMotion.press, value: configuration.isPressed)
    }
}

/// Which plate a plank is (`.plank`, `.plank.glow`, `.plank.call`,
/// `.plank.quiet`, `.plank.sunk`).
public enum PlankKind: Equatable {
    /// bronze verdigris: a move
    case bronze
    /// bronze, lit by the cold glow: the move the table waits for
    case glow
    /// the blood plate: Liar
    case call
    /// a step back (the stepper's minus, Leave)
    case quiet
    /// a move not allowed now: the plate under water
    case sunk

    var ink: Color {
        switch self {
        case .bronze: return Ink.bone2
        case .glow:   return Color(hex: 0xF1FFF8)
        case .call:   return Color(hex: 0xF3DDCF)
        case .quiet:  return Ink.ink
        case .sunk:   return Color(.sRGB, red: 190 / 255, green: 205 / 255, blue: 195 / 255, opacity: 0.5)
        }
    }
}

/// The plank's face: metal, tint, rivets, the kind's light. No label.
struct PlankFace: View {
    let kind: PlankKind
    var seed: Int
    var rivets = true
    var ring: CGFloat = 3

    /// The rim: the study's planks are <button>s, and the button's own 2px
    /// outset border (#a8a8a8 top and left, #545454 bottom and right, measured
    /// on its capture) frames every one of them; the plate sits inside it.
    static let rim: CGFloat = 2

    var body: some View {
        ZStack {
            PlankRim(width: Self.rim)
            inner.padding(Self.rim)
        }
        .shadow(color: .black.opacity(0.5), radius: 7, y: 6)
    }

    private var inner: some View {
        let shape = RoundedRectangle(cornerRadius: 5 - Self.rim, style: .circular)
        return PlateMetal(seed: seed, corner: 5 - Self.rim, ring: ring, base: kind == .call ? Color(hex: 0x3A1A12) : Ink.bronze2, shadow: false) {
            switch kind {
            case .call:
                LinearGradient(stops: [.init(color: Color(.sRGB, red: 120 / 255, green: 40 / 255, blue: 24 / 255, opacity: 0.68), location: 0),
                                       .init(color: Color(.sRGB, red: 80 / 255, green: 22 / 255, blue: 12 / 255, opacity: 0.6), location: 0.45),
                                       .init(color: Color(.sRGB, red: 30 / 255, green: 6 / 255, blue: 2 / 255, opacity: 0.78), location: 1)],
                               startPoint: .top, endPoint: .bottom)
            case .quiet:
                BronzeTint(top: 0.06, mid: 0.45, foot: 0.5)
            default:
                BronzeTint()
            }
        }
        .overlay {
            if rivets {
                GeometryReader { g in
                    ForEach(0..<4, id: \.self) { i in
                        PlankRivet().position(x: i % 2 == 0 ? 7 : g.size.width - 7, y: i < 2 ? 7 : g.size.height - 7)
                    }
                }
            }
        }
        .overlay {
            switch kind {
            case .call:
                // inset 0 0 18px rgba(140,30,16,.55)
                InnerGlow(color: Color(.sRGB, red: 140 / 255, green: 30 / 255, blue: 16 / 255, opacity: 0.55), radius: 9)
            case .glow:
                // inset 0 0 0 1px rgba(143,251,224,.35), inset 0 0 14px rgba(143,251,224,.25)
                ZStack {
                    shape.strokeBorder(Ink.glow.opacity(0.35), lineWidth: 1)
                    InnerGlow(color: Ink.glow.opacity(0.25), radius: 7)
                }
            case .sunk:
                shape.fill(Color(.sRGB, red: 6 / 255, green: 36 / 255, blue: 38 / 255, opacity: 0.62))
            default:
                EmptyView()
            }
        }
    }
}

/// A button's outset border: light on the top and left edges, dark on the
/// bottom and right, the corners mitred, round at radius 5.
struct PlankRim: View {
    var width: CGFloat
    var body: some View {
        let shape = RoundedRectangle(cornerRadius: 5, style: .circular)
        ZStack {
            shape.strokeBorder(Color(hex: 0x545454), lineWidth: width)
            shape.strokeBorder(Color(hex: 0xA8A8A8), lineWidth: width)
                .mask {
                    GeometryReader { g in
                        let w = g.size.width, h = g.size.height
                        Path { p in
                            p.move(to: .zero); p.addLine(to: CGPoint(x: w, y: 0)); p.addLine(to: CGPoint(x: w - width, y: width))
                            p.addLine(to: CGPoint(x: width, y: width)); p.addLine(to: CGPoint(x: width, y: h - width))
                            p.addLine(to: CGPoint(x: 0, y: h)); p.closeSubpath()
                        }
                        .fill(Color.black)
                    }
                }
        }
        .allowsHitTesting(false)
    }
}

/// The plank's rivet (`--rv`): a radial dot 7 points in from the corner,
/// bright at its middle, dark at 1.5, a shadow to 2.4, gone at 3.2.
struct PlankRivet: View {
    var body: some View {
        Circle()
            .fill(RadialGradient(stops: [.init(color: Color(hex: 0x9A8C66), location: 0),
                                         .init(color: Color(hex: 0x2A2416), location: 1.5 / 3.2),
                                         .init(color: Color.black.opacity(0.55), location: 2.4 / 3.2),
                                         .init(color: .clear, location: 1)],
                                 center: .center, startRadius: 0, endRadius: 3.2))
            .frame(width: 6.4, height: 6.4)
    }
}

/// An inset glow: a soft stroke inside the rounded plate.
struct InnerGlow: View {
    var color: Color
    var radius: CGFloat
    var body: some View {
        let shape = RoundedRectangle(cornerRadius: 5 - PlankFace.rim, style: .circular)
        shape.stroke(color, lineWidth: radius * 2)
            .blur(radius: radius / 2)
            .clipShape(shape)
            .allowsHitTesting(false)
    }
}

/// A plank with a word: 40 tall, small caps at 18 (`.plank`). `kind` is the
/// caller's; a sunk plank is disabled.
struct PlankButton: View {
    let title: String
    var kind: PlankKind = .bronze
    var width: CGFloat? = nil
    var height: CGFloat = 40
    var fontSize: CGFloat = 18
    var seed: Int = 0
    let action: () -> Void

    var body: some View {
        Button(action: { Haptics.fire(.drop); action() }) {
            label
                .lineLimit(1)
                .minimumScaleFactor(0.7)
                .padding(.horizontal, 18)
                .frame(minWidth: width, maxWidth: width ?? .infinity, minHeight: height, maxHeight: height)
                .background(PlankFace(kind: kind, seed: seed))
                .contentShape(Rectangle())
        }
        .buttonStyle(FPressStyle())
        .disabled(kind == .sunk)
    }

    @ViewBuilder private var label: some View {
        let t = Text(title).font(FType.sc(fontSize)).tracking(fontSize * 0.08).foregroundStyle(kind.ink)
        switch kind {
        case .glow:
            t.shadow(color: Ink.glow.opacity(0.7), radius: 5).shadow(color: .black, radius: 0, y: 1)
        case .sunk:
            t.shadow(color: .black.opacity(0.6), radius: 0, y: 1)
        default:
            t.shadow(color: .black.opacity(0.8), radius: 1.5, y: 1).shadow(color: .black, radius: 0, y: 1)
        }
    }
}

/// A square plank (`.plank.sq`): no rivets, the ring 2 in, one glyph at half
/// its side in the roman.
struct SquarePlank: View {
    let glyph: String
    var side: CGFloat = 40
    var kind: PlankKind = .bronze
    var seed: Int = 0
    let accessibility: String
    let action: () -> Void

    var body: some View {
        Button(action: { Haptics.fire(.drop); action() }) {
            Text(verbatim: glyph)
                .font(FType.serif(side * 0.5))
                .foregroundStyle(kind.ink)
                .shadow(color: .black.opacity(0.8), radius: 1.5, y: 1)
                .frame(width: side, height: side)
                .background(PlankFace(kind: kind, seed: seed, rivets: false, ring: 2))
                .contentShape(Rectangle())
        }
        .buttonStyle(FPressStyle())
        .disabled(kind == .sunk)
        .accessibilityLabel(accessibility)
    }
}
