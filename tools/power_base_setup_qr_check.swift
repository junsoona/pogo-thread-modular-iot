#!/usr/bin/swift
//
// Test support for tools/gen_power_base_setup_qr.swift (used by tools/test_gen_power_base_setup_qr.py):
// reads a QR code PNG the way a reader would and says whether it holds the expected string, and
// measures how the symbol is drawn. It shares no code with the generator.
//
//     power_base_setup_qr_check --png <file.png> --expect-payload-file <file>
//
// Prints one JSON object of facts. It never prints what the QR code says: the expected string is
// read from a file and only "matches" / "does not match" comes out. Use it with made-up test
// credentials only.
//
// macOS only (Vision, Core Image, ImageIO).

import CoreGraphics
import CoreImage
import Foundation
import ImageIO
import Vision

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(2)
}

var pngPath: String?
var expectPath: String?
var arguments = Array(CommandLine.arguments.dropFirst())
while arguments.count >= 2 {
    switch arguments[0] {
    case "--png": pngPath = arguments[1]
    case "--expect-payload-file": expectPath = arguments[1]
    default: fail("usage: power_base_setup_qr_check --png <file.png> --expect-payload-file <file>")
    }
    arguments.removeFirst(2)
}
guard arguments.isEmpty, let pngPath, let expectPath else {
    fail("usage: power_base_setup_qr_check --png <file.png> --expect-payload-file <file>")
}
guard let expected = FileManager.default.contents(atPath: expectPath).map({ Array($0) }),
      let png = FileManager.default.contents(atPath: pngPath) else { fail("cannot read the input files") }
guard let source = CGImageSourceCreateWithData(png as CFData, nil), let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
    fail("not an image")
}

var facts: [String: Any] = [
    "width": image.width, "height": image.height, "bits_per_pixel": image.bitsPerPixel,
    "color_model": image.colorSpace?.model == .monochrome ? "gray" : "other",
    "has_alpha": ![CGImageAlphaInfo.none, .noneSkipLast, .noneSkipFirst].contains(image.alphaInfo),
    "image_count": CGImageSourceGetCount(source),
]

// Reader 1: Vision.
let request = VNDetectBarcodesRequest()
request.symbologies = [.qr]
try VNImageRequestHandler(cgImage: image, options: [:]).perform([request])
let visionPayloads = (request.results ?? []).map { Array(($0.payloadStringValue ?? "").utf8) }
facts["vision_symbols"] = visionPayloads.count
facts["vision_match"] = visionPayloads.count == 1 && visionPayloads[0] == expected

// Reader 2: Core Image's detector (another decoder than Vision's request).
let detector = CIDetector(ofType: CIDetectorTypeQRCode, context: nil, options: [CIDetectorAccuracy: CIDetectorAccuracyHigh])
let coreImagePayloads = (detector?.features(in: CIImage(cgImage: image)) ?? []).compactMap { ($0 as? CIQRCodeFeature)?.messageString }.map { Array($0.utf8) }
facts["coreimage_symbols"] = coreImagePayloads.count
facts["coreimage_match"] = coreImagePayloads.count == 1 && coreImagePayloads[0] == expected

// How it is drawn: gray values as stored, without colour management.
let width = image.width
let height = image.height
var pixels = [UInt8](repeating: 0, count: width * height)
pixels.withUnsafeMutableBytes { buffer in
    let context = CGContext(
        data: buffer.baseAddress, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width,
        space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue
    )!
    context.interpolationQuality = .none
    context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
}
facts["pixel_values"] = Array(Set(pixels)).sorted().map { Int($0) }

var minX = width, maxX = -1, minY = height, maxY = -1
for y in 0..<height {
    for x in 0..<width where pixels[y * width + x] < 128 {
        minX = min(minX, x); maxX = max(maxX, x); minY = min(minY, y); maxY = max(maxY, y)
    }
}
if maxX >= 0 {
    // The first dark run of the symbol's first row is the top of a finder pattern: seven modules.
    var run = 0
    while minX + run < width, pixels[minY * width + minX + run] < 128 { run += 1 }
    let side = maxX - minX + 1
    facts["symbol_pixels"] = side
    facts["symbol_is_square"] = side == maxY - minY + 1
    if run % 7 == 0, run > 0, side % (run / 7) == 0 {
        let scale = run / 7
        let modules = side / scale
        facts["module_pixels"] = scale
        facts["symbol_modules"] = modules
        // Every module is one flat square.
        var flat = true
        for row in 0..<modules {
            for column in 0..<modules {
                let first = pixels[(minY + row * scale) * width + minX + column * scale]
                for y in 0..<scale {
                    for x in 0..<scale where pixels[(minY + row * scale + y) * width + minX + column * scale + x] != first { flat = false }
                }
            }
        }
        facts["modules_are_flat_squares"] = flat
        facts["quiet_zone_modules"] = [
            "left": Double(minX) / Double(scale), "top": Double(minY) / Double(scale),
            "right": Double(width - 1 - maxX) / Double(scale), "bottom": Double(height - 1 - maxY) / Double(scale),
        ]
    }
}

let json = try JSONSerialization.data(withJSONObject: facts, options: [.sortedKeys])
print(String(decoding: json, as: UTF8.self))
