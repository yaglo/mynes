import Foundation

struct PhysicalControl: Identifiable, Sendable {
    let id: Int
    let name: String
    let group: String
    let value: Double
    let minimum: Double
    let maximum: Double

    static func decode(_ data: Data) -> [PhysicalControl]? {
        guard data.count >= 16 else { return nil }
        func word(_ offset: Int) -> UInt32 {
            data.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: offset, as: UInt32.self)) }
        }
        guard word(0) == 5, word(4) == data.count - 8, word(8) == 1 else { return nil }
        let count = Int(word(12))
        guard count <= 48, data.count == 16 + count * 72 else { return nil }
        func string(_ offset: Int, _ length: Int) -> String {
            String(decoding: data[offset..<(offset + length)].prefix(while: { $0 != 0 }), as: UTF8.self)
        }
        var controls: [PhysicalControl] = []
        for i in 0..<count {
            let offset = 16 + i * 72
            let value = Double(Float(bitPattern: word(offset + 4)))
            let minimum = Double(Float(bitPattern: word(offset + 8)))
            let maximum = Double(Float(bitPattern: word(offset + 12)))
            guard value.isFinite, minimum.isFinite, maximum.isFinite, minimum < maximum,
                  Int(word(offset)) == i else { return nil }
            controls.append(.init(id: i, name: string(offset + 16, 32), group: string(offset + 48, 24),
                                  value: value, minimum: minimum, maximum: maximum))
        }
        return controls
    }
}
