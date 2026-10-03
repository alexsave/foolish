#if UTTT_BIG_BOARD
import Combine
import Darwin
import SwiftUI
import UtttKit

/// THE 243 BOARD IN THE HARNESS (docs/BIG_BOARD.md): UtttBigBoardView over a
/// synthetic position, with the last tapped cell, the process's memory
/// footprint and the geometry self-check written above it as text a UI test
/// reads (UtttPreviewUITests).
///
/// `simctl launch <udid> cards.uttt.preview --screen 243` opens straight here.
struct BigBoardPreview: View {
    @State private var tapped = -1
    /// How many times the mode's door (`setModeHold`) fired.
    @State private var holds = 0
    @State private var footprint = 0.0
    @State private var peak = 0.0
    /// The "resize" button's state: the board 340 points tall (Messages'
    /// compact drawer) instead of the rest of the screen, so a UI test can
    /// change the view's bounds under a zoom.
    @State private var compact = false
    private let geometry = BigBoardSynthetic.geometryCheck()
    private let tick = Timer.publish(every: 0.25, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(spacing: 2) {
            Text(verbatim: tapped >= 0 ? "tapped \(tapped)" : "tapped none")
                .accessibilityIdentifier("big.tapped")
            Text(verbatim: "hold fired \(holds)")
                .accessibilityIdentifier("hold.count")
            Text(String(format: "footprint %.1f MB peak %.1f MB", footprint, peak))
                .accessibilityIdentifier("big.memory")
            HStack {
                Text(geometry)
                    .accessibilityIdentifier("big.geometry")
                Button("resize") { compact.toggle() }
                    .accessibilityIdentifier("big.resize")
            }
            BigBoard(tapped: $tapped, holds: $holds)
                .frame(height: compact ? 340 : nil)
            if compact { Spacer(minLength: 0) }
        }
        .font(.system(size: 12, design: .monospaced))
        .foregroundStyle(Color(white: 0.85))
        .onReceive(tick) { _ in
            footprint = BigBoardSynthetic.footprintMB()
            peak = max(peak, footprint)
        }
    }

    struct BigBoard: UIViewRepresentable {
        @Binding var tapped: Int
        @Binding var holds: Int

        func makeUIView(context: Context) -> UtttBigBoardView {
            let v = UtttBigBoardView(frame: .zero)
            v.accessibilityIdentifier = "big.board"
            let b = BigBoardSynthetic.board
            v.set(cells: b.cells, nodes: b.nodes, regionRect: b.region, last: b.last, draft: b.draft)
            let binding = $tapped
            let held = $holds
            v.setModeHold { held.wrappedValue += 1 }
            v.onTap = { [weak v] mv in
                binding.wrappedValue = mv
                /* a tap on an empty cell stages it, as the game will */
                if let v, v.cells[mv] == 0 { v.draft = mv }
            }
            /* `--focus region` / `--focus last`: open zoomed onto it, so a
             * screenshot shows the wash, the rings and the draft; `--zoom Z`
             * (a number, or `max`): open at that zoom centred on the region */
            let a = ProcessInfo.processInfo.arguments
            if let i = a.firstIndex(of: "--zoom"), i + 1 < a.count {
                let z = a[i + 1] == "max" ? CGFloat.infinity : CGFloat(Double(a[i + 1]) ?? 1)
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak v] in
                    guard let v, z > 1 else { return }
                    let r = b.region ?? CGRect(x: 0, y: 0, width: 1, height: 1)
                    /* focus fits a rect to a third of the view: the rect that zoom means */
                    let w = z.isFinite ? 1 / (3 * z) : 1e-4
                    v.focus(on: CGRect(x: r.midX - w / 2, y: r.midY - w / 2, width: w, height: w), animated: false)
                }
            }
            if let i = a.firstIndex(of: "--focus"), i + 1 < a.count {
                let what = a[i + 1]
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak v] in
                    guard let v else { return }
                    if what == "region", let r = b.region { v.focus(on: r, animated: false) }
                    if what == "last", b.last >= 0 {
                        v.focus(on: UtttBigBoardView.rect(level: 4, prefix: b.last / 9), animated: false)
                    }
                }
            }
            return v
        }
        func updateUIView(_ v: UtttBigBoardView, context: Context) {}
    }
}

/// A deterministic made-up position, and the checks the harness shows.
/// PREVIEW CODE, NOT PRODUCT LOGIC: the statuses are worked out here with a
/// small bottom-up pass only so the board has decided blocks to draw; the
/// game's statuses are the kernel's (uttt_big.c).
enum BigBoardSynthetic {
    struct Board {
        var cells: [UInt8]
        var nodes: [UInt8]
        var region: CGRect?
        var last: Int
        var draft: Int
    }

    static let board: Board = make()

