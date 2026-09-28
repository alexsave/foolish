// The bridge from SWIFT, on this Mac: the generated readers
// (shared/tools/structgen, ios/layout.args) against the kernel compiled for
// this machine, stamped with the hash of the layout they were generated for.
// The C smoke (pk_api_smoke.c) proves the entry points; this proves the Swift
// side reads them through generated code and nothing else.
//
//     make -C pickemup/c swift-smoke
//
// Needs swiftc and libclang, so it is not part of `make run`.

import Foundation

var failures = 0, checks = 0
func check(_ ok: Bool, _ what: String) {
    checks += 1
    if !ok { print("  FAIL \(what)"); failures += 1 }
}

func text() -> String {
    var buf = [CChar](repeating: 0, count: Int(PK_API_TEXT_MAX))
    let n = pk_api_text(&buf, Int32(buf.count))
    check(n > 0, "the resident writes a link (\(n))")
    return String(cString: buf)
}

func be(_ id: UInt8, _ nick: String) {
    var ident = [UInt8](repeating: id, count: 16)
    pk_api_me(&ident, 16)
    let b = Array(nick.utf8)
    b.withUnsafeBufferPointer { pk_api_nickname($0.baseAddress, Int32(b.count)) }
    pk_api_seats_load(nil, 0)
}

@main struct SwiftSmoke {
    static func main() throws {
        check(pk_api_layout_hash() == SG_LAYOUT_HASH, "the library and the readers are one layout")

        be(1, "Alex")
        var seed = [UInt8](repeating: 0, count: 32)
        for i in 0..<32 { seed[i] = UInt8((i * 31 + 5) & 0xff) }
        check(pk_api_new(&seed, 1) == Int32(PK_EOK), "a new DM lobby")
        var t = try readPkApiTable(pk_api_table()!)
        check(t.phase == PK_PHASE_WAITING && t.me == 0 && t.seat.count == 1 && t.seat[0].name == "Alex",
              "the table reads: Alex waits in seat 0")
        check(t.offered == PK_LOBBY_WAITING, "offered WAITING")
        let invite = text()

        be(2, "Bo")
        check(pk_api_read(invite) == Int32(PK_EOK), "Bo reads the invitation")
        t = try readPkApiTable(pk_api_table()!)
        check(t.me == PK_SEAT_NONE && t.offered == PK_LOBBY_JOIN && t.canJoinStart != 0, "Bo may join and start")
        check(pk_api_join_start() == 1, "Bo joins and starts")
        t = try readPkApiTable(pk_api_table()!)
        check(t.phase == PK_PHASE_LIVE && t.seat.map(\.name) == ["Alex", "Bo"], "two named seats, live")

        var v = try readPkView(pk_api_view(PK_API_ME)!)
        check(v.me == 1 && v.myHand.count == 7 && v.myPlayable.count == 7 && v.reveal.count == 2,
              "Bo's view: seven cards, and no counts for anybody else")
        check(v.reveal.allSatisfy { $0.card.isEmpty }, "nothing revealed while playing")
        let deal = try readPkApiEvents(pk_api_plan(PK_API_ME, -1, 0)!)
        check(deal.ev.first?.kind == PK_EV_LOBBY_START && deal.ev.filter { $0.kind == PK_EV_DEAL }.count == 14,
              "the deal's plan: fourteen cards dealt")
        check(deal.ev.filter { $0.kind == PK_EV_DEAL && $0.seat == 0 }.allSatisfy { $0.card == PK_CARD_HIDDEN },
              "Alex's cards are hidden from Bo")

        check(pk_api_draw() == 1, "Bo draws")
        v = try readPkView(pk_api_view(PK_API_ME)!)
        check(v.myHand.count == 8 && v.canPass != 0, "eight in hand, and a pass is there")
        let draft = try readPkApiEvents(pk_api_plan_draft(PK_API_ME)!)
        check(draft.ev.contains { $0.kind == PK_EV_DRAW && $0.card == v.myHand[7] }, "the draft's draw is the new card")
        check(pk_api_pass() == 1, "Bo passes")
        let sent = text()
        check(pk_api_commit() == 1, "sent")

        be(1, "Alex")
        pk_api_sender(sent, 1, 0)
        check(pk_api_read(sent) == Int32(PK_EOK), "Alex reads it")
        t = try readPkApiTable(pk_api_table()!)
        check(t.me == 0 && t.by == PK_BY_TAG && t.bubbles == 1, "no record on this phone: seated by the tag")
        let since = try readPkSince(pk_api_since(0, 1)!)
        check(since.drawn.count == Int(PK_MAX_SEATS) && since.drawn[1] == 1, "since: Bo drew one")
        var line = [CChar](repeating: 0, count: 256)
        check(pk_api_words(PK_API_W_CAPTION, 1, &line, 256) > 0, "the caption")
        print("  \"\(String(cString: line))\"")
        check(pk_api_string(0, &line, 256) > 0 && String(cString: line) == PickemupStringsEn["GAME_NAME"], "GAME_NAME by key")
        check(PickemupStringKeys[0] == "GAME_NAME", "the generated key list")

        print("swift bridge: \(checks) checks, \(failures) failed")
        exit(failures == 0 ? 0 : 1)
    }
}
