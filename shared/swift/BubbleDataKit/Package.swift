// swift-tools-version: 5.9
// BubbleDataKit: a board of three-state cells drawn as a picture for a
// Messages app bubble, and read back. The format and every decision are in C
// (Sources/CBubbleData); the Swift target is a thin face over it.
import PackageDescription

let package = Package(
    name: "BubbleDataKit",
    platforms: [.iOS(.v17), .macOS(.v14)],
    products: [
        .library(name: "BubbleDataKit", targets: ["BubbleDataKit"]),
    ],
    targets: [
        .target(name: "CBubbleData"),
        .target(name: "BubbleDataKit", dependencies: ["CBubbleData"]),
        .testTarget(name: "BubbleDataKitTests", dependencies: ["BubbleDataKit", "CBubbleData"]),
    ]
)
