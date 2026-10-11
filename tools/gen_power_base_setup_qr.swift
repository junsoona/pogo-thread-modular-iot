#!/usr/bin/swift
//
// Makes the setup QR code of ONE Power Base from the setup code it already has
// (docs/power-base.md "QR 설정 코드").
//
// The QR code holds one ASCII string, 53 bytes:
//
//     POGO-PB:1:<eui64>:<setup_code>
//
// <eui64> is the board's EUI-64 as 16 lower-case hex digits and <setup_code> the 26 characters
// that tools/gen_power_base_ble_config.py wrote into the board's setup-code file. The app reads
// such a code into the setup code field of the board the user picked; it starts nothing.
//
// What this tool does NOT do: it draws no new code, changes no cfg image and touches no board.
// A board keeps the code it was prepared with.
//
// The PNG is the setup code. Whoever sees it can enroll, move or confirm the removal of that
// board. So:
//   - the code is read from a file (mode 0600, yours, a regular file, no symbolic link) and is
//     never taken from the command line or the environment, and never printed;
//   - the PNG is written with mode 0600 into a directory that only you can enter (it is created
//     with mode 0700 when it does not exist), never inside a Git work tree, and never over an
//     existing file;
//   - nothing is left behind when anything fails.
//
// macOS only (13 or later): it uses Core Image to lay out the symbol, ImageIO to write the PNG and
// Vision to read the finished PNG back before it gets its name. Nothing is installed.
//
//     swift tools/gen_power_base_setup_qr.swift \
//         --eui64 <16 hex digits of THIS board> \
//         --setup-code-file "$HOME/pogo-setup-codes/<eui64>.setup-code" \
//         --out "$HOME/pogo-setup-codes/<eui64>.setup-qr.png"
//
// or compiled once: xcrun swiftc -O tools/gen_power_base_setup_qr.swift -o <somewhere outside the repository>
//
// Exit status: 0 written; 1 refused (nothing was written); 2 usage.

import CoreGraphics
import CoreImage
import Foundation
import ImageIO
import UniformTypeIdentifiers
import Vision

let formatPrefix = "POGO-PB:1:"
let payloadLength = 53
/// Pixels per module. A whole number, so every module is a square of identical pixels.
let moduleScale = 12
/// Light modules around the symbol, on every side (the QR standard asks for four).
let quietZoneModules = 4
/// ISO/IEC 18004 level M: about 15 % of the symbol may be damaged.
let correctionLevel = "M"

struct Refusal: Error {
    let message: String
    init(_ message: String) { self.message = message }
}

func fail(_ status: Int32, _ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(status)
}

let usage = """
    usage: gen_power_base_setup_qr.swift --eui64 <16 hex digits> --setup-code-file <path> --out <path.png>

      --eui64             this board's EUI-64 (the one its setup code was made for)
      --setup-code-file   the board's existing setup-code file (mode 0600); its content is never shown
      --out               where to write the QR code PNG (mode 0600); must not exist, must not be in a Git work tree

    The setup code itself is never accepted as an argument.
    """

// MARK: - Arguments

struct Arguments {
    var eui64: String
    var setupCodeFile: String
    var out: String
}

func parseArguments(_ arguments: [String]) -> Arguments {
    var values: [String: String] = [:]
    var index = 0
    while index < arguments.count {
        let name = arguments[index]
        guard ["--eui64", "--setup-code-file", "--out"].contains(name) else {
            if name == "-h" || name == "--help" {
                print(usage)
                exit(0)
            }
            // Never echoed: a mistyped invocation may have a code where a flag should be.
            fail(2, "unknown argument (not shown)\n" + usage)
        }
        guard index + 1 < arguments.count else { fail(2, "\(name) needs a value\n" + usage) }
        guard values[name] == nil else { fail(2, "\(name) given twice\n" + usage) }
        values[name] = arguments[index + 1]
        index += 2
    }
    guard let eui64 = values["--eui64"], let file = values["--setup-code-file"], let out = values["--out"] else {
        fail(2, usage)
    }
    return Arguments(eui64: eui64, setupCodeFile: file, out: out)
}