    /// SplitMix64, so the same board every launch.
    private struct Rng {
        var s: UInt64
        mutating func next() -> UInt64 {
            s &+= 0x9E37_79B9_7F4A_7C15
            var z = s
            z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
            z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
            return z ^ (z >> 31)
        }
        mutating func unit() -> Double { Double(next() >> 11) / Double(1 << 53) }
    }

    private static let lines: [[Int]] = [[0, 1, 2], [3, 4, 5], [6, 7, 8], [0, 3, 6], [1, 4, 7], [2, 5, 8],
                                         [0, 4, 8], [2, 4, 6]]

    /// 1 or 2 for a line of that mark among nine children, 3 for nine
    /// decided children without one, 0 otherwise.
    private static func status(_ ch: [UInt8]) -> UInt8 {
        for m: UInt8 in [1, 2] where lines.contains(where: { $0.allSatisfy { ch[$0] == m } }) { return m }
        return ch.allSatisfy { $0 != 0 } ? 3 : 0
    }

    /// A GAME EARLY IN PLAY: a few marks scattered over the whole board,
    /// and a few dense regions where blocks have been decided - won at
    /// levels 2, 3 and 4 (so all three sizes of big mark show) and drawn at
    /// level 4 - most of them round the centre, where the forced target is,
    /// so one screenshot at each zoom shows them.
    private static func make() -> Board {
        var rng = Rng(s: 243)
        var cells = [UInt8](repeating: 0, count: UtttBigBoardView.leafCount)
        func mark() -> UInt8 { rng.unit() < 0.5 ? 1 : 2 }
        /* the scatter: a mark on about one cell in fifty */
        for i in 0..<cells.count where rng.unit() < 0.02 { cells[i] = mark() }

        /* a node's leaves, by digits from the top */
        func node(_ digits: [Int]) -> Int { digits.reduce(0) { $0 * 9 + $1 } }
        func leaves(level: Int, prefix: Int) -> Range<Int> {
            let span = [59_049, 6561, 729, 81, 9, 1][level]
            return (prefix * span)..<((prefix + 1) * span)
        }
        /* a decided block is a played-out one: about half its cells taken */
        func fill(level: Int, prefix: Int) {
            for i in leaves(level: level, prefix: prefix) where rng.unit() < 0.45 { cells[i] = mark() }
        }
        /* a planted win: a diagonal of diagonals down to the cells */
        func plant(level: Int, prefix: Int, mark: UInt8) {
            for d in [0, 4, 8] {
                if level == 4 { cells[prefix * 9 + d] = mark }
                else { plant(level: level + 1, prefix: prefix * 9 + d, mark: mark) }
            }
        }
        func win(_ digits: [Int], _ m: UInt8) {
            fill(level: digits.count, prefix: node(digits))
            plant(level: digits.count, prefix: node(digits), mark: m)
        }
        /* a level-4 draw: nine cells, no line */
        func draw(_ digits: [Int]) {
            let p = node(digits)
            for (k, m) in ([1, 2, 1, 1, 2, 2, 2, 1, 1] as [UInt8]).enumerated() { cells[p * 9 + k] = m }
        }

        win([8, 4], 1)                     // level 2: X, bottom right
        win([0, 8], 2)                     // level 2: O, top left
        win([4, 4, 0], 2)                  // level 3: O, beside the centre
        win([4, 4, 8], 1)                  // level 3: X, beside the centre
        win([2, 0, 4], 2)                  // level 3: O, top right
        win([7, 2, 6], 1)                  // level 3: X, bottom middle
        win([4, 4, 4, 0], 1)               // level 4, in the target's own 9 x 9
        win([4, 4, 4, 2], 1)
        win([4, 4, 4, 6], 2)
        draw([4, 4, 4, 8])                 // a drawn 3 x 3 there too
        draw([4, 1, 3, 3])
        for _ in 0..<40 {                  // and decided 3 x 3s all over
            let p = Int(rng.next() % 6561)
            let digits = [p / 729, p / 81 % 9, p / 9 % 9, p % 9]
            if rng.unit() < 0.15 { draw(digits) } else { win(digits, mark()) }
        }

        /* THE TARGET, the very centre 3 x 3 (digits 4 4 4 4): kept nearly
         * empty, and the last move an X whose last four digits name it, in
         * the middle-left top block (digit 3). */
        let target = node([4, 4, 4, 4])
        for i in leaves(level: 4, prefix: target) { cells[i] = 0 }
        cells[target * 9 + 1] = 2
        cells[target * 9 + 6] = 1
        let last = 3 * 6561 + target
        cells[last] = 1
        let draft = (target * 9..<target * 9 + 9).first { cells[$0] == 0 } ?? -1

        var nodes = [UInt8](repeating: 0, count: UtttBigBoardView.nodeCount)
        let offset = [0, 1, 10, 91, 820]
        let count = [1, 9, 81, 729, 6561]
        for p in 0..<count[4] {
            nodes[offset[4] + p] = status(Array(cells[(p * 9)..<(p * 9 + 9)]))
        }
        for level in stride(from: 3, through: 0, by: -1) {
            for p in 0..<count[level] {
                let first = offset[level + 1] + p * 9
                nodes[offset[level] + p] = status(Array(nodes[first..<(first + 9)]))
            }
        }
        let open = !closed(nodes, level: 4, prefix: target)
        return Board(cells: cells, nodes: nodes, region: open ? UtttBigBoardView.rect(level: 4, prefix: target) : nil,
                     last: last, draft: open ? draft : -1)
    }

