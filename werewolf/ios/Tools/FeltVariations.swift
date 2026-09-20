// FeltVariations.swift — DEV ONLY, never part of any target.
//
// Bakes a set of candidate baizes so they can be judged on a phone at real
// scale. Compiled ad hoc by ios/Tools/felt_variations.sh alongside the real
// FeltTexture.swift, so every candidate is the SHIPPING generator with
// different palette numbers — not a mock-up that could flatter itself.
//
// Usage:  ios/Tools/felt_variations.sh <out-dir>

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let candidates: [(String, FeltTexture.Palette)] = [
    // A: the current bake — band-limited grain, medium cloud.
    ("felt-A", FeltTexture.Palette(baseR: 20, baseG: 56, baseB: 39,
        cloud: 8, grain: 6.0, grainScale: 2.4,
        tiltR: 0.85, tiltG: 1.0, tiltB: 0.72, cloudX: 62, cloudY: 48,
        fallbackHex: 0x143827)),
    // B: finer nap — more tooth, smaller fibre.
    ("felt-B", FeltTexture.Palette(baseR: 20, baseG: 56, baseB: 39,
        cloud: 8, grain: 5.0, grainScale: 1.7,
        tiltR: 0.85, tiltG: 1.0, tiltB: 0.72, cloudX: 62, cloudY: 48,
        fallbackHex: 0x143827)),
    // C: softer — calmer nap, closer to solid.
    ("felt-C", FeltTexture.Palette(baseR: 20, baseG: 56, baseB: 39,
        cloud: 7, grain: 3.5, grainScale: 3.6,
        tiltR: 0.85, tiltG: 1.0, tiltB: 0.72, cloudX: 62, cloudY: 48,
        fallbackHex: 0x143827)),
    // D: "zoomed out" — the mottling itself is smaller and tighter.
    ("felt-D", FeltTexture.Palette(baseR: 20, baseG: 56, baseB: 39,
        cloud: 8, grain: 5.0, grainScale: 2.4,
        tiltR: 0.85, tiltG: 1.0, tiltB: 0.72, cloudX: 26, cloudY: 20,
        fallbackHex: 0x143827)),
    // E: nearly solid — the least texture that is still not a flat fill.
    ("felt-E", FeltTexture.Palette(baseR: 20, baseG: 56, baseB: 39,
        cloud: 5, grain: 2.5, grainScale: 3.0,
        tiltR: 0.85, tiltG: 1.0, tiltB: 0.72, cloudX: 62, cloudY: 48,
        fallbackHex: 0x143827)),
    // F: broad cloud — big soft pools of light, minimal nap.
    ("felt-F", FeltTexture.Palette(baseR: 20, baseG: 56, baseB: 39,
        cloud: 11, grain: 3.0, grainScale: 3.0,
        tiltR: 0.85, tiltG: 1.0, tiltB: 0.72, cloudX: 130, cloudY: 100,
        fallbackHex: 0x143827)),
]

@main
struct FeltVariations {
static func main() {
    let out = URL(fileURLWithPath: CommandLine.arguments.count > 1
                  ? CommandLine.arguments[1] : FileManager.default.currentDirectoryPath,
                  isDirectory: true)
    try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)

    for (name, palette) in candidates {
        guard let img = FeltTexture.renderCGImage(w: FeltTexture.renderCanvas.w,
                                                  h: FeltTexture.renderCanvas.h,
                                                  palette: palette) else {
            FileHandle.standardError.write(Data("render failed: \(name)\n".utf8)); exit(1)
        }
        let url = out.appendingPathComponent("\(name).jpg")
        guard let dest = CGImageDestinationCreateWithURL(
            url as CFURL, UTType.jpeg.identifier as CFString, 1, nil) else { exit(1) }
        CGImageDestinationAddImage(dest, img, [kCGImageDestinationLossyCompressionQuality: 0.9] as CFDictionary)
        _ = CGImageDestinationFinalize(dest)
        print("  \(name).jpg")
    }
}
}
