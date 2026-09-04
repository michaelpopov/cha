import AppKit

guard CommandLine.arguments.count == 2 else {
    fputs("usage: make-icon.swift <iconset-directory>\n", stderr)
    exit(2)
}

let output = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try FileManager.default.createDirectory(
    at: output,
    withIntermediateDirectories: true)

let representations = [
    (name: "icon_16x16.png", pixels: 16),
    (name: "icon_16x16@2x.png", pixels: 32),
    (name: "icon_32x32.png", pixels: 32),
    (name: "icon_32x32@2x.png", pixels: 64),
    (name: "icon_128x128.png", pixels: 128),
    (name: "icon_128x128@2x.png", pixels: 256),
    (name: "icon_256x256.png", pixels: 256),
    (name: "icon_256x256@2x.png", pixels: 512),
    (name: "icon_512x512.png", pixels: 512),
    (name: "icon_512x512@2x.png", pixels: 1024),
]

for representation in representations {
    let size = CGFloat(representation.pixels)

    // Drawn straight into a bitmap of the named pixel size. An NSImage would
    // render at the build Mac's display scale instead, and iconutil files each
    // image by its pixel size rather than by its name, so a Retina build host
    // would silently drop the smallest representations.
    guard let bitmap = NSBitmapImageRep(
        bitmapDataPlanes: nil,
        pixelsWide: representation.pixels,
        pixelsHigh: representation.pixels,
        bitsPerSample: 8,
        samplesPerPixel: 4,
        hasAlpha: true,
        isPlanar: false,
        colorSpaceName: .deviceRGB,
        bytesPerRow: 0,
        bitsPerPixel: 0) else {
        throw CocoaError(.fileWriteUnknown)
    }
    bitmap.size = NSSize(width: size, height: size)
    guard let context = NSGraphicsContext(bitmapImageRep: bitmap) else {
        throw CocoaError(.fileWriteUnknown)
    }

    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = context
    context.shouldAntialias = true

    let margin = size * 0.04
    let background = NSBezierPath(
        roundedRect: NSRect(
            x: margin,
            y: margin,
            width: size - margin * 2,
            height: size - margin * 2),
        xRadius: size * 0.22,
        yRadius: size * 0.22)
    NSColor(calibratedRed: 17 / 255, green: 24 / 255, blue: 39 / 255, alpha: 1).setFill()
    background.fill()

    let mark = NSBezierPath()
    mark.move(to: NSPoint(x: size * 44 / 64, y: size * 44 / 64))
    mark.curve(
        to: NSPoint(x: size * 14 / 64, y: size * 32 / 64),
        controlPoint1: NSPoint(x: size * 29 / 64, y: size * 56 / 64),
        controlPoint2: NSPoint(x: size * 14 / 64, y: size * 44 / 64))
    mark.curve(
        to: NSPoint(x: size * 44 / 64, y: size * 20 / 64),
        controlPoint1: NSPoint(x: size * 14 / 64, y: size * 20 / 64),
        controlPoint2: NSPoint(x: size * 29 / 64, y: size * 8 / 64))
    mark.lineWidth = max(1.5, size * 7 / 64)
    mark.lineCapStyle = .round
    NSColor.white.setStroke()
    mark.stroke()

    NSGraphicsContext.restoreGraphicsState()

    guard let png = bitmap.representation(using: .png, properties: [:]) else {
        throw CocoaError(.fileWriteUnknown)
    }
    try png.write(to: output.appendingPathComponent(representation.name))
}