// MARK: - The two fields

/// The rule of tools/gen_power_base_ble_config.py: exactly 16 hex digits, either case, neither all
/// zeros nor all ones; lower case from here on.
func normalizedEUI64(_ text: String) throws -> String {
    let bytes = Array(text.utf8)
    guard bytes.count == 16 else { throw Refusal("--eui64 must be exactly 16 hex digits (the board's EUI-64)") }
    var lower: [UInt8] = []
    for byte in bytes {
        switch byte {
        case UInt8(ascii: "0")...UInt8(ascii: "9"), UInt8(ascii: "a")...UInt8(ascii: "f"): lower.append(byte)
        case UInt8(ascii: "A")...UInt8(ascii: "F"): lower.append(byte + 32)
        default: throw Refusal("--eui64 must be exactly 16 hex digits (the board's EUI-64)")
        }
    }
    if lower.allSatisfy({ $0 == UInt8(ascii: "0") }) || lower.allSatisfy({ $0 == UInt8(ascii: "f") }) {
        throw Refusal("--eui64 is all zeros or all ones, not a real EUI-64")
    }
    return String(decoding: lower, as: UTF8.self)
}

let base32Alphabet = Array("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567".utf8)

/// 26 characters of upper-case RFC 4648 base32 without padding that are the canonical encoding
/// of 16 bytes: decoded and encoded again, they must come out the same.
func isCanonicalSetupCode(_ characters: [UInt8]) -> Bool {
    guard characters.count == 26 else { return false }
    var decoded: [UInt8] = []
    var buffer: UInt32 = 0
    var bits = 0
    for character in characters {
        guard let value = base32Alphabet.firstIndex(of: character) else { return false }
        buffer = (buffer << 5 | UInt32(value)) & 0xFFF
        bits += 5
        if bits >= 8 {
            bits -= 8
            decoded.append(UInt8(truncatingIfNeeded: buffer >> UInt32(bits)))
        }
    }
    guard decoded.count == 16 else { return false }
    var encoded: [UInt8] = []
    buffer = 0
    bits = 0
    for byte in decoded {
        buffer = (buffer << 8 | UInt32(byte)) & 0x1FFF
        bits += 8
        while bits >= 5 {
            bits -= 5
            encoded.append(base32Alphabet[Int(buffer >> UInt32(bits) & 0x1F)])
        }
    }
    if bits > 0 { encoded.append(base32Alphabet[Int(buffer << UInt32(5 - bits) & 0x1F)]) }
    return encoded == characters
}

// MARK: - Paths

func absolutePath(_ path: String) -> String {
    let url = URL(fileURLWithPath: path, relativeTo: URL(fileURLWithPath: FileManager.default.currentDirectoryPath, isDirectory: true))
    return url.standardizedFileURL.path
}

/// The directory with every symbolic link resolved (it must exist).
func resolvedDirectory(_ path: String) -> String? {
    guard let pointer = realpath(path, nil) else { return nil }
    defer { free(pointer) }
    return String(cString: pointer)
}

/// The top of the Git work tree `directory` is in, if it is in one. A secret does not belong in
/// a work tree: one `git add` away from being published.
func gitWorkTree(containing directory: String) -> String? {
    var current = directory
    while true {
        var info = stat()
        if lstat(current + "/.git", &info) == 0 { return current }
        let parent = (current as NSString).deletingLastPathComponent
        if parent == current || parent.isEmpty { return nil }
        current = parent
    }
}

/// The nearest directory that exists at or above `path`.
func nearestExistingDirectory(_ path: String) -> String {
    var current = path
    while true {
        var info = stat()
        if stat(current, &info) == 0, info.st_mode & S_IFMT == S_IFDIR { return current }
        let parent = (current as NSString).deletingLastPathComponent
        if parent == current || parent.isEmpty { return "/" }
        current = parent
    }
}

