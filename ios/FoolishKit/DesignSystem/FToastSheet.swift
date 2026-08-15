// FToast + FSheet (§5.4). Minimal chrome: toasts for transient status (rejects),
// sheets for the few modal flows. 150ms ease-out (FMotion.chrome), never a spring.

import SwiftUI

/// A transient message at the top of the screen. Reject toasts pair with the
/// rigid haptic fired at the call site (§8.2 C1).
public struct FToast: View {
    public let text: String
    public var accent: Bool
    public init(_ text: String, accent: Bool = false) { self.text = text; self.accent = accent }

    public var body: some View {
        Text(text)
            .font(FType.body(15))
            .foregroundColor(FColor.textPrimary)
            .padding(.horizontal, FSpace.l)
            .padding(.vertical, FSpace.s)
            .background(accent ? FColor.accent : FColor.surface)
            .clipShape(RoundedRectangle(cornerRadius: FRadius.chip))
            .shadow(color: .black.opacity(0.35), radius: 8, y: 2)
    }
}

/// A container styled as the app's sheet surface. Callers present it with
/// `.sheet`; this is the visual chrome inside.
public struct FSheet<Content: View>: View {
    private let title: String
    private let content: Content
    public init(title: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }
    public var body: some View {
        VStack(alignment: .leading, spacing: FSpace.l) {
            Text(title).font(FType.title(20)).foregroundColor(FColor.textPrimary)
            content
            Spacer(minLength: 0)
        }
        .padding(FSpace.xl)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(FColor.surface)
        .clipShape(RoundedRectangle(cornerRadius: FRadius.sheet))
    }
}

/// Attach a toast that auto-dismisses. Usage: `.fToast($message)`.
public extension View {
    func fToast(_ message: Binding<String?>, accent: Bool = false, seconds: Double = 2.4) -> some View {
        overlay(alignment: .top) {
            if let text = message.wrappedValue {
                FToast(text, accent: accent)
                    .padding(.top, FSpace.s)
                    .transition(.move(edge: .top).combined(with: .opacity))
                    .task {
                        try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
                        withAnimation(FMotion.chrome) { message.wrappedValue = nil }
                    }
            }
        }
        .animation(FMotion.chrome, value: message.wrappedValue)
    }

    /// A brief plain-text flash (1.0(4)) — white text, NO background pill — for
    /// the message board's reject reasons. The owner asked for "white text
    /// briefly with no background instead of the pill"; legibility over the wool
    /// comes from a soft shadow, not a surface. Fades, then auto-clears.
    func fFlash(_ message: Binding<String?>, seconds: Double = 1.7) -> some View {
        overlay(alignment: .top) {
            if let text = message.wrappedValue {
                Text(text)
                    .font(FType.body(15))
                    .fontWeight(.semibold)
                    .foregroundColor(.white)
                    .multilineTextAlignment(.center)
                    .shadow(color: .black.opacity(0.55), radius: 4, y: 1)
                    .padding(.horizontal, FSpace.xl)
                    .padding(.top, FSpace.m)
                    .transition(.opacity)
                    .task {
                        try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
                        withAnimation(FMotion.chrome) { message.wrappedValue = nil }
                    }
            }
        }
        .animation(FMotion.chrome, value: message.wrappedValue)
    }
}
