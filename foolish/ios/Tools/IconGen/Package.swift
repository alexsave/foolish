// swift-tools-version:5.9
// IconGen — renders the app icon (§16.F2): the procedural fern (the §5.3 card
// back) on the table color at 1024px, one blessed seed (ICON_SEED). Run:
//   swift run --package-path ios/Tools/IconGen icongen \
//     ios/FoolishApp/Assets.xcassets/AppIcon.appiconset/AppIcon.png
// then point AppIcon.appiconset/Contents.json at AppIcon.png. macOS only
// (CoreGraphics + ImageIO).
import PackageDescription

let package = Package(
    name: "IconGen",
    platforms: [.macOS(.v12)],
    targets: [.executableTarget(name: "icongen", path: "Sources/icongen")]
)
