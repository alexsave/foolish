import SwiftUI
import UtttKit

/// A plain app that shows the game screens at the three sizes Messages gives,
/// so they can be looked at without a Messages extension in the loop.
///
/// This is a DEVELOPMENT HARNESS and ships in nothing. It exists because the
/// only other way to see a screen is to drive the real Messages app, and a
/// design that can only be inspected by playing a game is a design nobody
/// inspects.
@main
struct UtttPreviewApp: App {
    var body: some Scene {
        WindowGroup { PreviewRoot() }
    }
}

struct PreviewRoot: View {
    enum Size: String, CaseIterable, Identifiable {
        case collapsed = "340", expanded = "expanded", bubble = "300x195"
        var id: String { rawValue }
    }
    @State private var size: Size = .expanded
    @State private var model = UtttModel(seed: 77, you: .x, solo: true)

    var body: some View {
        VStack(spacing: 10) {
            Picker("", selection: $size) {
                ForEach(Size.allCases) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.segmented)
            .padding(.horizontal, 12)

            GeometryReader { geo in
                let w = min(geo.size.width, 402.0)
                Group {
                    switch size {
                    case .collapsed: UtttGameScreen(model: model).frame(width: w, height: 340)
                    case .expanded:  UtttGameScreen(model: model).frame(width: w, height: 700)
                    case .bubble:    UtttGameScreen(model: model).frame(width: 300, height: 195)
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
                .clipped()
            }
            Spacer(minLength: 0)
        }
        .padding(.top, 8)
        .background(Color(white: 0.07))
    }
}