// MARK: - Reading the setup code

/// The code in an existing setup-code file. Never echoes the file's content, in any refusal.
func readSetupCode(at path: String) throws -> [UInt8] {
    let absolute = absolutePath(path)
    var link = stat()
    guard lstat(absolute, &link) == 0 else { throw Refusal("--setup-code-file not found: \(absolute)") }
    guard link.st_mode & S_IFMT != S_IFLNK else {
        throw Refusal("--setup-code-file is a symbolic link; give the file itself")
    }
    let directory = (absolute as NSString).deletingLastPathComponent
    if let resolved = resolvedDirectory(directory), gitWorkTree(containing: resolved) != nil {
        throw Refusal("--setup-code-file is inside a Git work tree; a setup code must be kept outside every repository")
    }
    // O_NOFOLLOW: not a link swapped in after the check above. O_NONBLOCK: a FIFO does not hang.
    let descriptor = open(absolute, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
    guard descriptor >= 0 else { throw Refusal("--setup-code-file cannot be opened: \(absolute)") }
    defer { close(descriptor) }
    // Everything below is about the file that was opened, not about the name.
    var info = stat()
    guard fstat(descriptor, &info) == 0 else { throw Refusal("--setup-code-file cannot be inspected") }
    guard info.st_mode & S_IFMT == S_IFREG else { throw Refusal("--setup-code-file is not a regular file") }
    guard info.st_uid == getuid() else { throw Refusal("--setup-code-file belongs to another user") }
    guard info.st_mode & 0o077 == 0 else {
        throw Refusal("--setup-code-file is accessible to group/others (expected mode 0600)")
    }
    // 26 characters and at most the one line feed the prepare tool writes.
    guard info.st_size >= 26, info.st_size <= 27 else {
        throw Refusal("--setup-code-file does not hold a setup code (26 characters A-Z/2-7 and at most one line feed; its content is not shown)")
    }
    var content = [UInt8](repeating: 0, count: 28)
    let count = content.withUnsafeMutableBytes { read(descriptor, $0.baseAddress, 28) }
    guard count == Int(info.st_size) else { throw Refusal("--setup-code-file changed while it was read") }
    content.removeSubrange(count...)
    if content.count == 27 {
        guard content[26] == 0x0A else {
            throw Refusal("--setup-code-file does not hold a setup code (26 characters A-Z/2-7 and at most one line feed; its content is not shown)")
        }
        content.removeLast()
    }
    guard content.allSatisfy({ base32Alphabet.contains($0) }) else {
        throw Refusal("--setup-code-file does not hold a setup code (26 characters A-Z/2-7 and at most one line feed; its content is not shown)")
    }
    guard isCanonicalSetupCode(content) else {
        throw Refusal("--setup-code-file holds a non-canonical setup code (its content is not shown)")
    }
    return content
}

// MARK: - The symbol

struct Symbol {
    /// Modules per side, without the quiet zone.
    let size: Int
    /// Row by row from the top; `true` is a dark module.
    let dark: [Bool]
}

/// The QR symbol of `payload`, as Core Image lays it out, one value per module.
func makeSymbol(_ payload: [UInt8]) throws -> Symbol {
    guard let filter = CIFilter(name: "CIQRCodeGenerator") else { throw Refusal("Core Image has no QR code generator here") }
    filter.setValue(Data(payload), forKey: "inputMessage")
    filter.setValue(correctionLevel, forKey: "inputCorrectionLevel")
    guard let image = filter.outputImage else { throw Refusal("the QR code could not be generated") }
    let extent = image.extent.integral
    let width = Int(extent.width)
    let height = Int(extent.height)
    guard width == height, width > 0, width < 400 else { throw Refusal("the generated QR code has an unexpected size") }
    var pixels = [UInt8](repeating: 0, count: width * height)
    let context = CIContext(options: [.workingColorSpace: NSNull(), .outputColorSpace: NSNull()])
    // One byte per module, read straight out of the generator: no interpolation, no colour management.
    pixels.withUnsafeMutableBytes { buffer in
        context.render(image, toBitmap: buffer.baseAddress!, rowBytes: width, bounds: extent, format: .L8, colorSpace: nil)
    }
    guard pixels.allSatisfy({ $0 < 16 || $0 > 239 }) else { throw Refusal("the generated QR code is not black and white") }
    let dark = pixels.map { $0 < 128 }
    // The generator may add its own margin: keep the symbol only.
    var minX = width, maxX = -1, minY = height, maxY = -1
    for y in 0..<height {
        for x in 0..<width where dark[y * width + x] {
            minX = min(minX, x); maxX = max(maxX, x); minY = min(minY, y); maxY = max(maxY, y)
        }
    }
    let size = maxX - minX + 1
    // A QR symbol has 21, 25, 29, … modules per side.
    guard maxX >= 0, size == maxY - minY + 1, size >= 21, (size - 17) % 4 == 0 else {
        throw Refusal("the generated QR code has an unexpected shape")
    }
    var modules: [Bool] = []
    modules.reserveCapacity(size * size)
    for y in minY...maxY {
        for x in minX...maxX { modules.append(dark[y * width + x]) }
    }
    // Which way up: a QR symbol has its three finder patterns top left, top right and bottom
    // left, and none bottom right. Rows that came out bottom first are turned over; anything
    // else is refused rather than written as a mirrored symbol.
    func hasFinder(_ rows: [Bool], row: Int, column: Int) -> Bool {
        for y in 0..<7 {
            for x in 0..<7 {
                let ring = max(abs(y - 3), abs(x - 3))
                if rows[(row + y) * size + column + x] != (ring != 2) { return false }
            }
        }
        return true
    }
    func corners(_ rows: [Bool]) -> [Bool] {
        [hasFinder(rows, row: 0, column: 0), hasFinder(rows, row: 0, column: size - 7),
         hasFinder(rows, row: size - 7, column: 0), hasFinder(rows, row: size - 7, column: size - 7)]
    }
    if corners(modules) == [true, false, true, true] {
        modules = stride(from: size - 1, through: 0, by: -1).flatMap { Array(modules[($0 * size)..<(($0 + 1) * size)]) }
    }
    guard corners(modules) == [true, true, true, false] else { throw Refusal("the generated QR code has an unexpected shape") }
    return Symbol(size: size, dark: modules)
}

/// An 8-bit grayscale PNG: every module `moduleScale` pixels square, pure black on pure white,
/// with the quiet zone around it.
func makePNG(_ symbol: Symbol) throws -> Data {
    let side = (symbol.size + 2 * quietZoneModules) * moduleScale
    var pixels = [UInt8](repeating: 255, count: side * side)
    for row in 0..<symbol.size {
        for column in 0..<symbol.size where symbol.dark[row * symbol.size + column] {
            let x0 = (column + quietZoneModules) * moduleScale
            let y0 = (row + quietZoneModules) * moduleScale
            for y in y0..<(y0 + moduleScale) {
                for x in x0..<(x0 + moduleScale) { pixels[y * side + x] = 0 }
            }
        }
    }
    guard let provider = CGDataProvider(data: Data(pixels) as CFData),
          let image = CGImage(
              width: side, height: side, bitsPerComponent: 8, bitsPerPixel: 8, bytesPerRow: side,
              space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
              provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent
          ) else { throw Refusal("the QR code image could not be made") }
    let data = NSMutableData()
    guard let destination = CGImageDestinationCreateWithData(data, UTType.png.identifier as CFString, 1, nil) else {
        throw Refusal("the PNG could not be made")
    }
    CGImageDestinationAddImage(destination, image, nil)
    guard CGImageDestinationFinalize(destination) else { throw Refusal("the PNG could not be made") }
    return try withoutMetadata(data as Data)
}

/// The PNG with only the chunks a picture needs. ImageIO adds an EXIF chunk of its own; a file
/// that is a secret carries nothing but the picture.
func withoutMetadata(_ png: Data) throws -> Data {
    let bytes = [UInt8](png)
    let signature: [UInt8] = [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]
    guard bytes.count > 8, Array(bytes[..<8]) == signature else { throw Refusal("the PNG could not be made") }
    var output = signature
    var offset = 8
    var kinds: [String] = []
    while offset + 12 <= bytes.count {
        let length = Int(bytes[offset]) << 24 | Int(bytes[offset + 1]) << 16 | Int(bytes[offset + 2]) << 8 | Int(bytes[offset + 3])
        let end = offset + 12 + length
        guard end <= bytes.count else { throw Refusal("the PNG could not be made") }
        let kind = String(decoding: bytes[(offset + 4)..<(offset + 8)], as: UTF8.self)
        if ["IHDR", "IDAT", "IEND"].contains(kind) {
            output.append(contentsOf: bytes[offset..<end])
            kinds.append(kind)
        }
        offset = end
    }
    guard offset == bytes.count, kinds.first == "IHDR", kinds.last == "IEND", kinds.contains("IDAT") else {
        throw Refusal("the PNG could not be made")
    }
    return Data(output)
}

/// Reads the finished PNG as a camera app would: exactly one QR code, saying exactly `payload`.
func verifyPNG(_ png: Data, says payload: [UInt8]) throws {
    guard let source = CGImageSourceCreateWithData(png as CFData, nil), let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
        throw Refusal("the finished PNG cannot be read back")
    }
    let request = VNDetectBarcodesRequest()
    request.symbologies = [.qr]
    do {
        try VNImageRequestHandler(cgImage: image, options: [:]).perform([request])
    } catch {
        throw Refusal("the finished PNG could not be checked (Vision failed)")
    }
    let found = (request.results ?? []).map { Array(($0.payloadStringValue ?? "").utf8) }
    guard found.count == 1, found[0] == payload else {
        // Says nothing about what was read instead.
        throw Refusal("the finished PNG does not read back as this board's code; nothing was written")
    }
}

