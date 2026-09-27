// The bridge from SWIFT, on this Mac: the generated readers
// (shared/tools/structgen, ios/layout.args) against the kernel compiled for
// this machine, stamped with the hash of the layout they were generated for.
// The C smoke (tb_api_smoke.c) proves the entry points; this proves the Swift
// side reads them through generated code and nothing else, T11 included: a
// staged keep reads as blanks, and the send echo brings the reroll.
//
//     make -C tallybones/c swift-smoke
//
// Needs swiftc and libclang, so it is not part of `make run`.

import Foundation

var failures = 0, checks = 0
func check(_ ok: Bool, _ what: String) {
    checks += 1
    if !ok { print("  FAIL \(what)"); failures += 1 }
}

func text() -> String {
    var buf = [CChar](repeating: 0, count: Int(TB_API_TEXT_MAX))
    let n = tb_api_text(&buf, Int32(buf.count))
    check(n > 0, "the resident writes a link (\(n))")
    return String(cString: buf)
}

func words(_ what: Int32, _ arg: Int32) -> String {
    var line = [CChar](repeating: 0, count: 256)
    check(tb_api_words(what, arg, &line, 256) >= 0, "words \(what)")
    return String(cString: line)
}

func be(_ id: UInt8, _ nick: String) {
    var ident = [UInt8](repeating: id, count: 16)
    tb_api_me(&ident, 16)
    let b = Array(nick.utf8)
    b.withUnsafeBufferPointer { tb_api_nickname($0.baseAddress, Int32(b.count)) }
    tb_api_seats_load(nil, 0)
}

@main struct SwiftSmoke {
    static func main() throws {
        check(tb_api_layout_hash() == SG_LAYOUT_HASH, "the library and the readers are one layout")

        be(1, "Alex")
        var seed = [UInt8](repeating: 0, count: 32)
        for i in 0..<32 { seed[i] = UInt8((i * 31 + 5) & 0xff) }
        check(tb_api_new(&seed, 1) == Int32(TB_EOK), "a new DM lobby")
        var t = try readTbApiTable(tb_api_table()!)
        check(t.phase == TB_PHASE_WAITING && t.me == 0 && t.seat.count == 1 && t.seat[0].name == "Alex",
              "the table reads: Alex waits in seat 0")
        let invite = text()

        be(2, "Bo")
        check(tb_api_read(invite) == Int32(TB_EOK), "Bo reads the invitation")
        t = try readTbApiTable(tb_api_table()!)
        check(t.me == TB_SEAT_NONE && t.offered == TB_LOBBY_JOIN && t.canJoinStart != 0, "Bo may join and start")
        check(tb_api_stage_join_start() == 1, "Bo joins and starts")
        t = try readTbApiTable(tb_api_table()!)
        check(t.phase == TB_PHASE_LIVE && t.seat.map(\.name) == ["Alex", "Bo"], "two named seats, live")
        let start = text()

        be(1, "Alex")
        tb_api_sender(start, 1, 0)
        check(tb_api_read(start) == Int32(TB_EOK), "Alex reads the start")
        t = try readTbApiTable(tb_api_table()!)
        check(t.me == 0 && t.myTurn != 0 && t.by == TB_BY_TAG, "Alex, by the tag, on turn")
        var v = try readTbView(tb_api_view()!)
        check(v.seat.count == 2 && v.known == 31 && v.dice.allSatisfy { (1...6).contains($0) }, "roll 1: five values")
        let roll1 = v.dice
        let deal = try readTbApiEvents(tb_api_plan(-1, 0)!)
        check(deal.ev.first?.kind == TB_EV_START && deal.ev.last?.kind == TB_EV_ROLL, "the start's plan")

        check(tb_api_stage_keep(0x03) == 1, "keep dice 1 and 2")
        v = try readTbView(tb_api_view()!)
        check(v.draft != 0 && v.known == 0x03 && Array(v.dice[2...]) == [0, 0, 0], "staged: the rerolls are blank")
        check(Array(v.dice[0...1]) == Array(roll1[0...1]), "the kept two stay")
        let staged = words(TB_API_W_STAGED_CAPTION, 0)
        print("  \"\(staged)\"")
        check(staged.hasPrefix("Alex keeps") && staged.hasSuffix("rerolls three"), "the staged caption")
        check(tb_api_read(text()) == Int32(TB_ESTAGED), "my own staged link is refused")
        check(tb_api_mark_sent() == 1, "sent")
        v = try readTbView(tb_api_view()!)
        check(v.draft == 0 && v.known == 31 && v.roll == 2, "the reroll is here")
        let beats = try readTbBeats(tb_api_beats_now()!)
        check(beats.beat.count == 1 && beats.beat[0].kind == TB_BK_SETTLE && beats.beat[0].mask == 0x1C,
              "three dice settle")
        let end = try readTbBeatFrame(tb_api_beats_frame(UInt32(beats.totalMs))!)
        check(end.done != 0 && end.dice == v.dice, "the frame lands on the view")
        let mid = try readTbBeatSample(tb_api_beat_sample(0, 2, UInt32(beats.beat[0].startMs + 10))!)
        check(mid.state == TB_BS_ACTIVE && mid.apply != 0, "die 3 in the air")

        check(words(TB_API_W_CAT, Int32(TB_C_TALLYBONES)) == TallybonesStringsEn["GAME_NAME"], "the category is the name")
        check(TallybonesStringKeys[0] == "GAME_NAME", "the generated key list")

        print("swift bridge: \(checks) checks, \(failures) failed")
        exit(failures == 0 ? 0 : 1)
    }
}
