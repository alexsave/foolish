import SwiftUI

/// The play surface. Three things on a sheet: who you are, the board, and one
/// door - which is the whole design, and is why this file is short.
public struct UtttGameScreen: View {
    @StateObject private var model: UtttModel
    public init(model: UtttModel) { _model = StateObject(wrappedValue: model) }

    public var body: some View {
        UtttSheet {
            VStack(spacing: 0) {
                HStack(alignment: .top) {
                    VStack(spacing: 3) {
                        Text("you\nare")
                            .font(.system(size: 9.5, weight: .semibold))
                            .tracking(1.9)
                            .textCase(.uppercase)
                            .multilineTextAlignment(.center)
                            .foregroundStyle(Color(red: 0.54, green: 0.52, blue: 0.47))
                        UtttMarkIcon(mark: model.you, seed: model.seed &+ 4)
                            .frame(width: 34, height: 34)
                    }
                    .frame(width: 46)
                    Spacer()
                    VStack(alignment: .trailing, spacing: 2) {
                        Text(model.headline)
                            .font(.system(size: 19, weight: .bold))
                            .foregroundStyle(Color(red: 0.11, green: 0.106, blue: 0.087))
                        if !model.subline.isEmpty {
                            Text(model.subline)
                                .font(.system(size: 13.5))
                                .foregroundStyle(Color(red: 0.42, green: 0.40, blue: 0.35))
                        }
                    }
                }
                Spacer(minLength: 6)
                UtttBoard(active: model.active, last: model.last,
                          positionKey: model.positionKey,
                          animating: model.animating,
                          onTap: { model.tap(at: $0) })
                Spacer(minLength: 6)
            }
            .padding(13)
        }
    }
}