// MARK: - Writing

/// The unfinished file, for the signal handler: an interrupted run leaves no half-written code.
var partialPathForSignal: UnsafeMutablePointer<CChar>?

func removePartialOnSignal() {
    for number in [SIGINT, SIGTERM, SIGHUP] {
        signal(number) { _ in
            if let path = partialPathForSignal { unlink(path) }
            _exit(130)
        }
    }
}

/// The directory the PNG goes into: an existing one must be yours and closed to everyone else;
/// a missing one is created with mode 0700. Returns the directories that were created (deepest last).
func prepareOutputDirectory(_ directory: String) throws -> [String] {
    var info = stat()
    if lstat(directory, &info) == 0 {
        guard info.st_mode & S_IFMT != S_IFLNK else { throw Refusal("the directory of --out is a symbolic link: \(directory)") }
        guard info.st_mode & S_IFMT == S_IFDIR else { throw Refusal("the directory of --out is not a directory: \(directory)") }
        guard info.st_uid == getuid() else { throw Refusal("the directory of --out belongs to another user: \(directory)") }
        guard info.st_mode & 0o077 == 0 else {
            throw Refusal("the directory of --out is accessible to group/others (expected mode 0700): \(directory). " +
                          "Choose a directory that only you can enter, or one that does not exist yet (it is created with mode 0700)")
        }
        return []
    }
    var missing: [String] = []
    var current = directory
    while lstat(current, &info) != 0 {
        missing.append(current)
        let parent = (current as NSString).deletingLastPathComponent
        guard parent != current, !parent.isEmpty else { break }
        current = parent
    }
    var created: [String] = []
    for path in missing.reversed() {
        guard mkdir(path, 0o700) == 0 else {
            for made in created.reversed() { rmdir(made) }
            throw Refusal("the directory of --out could not be created: \(path)")
        }
        created.append(path)
        // Not left to the umask.
        chmod(path, 0o700)
    }
    return created
}

