// The bridge from SWIFT, on this Mac: the generated readers
// (shared/tools/structgen, ios/layout.args) against the kernel compiled for
// this machine, stamped with the hash of the layout they were generated for.
// The C smoke (cn_api_smoke.c) proves the entry points; this proves the Swift
// side reads them through generated code and nothing else.
//
//     make -C chuiniu/c swift-smoke
//
// Needs swiftc and libclang, so it is not part of `make run`.

import Foundation

var failures = 0, checks = 0
func check(_ ok: Bool, _ what: String) {
    checks += 1
    if !ok { print("  FAIL \(what)"); fflush(stdout); failures += 1 }
}

func text() -> String {
    var buf = [CChar](repeating: 0, count: Int(CN_API_TEXT_MAX))
    let n = cn_api_text(&buf, Int32(buf.count))
    check(n > 0, "the resident writes a link (\(n))")
    return String(cString: buf)
}

func words(_ what: Int32, _ arg: Int32 = 0) -> String {
    var buf = [CChar](repeating: 0, count: 512)
    _ = cn_api_words(what, arg, &buf, 512)
    return String(cString: buf)
}

// ONE KERNEL, TWO PHONES: each person's seat records are their own device's
// (the App Group's), so switching person saves one set and loads the other.
var records: [UInt8: [UInt8]] = [:]
var current: UInt8 = 0
func be(_ id: UInt8, _ nick: String) {
    if current != 0 {
        var out = [UInt8](repeating: 0, count: Int(CN_API_REC_BYTES))
        let n = cn_api_seats_save(&out, Int32(out.count))
        records[current] = Array(out.prefix(Int(max(n, 0))))
    }
    let mine = records[id] ?? []
    cn_api_seats_load(mine, Int32(mine.count))
    current = id
    var ident = [UInt8](repeating: id, count: 16)
    cn_api_me(&ident, 16)
    let b = Array(nick.utf8)
    cn_api_nickname(b, Int32(b.count))
    cn_api_sender(nil, 0, -1)
}

@main
struct Smoke {
    static func main() {
        do {
            check(cn_api_layout_hash() == SG_LAYOUT_HASH, "the library and the readers are one layout")

            be(1, "Alex")
            var seed = [UInt8](repeating: 0, count: 32)
            for i in 0..<32 { seed[i] = UInt8(i * 5 + 1) }
            check(cn_api_new(&seed, 1) == Int32(CN_EOK), "Alex makes a DM lobby")
            var t = try readCnApiTable(cn_api_table()!)
            check(t.phase == CN_PHASE_WAITING && t.seat.map(\.name) == ["Alex"], "one named seat, waiting")
            check(words(CN_API_W_STAGED_CAPTION) == "Alex wants a game of Chui Niu. Tap to join", "the invite")
            let lobby = text()

            be(2, "Bo")
            check(cn_api_read(lobby) == Int32(CN_EOK), "Bo reads it")
            t = try readCnApiTable(cn_api_table()!)
            check(t.offered == CN_LOBBY_JOIN && t.canJoinStart == 1, "Bo may join and start")
            check(cn_api_join_start() == 1, "Bo joins and starts")
            let start = text()

            be(1, "Alex")
            check(cn_api_read(lobby) == Int32(CN_EOK), "Alex's phone still shows the lobby")
            check(cn_api_adopt(start) == Int32(CN_EOK), "Alex adopts the start")
            t = try readCnApiTable(cn_api_table()!)
            check(t.phase == CN_PHASE_LIVE && t.gamePhase == CN_PH_BIDDING && t.seat.map(\.name) == ["Alex", "Bo"], "live, two seats")
            let beats = try readCnBeats(cn_api_beats_now()!)
            check(beats.beat.count == 1 && beats.beat[0].kind == CN_BK_SHAKE, "the start's cups shake")
            let f0 = try readCnBeatFrame(cn_api_beats_frame(0)!)
            check(f0.shaking == 1 && f0.state.count == 1, "the frame at 0")
            let fEnd = try readCnBeatFrame(cn_api_beats_frame(UInt32(beats.totalMs))!)
            check(fEnd.done == 1 && fEnd.prog == [1.0], "the frame at the end")

            var v = try readCnView(cn_api_view(CN_API_ME)!)
            check(v.myDice.count == 5 && v.myDice == v.myDice.sorted() && v.diceN.count == Int(CN_MAX_SEATS), "five sorted dice")
            check(v.myTurn == 1 && v.canRaise == 1 && v.canCall == 0 && v.minQ == 1 && v.minF == 2 && v.maxQ == 10, "the opening menu")
            check(v.minQFace == [0, 0, 1, 1, 1, 1, 1], "any face from one")
            check(words(CN_API_W_HEADLINE) == "Your turn: open the bidding", "the headline")
            check(cn_api_raise(3, 4) == 1, "Alex stages three 4s")
            t = try readCnApiTable(cn_api_table()!)
            check(t.staged == CN_API_STAGED_BID && t.stagedQ == 3 && t.stagedF == 4, "staged")
            let staged = try readCnApiEvents(cn_api_plan_staged()!)
            check(staged.ev.count == 1 && staged.ev[0].kind == CN_EV_BID, "the staged plan")
            check(words(CN_API_W_STAGED_CAPTION) == "Alex bid three 4s", "the staged caption")
            let bid = text()
            check(cn_api_commit() == 1, "sent")

            be(2, "Bo")
            check(cn_api_adopt(bid) == Int32(CN_EOK), "Bo adopts the bid")
            v = try readCnView(cn_api_view(CN_API_ME)!)
            check(v.myTurn == 1 && v.canCall == 1 && v.bidQ == 3 && v.bidF == 4, "Bo may call")
            check(v.minQFace == [0, 0, 4, 4, 4, 3, 3], "above three 4s: four 2s to four 4s, three 5s and 6s")
            check(cn_api_call() == 1, "Bo stages the call")
            v = try readCnView(cn_api_view(CN_API_ME)!)
            check(v.revealed == 0, "nothing lifts while staged")
            check(words(CN_API_W_STAGED_CAPTION) == "Bo calls three 4s", "the call's caption")
            _ = text()
            check(cn_api_commit() == 1, "the call is sent")
            v = try readCnView(cn_api_view(CN_API_ME)!)
            check(v.revealed == 1 && v.shownN[0] == 5 && v.shownN[1] == 5 && v.shown.count == Int(CN_MAX_DICE), "every cup lifts")
            check(v.shownCounts.reduce(0, +) == v.callCount, "the counting dice are flagged")
            let plan = try readCnApiEvents(cn_api_plan(1, 2)!)
            check(plan.ev.map(\.kind) == [CN_EV_CALL, CN_EV_REVEAL, CN_EV_LOSE, CN_EV_ROUND], "the call's plan")
            check(plan.ev[1].diceN[0] == 5 && plan.ev[1].dice.count == Int(CN_MAX_DICE), "the reveal carries the dice")
            check(words(CN_API_W_OUTCOME).hasPrefix("Bo calls. Three 4s was "), "the outcome")

            var line = [CChar](repeating: 0, count: 256)
            check(cn_api_string(0, &line, 256) > 0 && String(cString: line) == ChuiniuStringsEn["GAME_NAME"], "GAME_NAME by key")
            check(ChuiniuStringKeys[0] == "GAME_NAME", "the generated key list")

            print("swift bridge: \(checks) checks, \(failures) failed")
            exit(failures == 0 ? 0 : 1)
        } catch {
            print("  FAIL a reader threw: \(error)")
            exit(1)
        }
    }
}
