import Foundation

struct PresetEntry: Identifiable, Sendable, Equatable {
    let id: Int
    let name: String
    let isUser: Bool
}
struct PresetCatalog: Sendable {
    var revision: UInt32 = 0
    var entries: [PresetEntry] = []
    var activeID: Int = -1
    var modified = false
    var active: PresetEntry? { entries.first { $0.id == activeID } }

    static func decode(_ data: Data) -> PresetCatalog? {
        guard data.count >= 28 else { return nil }
        func word(_ n: Int) -> UInt32 {
            data.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: n, as: UInt32.self)) }
        }
        guard word(0) == 7, word(4) == data.count - 8, word(8) == 1 else { return nil }
        let count = Int(word(16))
        guard count <= 63, data.count == 28 + count * 136 else { return nil }
        var entries: [PresetEntry] = []
        for i in 0..<count {
            let offset = 28 + i * 136
            guard word(offset) == i else { return nil }
            let name = String(decoding: data[(offset + 8)..<(offset + 136)].prefix { $0 != 0 }, as: UTF8.self)
            entries.append(.init(id: i, name: name, isUser: word(offset + 4) != 0))
        }
        return .init(revision: word(12), entries: entries, activeID: Int(Int32(bitPattern: word(20))), modified: word(24) != 0)
    }
}