func run() throws {
    let arguments = parseArguments(Array(CommandLine.arguments.dropFirst()))
    let eui64 = try normalizedEUI64(arguments.eui64)

    let out = absolutePath(arguments.out)
    let name = (out as NSString).lastPathComponent
    let directory = (out as NSString).deletingLastPathComponent
    guard name.lowercased().hasSuffix(".png"), name.count > 4 else { throw Refusal("--out must name a .png file") }
    // Decided before anything is read or made.
    guard let anchor = resolvedDirectory(nearestExistingDirectory(directory)) else { throw Refusal("--out cannot be resolved: \(out)") }
    if let workTree = gitWorkTree(containing: anchor) {
        throw Refusal("--out is inside a Git work tree (\(workTree)); the QR code is a secret like the setup code and must be " +
                      "kept outside every repository (e.g. a 0700 directory in your home)")
    }
    var existing = stat()
    guard lstat(out, &existing) != 0 else {
        throw Refusal("--out already exists: \(out). Nothing is overwritten; remove it yourself or choose another name")
    }

    let code = try readSetupCode(at: arguments.setupCodeFile)
    let payload = Array(formatPrefix.utf8) + Array(eui64.utf8) + [UInt8(ascii: ":")] + code
    guard payload.count == payloadLength, payload.allSatisfy({ $0 < 0x80 }) else { throw Refusal("internal error: unexpected payload shape") }

    let png = try makePNG(makeSymbol(payload))
    try verifyPNG(png, says: payload)

    let created = try prepareOutputDirectory(directory)
    let partial = directory + "/." + name + ".partial-\(getpid())"
    var finished = false
    partialPathForSignal = strdup(partial)
    removePartialOnSignal()
    defer {
        partialPathForSignal = nil
        if !finished {
            unlink(partial)
            for made in created.reversed() { rmdir(made) }
        }
    }
    // The directory may have been a missing one a moment ago: check where it really is, again.
    guard let resolved = resolvedDirectory(directory), gitWorkTree(containing: resolved) == nil else {
        throw Refusal("--out is inside a Git work tree; nothing was written")
    }
    let descriptor = open(partial, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)
    guard descriptor >= 0 else { throw Refusal("the QR code file could not be created in \(directory)") }
    var written = 0
    let complete = png.withUnsafeBytes { buffer -> Bool in
        while written < buffer.count {
            let count = write(descriptor, buffer.baseAddress! + written, buffer.count - written)
            guard count > 0 else { return false }
            written += count
        }
        return true
    }
    let modeSet = fchmod(descriptor, 0o600) == 0
    let synced = fsync(descriptor) == 0
    close(descriptor)
    guard complete, modeSet, synced else { throw Refusal("the QR code file could not be written completely; nothing was kept") }
    // RENAME_EXCL: gets its name only if nothing has taken that name meanwhile.
    guard renamex_np(partial, out, UInt32(RENAME_EXCL)) == 0 else {
        throw Refusal("--out already exists: \(out). Nothing is overwritten")
    }
    finished = true

    print("wrote \(out) (\(png.count) bytes, mode 0600): Power Base setup QR code, format POGO-PB version 1, for EUI-64 \(eui64)")
    print("the QR code's content is not displayed. This PNG IS the setup code of that board: keep it as you keep " +
          "the setup-code file, and do not put it in a repository, a document or a chat")
    print("no setup code was drawn and no cfg image or board was changed")
}

do {
    try run()
} catch let refusal as Refusal {
    fail(1, "refusing to make a QR code: " + refusal.message)
} catch {
    fail(1, "refusing to make a QR code: unexpected error")
}
