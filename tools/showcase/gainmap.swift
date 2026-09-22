// HDR gain-map JPEG from the emulator's SDR and HDR renders of one frame.
//
//   swift gainmap.swift SDR.png HDR-PQ.png OUT.jpg [QUALITY]
//
// SDR.png is the SDR render (8-bit sRGB). HDR-PQ.png is the HDR render of the
// same frame as a 16-bit PNG in BT.2020 PQ. Core Image writes SDR.png as the
// JPEG base image and computes an ISO 21496-1 gain map toward HDR-PQ.png
// (CIImageRepresentationOption.hdrImage, macOS 15 or later). Browsers on a
// display with headroom show the HDR render; everything else shows the SDR
// render. Exit status 3 means this macOS is too old.
import CoreImage
import Foundation

func fail(_ message: String, _ status: Int32 = 1) -> Never {
    FileHandle.standardError.write(Data(("gainmap: " + message + "\n").utf8))
    exit(status)
}

let args = CommandLine.arguments
guard args.count == 4 || args.count == 5 else {
    fail("usage: gainmap.swift SDR.png HDR-PQ.png OUT.jpg [QUALITY]", 2)
}
guard #available(macOS 15, *) else { fail("needs macOS 15 or later", 3) }
let quality = args.count == 5 ? (Double(args[4]) ?? 0.9) : 0.9
guard let srgb = CGColorSpace(name: CGColorSpace.sRGB),
      let pq = CGColorSpace(name: CGColorSpace.itur_2100_PQ) else { fail("colour spaces unavailable") }
guard let sdr = CIImage(contentsOf: URL(fileURLWithPath: args[1]), options: [.colorSpace: srgb]) else {
    fail("cannot read \(args[1])")
}
guard let hdr = CIImage(contentsOf: URL(fileURLWithPath: args[2]), options: [.colorSpace: pq]) else {
    fail("cannot read \(args[2])")
}
guard sdr.extent == hdr.extent else { fail("sizes differ: \(sdr.extent.size) and \(hdr.extent.size)") }
let options: [CIImageRepresentationOption: Any] = [
    .hdrImage: hdr,
    CIImageRepresentationOption(rawValue: kCGImageDestinationLossyCompressionQuality as String): quality,
]
do {
    try CIContext().writeJPEGRepresentation(of: sdr, to: URL(fileURLWithPath: args[3]),
                                            colorSpace: srgb, options: options)
} catch {
    fail("\(error)")
}
