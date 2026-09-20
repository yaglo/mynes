import XCTest
@testable import ChainVisualiser

final class ProtocolTests: XCTestCase {
    private func word(_ n: UInt32, into data: inout Data) {
        var value=n.littleEndian
        withUnsafeBytes(of: &value) { data.append(contentsOf: $0) }
    }
    func testPresetCatalog() {
        var data=Data()
        for n: UInt32 in [7,156,1,42,1,0,1,0,1] { word(n,into:&data) }
        let name=Data("Custom CRT".utf8)
        data.append(name); data.append(Data(repeating:0,count:128-name.count))
        let result=PresetCatalog.decode(data)
        XCTAssertEqual(result?.active?.name,"Custom CRT")
        XCTAssertEqual(result?.active?.isUser,true)
        XCTAssertEqual(result?.modified,true)
        XCTAssertEqual(result?.revision,42)
        XCTAssertNil(PresetCatalog.decode(data.dropLast()))
        data.append(0)
        XCTAssertNil(PresetCatalog.decode(data))
    }
    func testEmptyAndMalformedSnapshots() {
        var data=Data()
        for n: UInt32 in [0,12,16,100,0] { word(n,into:&data) }
        XCTAssertEqual(ChainSnapshot.decode(from:data)?.frameNumber,16)
        data[16]=1
        XCTAssertNil(ChainSnapshot.decode(from:data))
        XCTAssertNil(ChainSnapshot.decode(from:Data()))
    }
    func testUnknownKernelIsSafe() {
        XCTAssertEqual(KernelType.from(ordinal:255),.unknown)
        XCTAssertEqual(KernelType.from(ordinal:16),.raster)
        XCTAssertEqual(KernelType.from(ordinal:18),.receiverDemod)
    }
}

// High-rate telemetry must not invalidate static chain/editor controls.
import Observation
import os

extension ProtocolTests {
    @MainActor
    func testTelemetryDoesNotInvalidateTopology() {
        let connection = EmulatorConnection()
        func snapshot(frame: UInt32, timing: UInt32, enabled: UInt8 = 1) -> Data {
            var data = Data()
            for n: UInt32 in [0, 56, frame, frame * 17, 1] { word(n, into: &data) }
            data.append(contentsOf: [enabled, 0, 16, 0])
            word(timing, into: &data); word(timing, into: &data)
            data.append(Data("Raster".utf8)); data.append(Data(repeating: 0, count: 26))
            return data
        }
        connection.applyMessage(snapshot(frame: 0, timing: 1))
        let changed = OSAllocatedUnfairLock(initialState: false)
        withObservationTracking { _ = connection.videoStages } onChange: { changed.withLock { $0 = true } }
        for frame in 1...120 { connection.applyMessage(snapshot(frame: UInt32(frame), timing: UInt32(frame))) }
        XCTAssertFalse(changed.withLock { $0 }, "Timing and frame counters must not redraw the chain")
        XCTAssertEqual(connection.framesPerSecond, 1000.0 / 17, accuracy: 0.01)
        connection.applyMessage(snapshot(frame: 121, timing: 1, enabled: 0))
        XCTAssertTrue(changed.withLock { $0 }, "A real topology change must update the chain")
    }
}
