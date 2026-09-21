import SwiftUI
import UtttKit

/// A plain app that shows the game screen at the sizes Messages gives it, so
/// they can be looked at without a Messages extension in the loop.
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
    /// THE SIZES ARE MEASURED, NOT INVENTED. Messages hands the collapsed view
    /// 340 points, or 323 while the compose field holds the first responder,
    /// and the expanded view the screen less about 126 points of its own
    /// chrome - 541 on an SE, 726 on a 16, 748 on a 16 Pro, 830 on a Pro Max.
    /// The width is always the phone's.
    enum Size: String, CaseIterable, Identifiable {
        case collapsed = "340", typing = "323", expanded = "expanded"
        var id: String { rawValue }

        func height(screen: CGSize) -> CGFloat {
            switch self {
            case .collapsed: return 340
            case .typing:    return 323
            case .expanded:  return screen.height - 126
            }
        }

        /// So a screenshot run can ask for one size without a tap:
        /// `simctl launch <udid> cards.uttt.preview --size expanded`.
        static var launched: Size {
            let a = ProcessInfo.processInfo.arguments
            guard let i = a.firstIndex(of: "--size"), i + 1 < a.count,
                  let s = Size(rawValue: a[i + 1]) else { return .collapsed }
            return s
        }
    }

    @State private var size: Size = Size.launched
    @State private var model = UtttModel(seed: 77, you: .x)
    @State private var loaded = false

    /// Twelve moves of the game in the design document, so the harness shows
    /// marks and a live wash rather than an empty grid.
    static let sample = [34, 67, 44, 80, 76, 43, 69, 62, 79, 63, 4, 40]

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .bottom) {
                Color(white: 0.07)
                // Anchored to the bottom, which is where Messages puts it.
                UtttGameScreen(model: model)
                    .frame(width: geo.size.width,
                           height: size.height(screen: geo.size))
                VStack {
                    Picker("", selection: $size) {
                        ForEach(Size.allCases) { Text($0.rawValue).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    .frame(width: 240)
                    .padding(.top, 52)
                    Spacer()
                }
            }
        }
        .ignoresSafeArea()
        .onAppear {
            guard !loaded else { return }
            loaded = true
            Uttt.newGame(seed: 77)
            for m in Self.sample { Uttt.play(m) }
            model.refresh()
        }
    }
}
