// GenerateTextures.swift — the BUILD-TIME half of the wool/wood materials.
//
// Runs on the Mac, calls the very same WoolTexture/WoodTexture generators the
// app used to run on launch, and writes the results to
// ios/FoolishKit/Resources/. The shipping app then loads those images
// (FTextures) and
// generates zero procedural pixels — which is the point: an iMessage extension
// is memory- and watchdog-capped far below an app, and round-5 proved that a
// big procedural render there is a dark, empty panel on a real phone even when
// the simulator looks perfect.
//
// This is NOT compiled into any target. It is compiled ad hoc by
// ios/Tools/regenerate_textures.sh, which passes the two generator sources
// alongside it — so there is ONE generator, not a build-time copy that drifts
// from the runtime one. (The generators are deliberately UIKit-free for exactly
// this reason.)
//
// Usage:  ios/Tools/regenerate_textures.sh [output-dir]
// then commit the regenerated JPEGs.

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

// MARK: - output

/// Write a baked texture as JPEG.
///
/// JPEG, not PNG, and the reason is measured: a wool weave is high-frequency
/// noise, which is the worst case for PNG's predictors. The same 592x1280
/// weave is 2071 KB as PNG and 590 KB at quality 0.85 — 3.5x smaller in a
/// download for a texture that is then MAGNIFIED 2.3x on screen (see
/// WoolTexture.pointsPerTexel), so a JPEG block is a third of a display pixel
/// and cannot be resolved. Nothing here is line art or has hard edges; the
/// wood is smoother still.
func writeJPEG(_ image: CGImage, to url: URL, quality: Double) {
    guard let dest = CGImageDestinationCreateWithURL(
        url as CFURL, UTType.jpeg.identifier as CFString, 1, nil) else {
        FileHandle.standardError.write(Data("cannot create \(url.path)\n".utf8))
        exit(1)
    }
    let options: [CFString: Any] = [kCGImageDestinationLossyCompressionQuality: quality]
    CGImageDestinationAddImage(dest, image, options as CFDictionary)
    guard CGImageDestinationFinalize(dest) else {
        FileHandle.standardError.write(Data("cannot write \(url.path)\n".utf8))
        exit(1)
    }
}

func report(_ name: String, _ url: URL, _ image: CGImage, seconds: Double) {
    let bytes = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? Int ?? 0
    let kb = Double(bytes) / 1024
    print(String(format: "  %-18@  %dx%d px  %.0f KB  (%.1fs)",
                 name as NSString, image.width, image.height, kb, seconds))
}

// MARK: - main

@main
struct GenerateTextures {
static func main() {
    let args = CommandLine.arguments
    let outDir = URL(fileURLWithPath: args.count > 1 ? args[1] : FileManager.default.currentDirectoryPath,
                     isDirectory: true)
    try? FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)

    print("Baking textures into \(outDir.path)")

    // ---- wool -------------------------------------------------------------
    // Rendered on the web's landscape canvas (every constant in the generator is
    // in pixels of that shape), then cropped to the portrait sliver a phone can
    // actually show. Cropping is what keeps the file - and the decoded bitmap the
    // extension holds - about a third of the size for zero visible difference.
    //
    // One pass per entry in `WoolTexture.bakes` (light, dark-green, dark-navy).
    // The list is the generator's, not this tool's: a new look is a palette plus
    // a line there, and nothing here changes.
    for bake in WoolTexture.bakes {
        let t0 = Date()
        guard let full = WoolTexture.renderCGImage(w: WoolTexture.renderCanvas.w,
                                                   h: WoolTexture.renderCanvas.h,
                                                   palette: bake.palette) else {
            FileHandle.standardError.write(Data("wool render failed: \(bake.name)\n".utf8)); exit(1)
        }
        let c = WoolTexture.shippedCrop
        guard let cropped = full.cropping(to: CGRect(x: c.x, y: c.y, width: c.w, height: c.h)) else {
            FileHandle.standardError.write(Data("wool crop failed: \(bake.name)\n".utf8)); exit(1)
        }
        let url = outDir.appendingPathComponent("\(bake.name).jpg")
        writeJPEG(cropped, to: url, quality: 0.85)
        report(bake.name, url, cropped, seconds: Date().timeIntervalSince(t0))
    }

    // ---- wood -------------------------------------------------------------
    // No crop: the canvas IS the swatch, sized so the largest wood surface in the
    // app is a sub-rectangle of it and nothing ever tiles.
    for bake in WoodTexture.bakes {
        let t0 = Date()
        guard let wood = WoodTexture.renderCGImage(w: WoodTexture.renderCanvas.w,
                                                   h: WoodTexture.renderCanvas.h,
                                                   palette: bake.palette) else {
            FileHandle.standardError.write(Data("wood render failed: \(bake.name)\n".utf8)); exit(1)
        }
        let url = outDir.appendingPathComponent("\(bake.name).jpg")
        writeJPEG(wood, to: url, quality: 0.9)
        report(bake.name, url, wood, seconds: Date().timeIntervalSince(t0))
    }

    print("Done. Commit the images; the app loads them through FTextures.")
}
}
