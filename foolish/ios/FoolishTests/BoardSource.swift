// THE BOARD, AS TEXT - for the sixteen test files that assert on what
// MessageTableView's source actually says.
//
// WHY THOSE TESTS READ SOURCE AT ALL is written at the head of each of them,
// and the reason is always the same shape: the subject is WHERE a modifier sits
// in a `body`, or WHICH argument a view hands its children, or that one rule is
// written once rather than four times. None of those is a value a rendered view
// returns, and a board needs a laid-out screen, a `GameView` and a live
// controller before any of it means anything. What went wrong was a fact about
// the source, so the test is about the source.
//
// WHAT THIS ADDS. The board is no longer one file: it is
// `MessageTableView.swift` plus a `MessageTableView+<subject>.swift` per domain
// beside it. A test that read the main file alone would now quietly stop
// covering everything that moved - `HandLandingTests`' "no call site re-derives
// the landing chain" would scan a file with hardly any call sites left in it,
// and pass while saying nothing. So the board is read as the SUM of its files,
// which is the same text those assertions were written against.
//
// MAIN FILE FIRST, then the extensions in a stable (alphabetical) order. Three
// of the assertions are about ORDER - the flight layer before the status mark
// before my own hand, the action column's fixed-size box before its
// `.transaction` - and every one of them is about lines inside `table` or
// `boardContent`, which are in the main file. Putting it first keeps those
// indices meaning what they meant.
//
// A MISSING FILE IS A RED TEST, not a silently empty string: that is the whole
// failure mode a source test has, and it is the one that looks green.

import XCTest

enum BoardSource {

    /// `ios/` - two directories up from this file.
    private static var iosRoot: URL {
        URL(fileURLWithPath: #filePath).deletingLastPathComponent()   // FoolishTests
                                      .deletingLastPathComponent()    // ios
    }

    /// Every file the board is made of, main declaration first.
    static func files(file: StaticString = #filePath, line: UInt = #line) -> [URL] {
        let dir = iosRoot.appendingPathComponent("FoolishKit/Boards")
        let main = dir.appendingPathComponent("MessageTableView.swift")
        guard FileManager.default.fileExists(atPath: main.path) else {
            XCTFail("FoolishKit/Boards/MessageTableView.swift is gone - this test needs "
                    + "its path updated", file: file, line: line)
            return []
        }
        let rest = ((try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? [])
            .filter { $0.hasPrefix("MessageTableView+") && $0.hasSuffix(".swift") }
            .sorted()
            .map { dir.appendingPathComponent($0) }
        return [main] + rest
    }

    /// The whole board as one string - the main file, then each extension.
    static func text(file: StaticString = #filePath, line: UInt = #line) throws -> String {
        let urls = files(file: file, line: line)
        guard !urls.isEmpty else { return "" }
        return try urls.map { try String(contentsOf: $0, encoding: .utf8) }
            .joined(separator: "\n")
    }

    /// …and as lines, for the assertions that are about order.
    static func lines(file: StaticString = #filePath, line: UInt = #line) throws -> [String] {
        try text(file: file, line: line).components(separatedBy: "\n")
    }
}