    private static func closed(_ nodes: [UInt8], level: Int, prefix: Int) -> Bool {
        let offset = [0, 1, 10, 91, 820]
        var l = level, p = prefix
        while l >= 0 {
            if nodes[offset[l] + p] != 0 { return true }
            l -= 1; p /= 9
        }
        return false
    }

    /// THE GEOMETRY AGAINST HAND-WORKED VALUES, and every cell's centre back
    /// to its own index: "geometry ok", or the first case that failed.
    static func geometryCheck() -> String {
        typealias V = UtttBigBoardView
        func near(_ a: CGRect, _ b: CGRect) -> Bool {
            abs(a.minX - b.minX) < 1e-9 && abs(a.minY - b.minY) < 1e-9
                && abs(a.width - b.width) < 1e-9 && abs(a.height - b.height) < 1e-9
        }
        let t = 1.0 / 3, n = 1.0 / 9, c = 1.0 / 243
        let rects: [(Int, Int, CGRect)] = [
            (0, 0, CGRect(x: 0, y: 0, width: 1, height: 1)),
            (1, 4, CGRect(x: t, y: t, width: t, height: t)),                   // the centre block
            (1, 5, CGRect(x: 2 * t, y: t, width: t, height: t)),               // row 1, column 2
            (2, 72, CGRect(x: 6 * n, y: 6 * n, width: n, height: n)),          // block 8, its block 0
            (5, 0, CGRect(x: 0, y: 0, width: c, height: c)),
            (5, 1, CGRect(x: c, y: 0, width: c, height: c)),                   // d5 = 1: one column right
            (5, 3, CGRect(x: 0, y: c, width: c, height: c)),                   // d5 = 3: one row down
            (5, 9, CGRect(x: 3 * c, y: 0, width: c, height: c)),               // d4 = 1: three columns right
            (5, 2 * 6561 + 3, CGRect(x: 162 * c, y: c, width: c, height: c)),  // d1 = 2, d5 = 3
            (5, 59_048, CGRect(x: 242 * c, y: 242 * c, width: c, height: c)),  // all eights
            (5, 59_049, .zero), (6, 0, .zero), (1, -1, .zero),
        ]
        for (l, p, want) in rects where !near(V.rect(level: l, prefix: p), want) {
            return "geometry FAIL rect(\(l),\(p)) = \(V.rect(level: l, prefix: p))"
        }
        let hits: [(CGPoint, Int)] = [
            (CGPoint(x: 0.5, y: 0.5), 29_524),        // col = row = 121 = 11111 in base 3: every digit 4
            (CGPoint(x: 0, y: 0), 0),
            (CGPoint(x: 1, y: 1), 59_048),            // the far edge belongs to the last cell
            (CGPoint(x: 1, y: 0), 2 * 7381),          // col 242: every digit 2
            (CGPoint(x: -0.001, y: 0.5), -1),
            (CGPoint(x: 0.5, y: 1.001), -1),
        ]
        for (p, want) in hits where V.cell(at: p) != want {
            return "geometry FAIL cell(at: \(p)) = \(V.cell(at: p)), want \(want)"
        }
        let ids: [(Int, Int, Int)] = [(0, 0, 0), (1, 1, 0), (9, 1, 8), (10, 2, 0), (90, 2, 80),
                                      (91, 3, 0), (819, 3, 728), (820, 4, 0), (7380, 4, 6560)]
        for (id, l, p) in ids {
            guard let got = V.node(id: id), got.level == l, got.prefix == p else {
                return "geometry FAIL node(id: \(id))"
            }
        }
        if V.node(id: 7381) != nil || V.node(id: -1) != nil { return "geometry FAIL node off the tree" }
        for mv in 0..<V.leafCount {
            let r = V.rect(level: 5, prefix: mv)
            if V.cell(at: CGPoint(x: r.midX, y: r.midY)) != mv || V.cell(at: r.origin) != mv {
                return "geometry FAIL round trip \(mv)"
            }
        }
        return "geometry ok"
    }

    /// The process's physical footprint (what jetsam counts), in MB.
    static func footprintMB() -> Double {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let kr = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        return kr == KERN_SUCCESS ? Double(info.phys_footprint) / 1_048_576 : -1
    }
}
#endif
