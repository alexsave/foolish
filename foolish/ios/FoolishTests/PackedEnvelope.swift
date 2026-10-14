// PackedEnvelope.swift - a server game envelope, assembled byte by byte around
// a REAL masked-state blob from the kernel, for tests that need what the
// server would send an OnlineGame (PackedRosterTests, OnlineSessionViewTests).
//
// The server's layout is written out longhand on purpose: a test that called
// the production encoder would agree with the production decoder no matter
// what either of them did. The bytes themselves are pinned against the
// server's writer in e2e/packed_roster_wire.test.ts.
import Foundation
@testable import FoolishKit

enum PackedEnvelope {

    /// The packed roster trailer.
    static func trailer(id: String, name: String, status: Int,
                        names: [String], ids: [String], isAI: [Bool],
                        good: [String] = [], ts: Double? = nil) -> [UInt8] {
        var w = PackedWriter()
        w.u8(1)                                  // ROSTER_WIRE_FORMAT
        w.blob(Array(id.utf8))
        w.blob(Array(name.utf8))
        w.u8(status)
        w.u8(names.count)                        // the kernel's names block
        for (seat, n) in names.enumerated() {
            w.u8(seat)
            w.blob8(Array(n.utf8))
        }
        for i in 0..<names.count {               // identity, same seats, same order
            w.blob(Array(ids[i].utf8))           // u16: an id has no trim rule
            w.u8(isAI[i] ? 1 : 0)
        }
        w.u8(good.count)
        for g in good { w.blob(Array(g.utf8)) }
        if let ts {
            w.u8(1)
            w.f64(ts)
        } else {
            w.u8(0)
        }
        return w.bytes
    }

    /// One envelope: header, the (optional) JSON island, the view blob, the
    /// (optional) trailer.
    static func envelope(seat: Int, version: Int, island: Data?, viewBlob: Data,
                         trailer: [UInt8]?) -> Data {
        var out: [UInt8] = []
        out.append(1)                                                   // GAME_RESP_FORMAT
        out.append(UInt8((seat >= 0 ? 1 : 0) | (trailer != nil ? 2 : 0)))
        out.append(seat >= 0 ? UInt8(seat) : 0xFF)
        for i in 0..<4 { out.append(UInt8((version >> (8 * i)) & 0xFF)) }
        let isle = [UInt8](island ?? Data())
        out.append(UInt8(isle.count & 0xFF)); out.append(UInt8((isle.count >> 8) & 0xFF))
        out.append(contentsOf: isle)
        let blob = [UInt8](viewBlob)
        out.append(UInt8(blob.count & 0xFF)); out.append(UInt8((blob.count >> 8) & 0xFF))
        out.append(contentsOf: blob)
        if let trailer { out.append(contentsOf: trailer) }
        return Data(out)
    }

    /// A dealt game's masked view blob for one seat, with the
    /// [VIEW_FORMAT_VERSION | viewer] header the server wraps it in.
    static func viewBlob(_ engine: EngineC, seat: Int) async throws -> Data {
        let state = try await engine.statePackedData(viewer: seat)
        return Data([1, UInt8(seat)]) + state
    }
}
