// StageViewTests - the host side of the kernel's stage: the turned layer
// maps every point where the kernel's camera says, a view with no size never
// asks for a frame, my cup's ellipse is the one tap, the clock is the
// study's, and the frame drawn in bands over the cores is the frame drawn on
// one thread.
//
// Every table here is a real one: a group game played into the bridge by
// phones, as BridgeKernelTests does, and begun on the one stage.

import QuartzCore
import UIKit
import XCTest
@testable import ChuiniuKit

@MainActor
final class StageViewTests: XCTestCase {
    private var stores: [String: UserDefaults] = [:]
    private static let names = ["Alex", "Bo", "Cy", "Di", "Ed", "Fay"]
    private static let seed: [UInt8] = (0..<32).map { UInt8(($0 * 13 + 5) & 0xFF) }

    /// The study's four drawers: the collapsed 6.9-inch surface, a small
    /// phone's expanded drawer, the 17e's and the Pro Max's.
    private static let sizes = [CGSize(width: 390, height: 340), CGSize(width: 375, height: 541),
                                CGSize(width: 390, height: 718), CGSize(width: 430, height: 830)]

    private func phone(_ name: String) -> BridgeKernel {
        let suite = "chuiniu.stagetests.\(name).\(ObjectIdentifier(self).hashValue)"
        let store = stores[name] ?? {
            let d = UserDefaults(suiteName: suite)!
            d.removePersistentDomain(forName: suite)
            stores[name] = d
            return d
        }()
        let k = BridgeKernel(store: store, devPerson: false)
        k.me(Data(repeating: UInt8(name.utf8.first!), count: 16))
        k.nickname(name)
        k.sender(nil, isDM: false, iSent: false)
        return k
    }

    /// A group game of `seats` just started, resident on Alex's phone (seat 0,
    /// whose turn it is: the picker's shelf is up).
    @discardableResult
    private func started(seats: Int) throws -> BridgeKernel {
        let alex = phone("Alex")
        XCTAssertTrue(alex.newGame(dm: false, seed: Self.seed))
        var link = try XCTUnwrap(alex.stagedURL())
        for s in 1..<seats {
            let p = phone(Self.names[s])
            XCTAssertEqual(p.adoptBubble(link), 0)
            XCTAssertTrue(p.join(name: Self.names[s]))
            link = try XCTUnwrap(p.stagedURL())
        }
        if seats < 6 {
            let a = phone("Alex")
            XCTAssertEqual(a.adoptBubble(link), 0)
            XCTAssertTrue(a.start())
            link = try XCTUnwrap(a.stagedURL())
        }
        let me = phone("Alex")
        XCTAssertEqual(me.adoptBubble(link), 0)
        XCTAssertEqual(me.table.phase, .bidding, "\(seats) seats: started")
        XCTAssertEqual(me.table.me, 0)
        XCTAssertNotNil(me.table.menu, "my turn")
        return me
    }

    private func request(_ drawer: CGSize, table: TableModel) -> StageRequest {
        StageRequest(screen: .table, drawer: drawer, scale: 3, roll: table.rollPending, rollID: table.rollID, table: StageTableKey(table))
    }

    // MARK: the camera

    /// THE SIGN, CHECKED ON THE GLASS (package B verified it only on paper):
    /// the layer turned by `StageDirector.tilt` puts every flat point where
    /// the kernel's homography (cn_cam_map's map) does, at the four study
    /// sizes; the kernel's `ca` is Core Animation's own perspective, rotation
    /// and scale built from theta, D and zoom; and the turn's origin stays put.
    func testTheTurnedLayerMapsEveryPointAsTheKernelsCamera() throws {
        try started(seats: 6)
        let stage = KernelSeam.stage()
        for size in Self.sizes {
            let hud = try XCTUnwrap(stage.begin(.table, drawer: size, scale: 3), "\(size)")
            let tag = "\(Int(size.width))x\(Int(size.height))"
            XCTAssertGreaterThan(abs(hud.theta), 0.01, "\(tag): the camera turns")

            // the kernel's ca is CA's composition (cn_cam.h's recipe)
            var t = CATransform3DIdentity
            t.m34 = -1 / hud.camD
            t = CATransform3DConcat(CATransform3DMakeRotation(hud.theta, 1, 0, 0), t)
            t = CATransform3DConcat(CATransform3DMakeScale(hud.zoom, hud.zoom, 1), t)
            let recipe = [t.m11, t.m12, t.m13, t.m14, t.m21, t.m22, t.m23, t.m24,
                          t.m31, t.m32, t.m33, t.m34, t.m41, t.m42, t.m43, t.m44]
            for (i, (a, b)) in zip(recipe, hud.ca).enumerated() {
                XCTAssertEqual(a, b, accuracy: 1e-5, "\(tag): ca[\(i)] is CA's perspective * rotateX(theta) * scale(zoom)")
            }

            // the layer the host turns, exactly as StageUIView builds it
            let parent = CALayer()
            parent.frame = CGRect(origin: .zero, size: size)
            let tilt = CALayer()
            tilt.anchorPoint = .zero
            tilt.position = .zero
            tilt.bounds = CGRect(origin: .zero, size: size)
            tilt.transform = StageDirector.tilt(hud)
            parent.addSublayer(tilt)

            let origin = CGPoint(x: hud.originX, y: hud.originY)
            let o = tilt.convert(origin, to: parent)
            XCTAssertEqual(o.x, origin.x, accuracy: 0.01, "\(tag): the turn's origin stays put")
            XCTAssertEqual(o.y, origin.y, accuracy: 0.01, "\(tag): the turn's origin stays put")

            var points = [origin]
            for s in 0..<hud.seats { points.append(CGPoint(x: hud.cupX[s], y: hud.cupY[s])) }
            for fx in stride(from: 0.0, through: 1.0, by: 0.25) {
                for fy in stride(from: 0.0, through: 1.0, by: 0.25) {
                    points.append(CGPoint(x: size.width * fx, y: size.height * fy))
                }
            }
            for p in points {
                let painted = tilt.convert(p, to: parent)
                let kernel = StageDirector.glass(p, hud)
                XCTAssertEqual(painted.x, kernel.x, accuracy: 0.05, "\(tag): \(p) lands at x the kernel's camera says")
                XCTAssertEqual(painted.y, kernel.y, accuracy: 0.05, "\(tag): \(p) lands at y the kernel's camera says")
            }
            // the far side shrinks away: a point above the origin comes closer to it
            let above = tilt.convert(CGPoint(x: origin.x, y: origin.y - 200), to: parent)
            XCTAssertLessThan(origin.y - above.y, 200 * hud.zoom, "\(tag): the far side leans away")

            // my cup's mouth, turned, is inside my cup's tap ellipse
            let mine = tilt.convert(CGPoint(x: hud.cupX[hud.me], y: hud.cupY[hud.me]), to: parent)
            XCTAssertTrue(StageDirector.inside(mine, hit: hud.hit), "\(tag): my cup's mouth is under my tap")
        }
    }

    // MARK: the planks cover the view

    /// How far `p` lies inside the quad `q` (corners in turn order): the least
    /// of its distances to the four edges, negative outside.
    private static func insideBy(_ q: [CGPoint], _ p: CGPoint) -> CGFloat {
        var area: CGFloat = 0
        for k in 0..<4 { area += q[k].x * q[(k + 1) % 4].y - q[(k + 1) % 4].x * q[k].y }
        let sgn: CGFloat = area > 0 ? 1 : -1
        var least = CGFloat.greatestFiniteMagnitude
        for k in 0..<4 {
            let a = q[k], b = q[(k + 1) % 4]
            let len = hypot(b.x - a.x, b.y - a.y)
            least = min(least, sgn * ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) / len)
        }
        return least
    }

    /// THE BAND ABOVE THE PLANKS (s03_six_expanded.png: a bare strip between
    /// the drawer's rounded top and the first planks on a 440 by 956 phone).
    /// The view built as the table builds it, at the study's drawers and the
    /// shortest and tallest, with Messages' insets and with the kernel's whole
    /// reach as insets: the planks' layer, turned, holds every corner of the
    /// view (the drawer and the safe areas it reaches under) with room to
    /// spare. The planks are tiles of the one image and nothing has a bitmap
    /// of its own.
    func testThePlanksTurnedCoverTheWholeViewAtEveryDrawer() throws {
        let me = try started(seats: 6)
        let sizes = Self.sizes + [CGSize(width: 440, height: 281), CGSize(width: 375, height: 340),
                                  CGSize(width: 440, height: 718), CGSize(width: 440, height: 956)]
        let reach = CGFloat(CN_CAM_REACH)
        let insets = [UIEdgeInsets(top: 32, left: 0, bottom: 34, right: 0),
                      UIEdgeInsets(top: reach, left: reach, bottom: reach, right: reach)]
        let image = try XCTUnwrap(CnTextures.planks?.cgImage)
        for size in sizes {
            for inset in insets {
                let tag = "\(Int(size.width))x\(Int(size.height)) inset \(Int(inset.top))"
                let director = StageDirector(stage: KernelSeam.stage())
                let full = CGRect(x: 0, y: 0, width: size.width + inset.left + inset.right, height: size.height + inset.top + inset.bottom)
                let window = UIWindow(frame: full)
                let view = StageUIView(director: director)
                view.frame = full
                view.inset = inset
                window.addSubview(view)
                director.begin(request(size, table: me.table), planMs: nil)
                let hud = try XCTUnwrap(director.hud, tag)
                view.update(names: [], outWord: "", hud: hud)
                view.layoutIfNeeded()
                XCTAssertEqual(view.drawer.size, size, tag)

                // the layer is the kernel's rect; on a tall drawer it runs up past the study's overdraw (on a
                // short one it is smaller than the overdraw: the camera hardly turns there)
                XCTAssertEqual(view.planks.frame, StageUIView.plankRect(hud, size), tag)
                if size.height >= 718 {
                    XCTAssertLessThan(view.planks.frame.minY, StageUIView.overdraw(size).minY, "\(tag): the planks run up past the study's overdraw")
                }

                // its four corners, turned, hold the whole view by room to spare
                let b = view.planks.bounds
                let quad = [CGPoint(x: b.minX, y: b.minY), CGPoint(x: b.maxX, y: b.minY),
                            CGPoint(x: b.maxX, y: b.maxY), CGPoint(x: b.minX, y: b.maxY)].map { view.planks.convert($0, to: view.layer) }
                let v = view.bounds
                for c in [CGPoint(x: v.minX, y: v.minY), CGPoint(x: v.maxX, y: v.minY), CGPoint(x: v.maxX, y: v.maxY), CGPoint(x: v.minX, y: v.maxY)] {
                    XCTAssertGreaterThanOrEqual(Self.insideBy(quad, c), 4, "\(tag): the view's corner \(c) is under the turned planks (quad \(quad))")
                }

                // memory: tiles of the one image, no bitmap of the layer's own
                let tiles = view.planks.sublayers?.filter { $0.name == "tile" } ?? []
                XCTAssertNil(view.planks.contents, "\(tag): the planks' layer has no bitmap")
                XCTAssertGreaterThan(tiles.count, 0, tag)
                XCTAssertLessThanOrEqual(tiles.count, 90, "\(tag): \(tiles.count) tiles")
                XCTAssertTrue(tiles.allSatisfy { ($0.contents as! CGImage) === image && $0.bounds.size == PlankTile.size }, "\(tag): every tile is the one image")
                // the tiles cover the layer: the first at or above-left of its corner, the last past its far one
                let union = tiles.reduce(CGRect.null) { $0.union($1.frame) }
                XCTAssertTrue(union.contains(b), "\(tag): the tiles cover the layer (\(union) of \(b))")
                // and they keep the study's phase in the drawer: a tile corner where the study put one
                let phase = StageUIView.overdraw(size).origin
                let o = StageUIView.tileOrigin(size)
                let corner = view.planks.convert(tiles[0].frame.origin, to: view.tilt)
                let dx = (corner.x - (phase.x + o.x)) / PlankTile.size.width, dy = (corner.y - (phase.y + o.y)) / PlankTile.size.height
                XCTAssertEqual(dx, dx.rounded(), accuracy: 1e-6, "\(tag): the tiles' phase across is the study's")
                XCTAssertEqual(dy, dy.rounded(), accuracy: 1e-6, "\(tag): the tiles' phase down is the study's")
                // under it, the same wood, never black
                XCTAssertGreaterThan(view.back.sublayers?.filter { $0.name == "tile" }.count ?? 0, 0, "\(tag): planks under the turned layer")
                XCTAssertTrue(view.back.sublayers?.allSatisfy { $0.name == "tile" } ?? false, "\(tag): and nothing over them")
                window.isHidden = true
            }
        }
    }

    // MARK: no size, no frame

    /// A stage spy: the real stage, counted.
    private final class Counting: TableStage {
        let real = KernelSeam.stage()
        var begins = 0, frames = 0, named = 0
        func begin(_ screen: StageScreen, drawer: CGSize, scale: CGFloat) -> CnStageHudSnap? {
            begins += 1
            return real.begin(screen, drawer: drawer, scale: scale)
        }
        func frame(atMs ms: Int, peek: Double) -> StageFrame? { frames += 1; return real.frame(atMs: ms, peek: peek) }
        func submit(atMs ms: Int, peek: Double, then done: @escaping @MainActor (StageFrame?) -> Void) {
            frames += 1
            real.submit(atMs: ms, peek: peek, then: done)
        }
        func frameOnOneThread(atMs ms: Int, peek: Double) -> StageFrame? { real.frameOnOneThread(atMs: ms, peek: peek) }
        func peekEase(_ t: Double) -> Double { real.peekEase(t) }
        func done(atMs ms: Int) -> Bool { real.done(atMs: ms) }
        func purge() { real.purge() }
        func bubble(scale: CGFloat) -> BubbleFrame? { real.bubble(scale: scale) }
        func name(seat: Int, bitmap: NameBitmap?) { named += 1; real.name(seat: seat, bitmap: bitmap) }
        var holdsArena: Bool { real.holdsArena }
        var drawer: CGSize? { real.drawer }
    }

    func testAViewWithNoSizeNeverAsksForAFrame() throws {
        let me = try started(seats: 2)
        me.rollSeen(rollID: me.table.rollID)            // the round's throw already watched: a still table
        let spy = Counting()
        let director = StageDirector(stage: spy)
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 390, height: 718))
        let view = StageUIView(director: director)
        view.frame = .zero
        window.addSubview(view)
        window.isHidden = false

        director.begin(request(.zero, table: me.table), planMs: nil)
        director.begin(request(CGSize(width: 390, height: 0), table: me.table), planMs: nil)
        view.layoutIfNeeded()
        view.wake()
        director.advance(0.016)
        director.requestFrame()
        XCTAssertEqual(spy.begins, 0, "nothing begun for a drawer with no size")
        XCTAssertEqual(spy.frames, 0, "no frame for a view with no size")

        // the same view given a size draws (so the count above could go up)
        view.frame = CGRect(x: 0, y: 0, width: 390, height: 718)
        director.begin(request(view.bounds.size, table: me.table), planMs: nil)
        view.layoutIfNeeded()
        view.wake()
        XCTAssertEqual(spy.begins, 1)
        XCTAssertEqual(spy.frames, 1, "one still frame, drawn once")
        drain(director)
        XCTAssertNotNil(director.last, "it landed")
        XCTAssertEqual(view.canvas.frame.width, CGFloat(director.last?.shot.canvas[2] ?? 0), "at the shot's canvas")

        // shrunk to nothing: the link stops and nothing more is asked
        view.frame = .zero
        view.layoutIfNeeded()
        director.setPeek(open: true, animated: true)
        view.wake()
        drain(director)
        XCTAssertEqual(spy.frames, 1, "a view shrunk to nothing asks for no frame")
        window.isHidden = true
    }

    /// The main run loop turned until the director's frame in flight has
    /// landed (a frame lands on the main queue).
    private func drain(_ d: StageDirector, file: StaticString = #filePath, line: UInt = #line) {
        let until = Date().addingTimeInterval(20)
        while d.inFlight, Date() < until { RunLoop.main.run(until: Date().addingTimeInterval(0.002)) }
        XCTAssertFalse(d.inFlight, "the frame landed", file: file, line: line)
    }
    private static func bytes(_ f: StageFrame?) -> Data { (f?.image.dataProvider?.data as Data?) ?? Data() }

    // MARK: the tap

    func testMyCupsEllipseIsTheOneTapAndItTipsTheCupByTheKernelsEase() throws {
        let me = try started(seats: 4)
        me.rollSeen(rollID: me.table.rollID)            // the round's throw already watched
        let director = StageDirector(stage: KernelSeam.stage())
        director.begin(request(CGSize(width: 390, height: 718), table: me.table), planMs: nil)
        let hud = try XCTUnwrap(director.hud)
        XCTAssertTrue(director.atRest, "no throw: at rest")
        let centre = CGPoint(x: hud.hit[0], y: hud.hit[1])

        XCTAssertTrue(director.tap(at: centre), "my cup is tapped")
        XCTAssertTrue(director.peekOpen, "the tap tips it up")
        XCTAssertTrue(director.tap(at: CGPoint(x: hud.hit[0] + hud.hit[2] * 0.95, y: hud.hit[1])), "inside its edge")
        XCTAssertFalse(director.peekOpen, "the tap again sets it down")

        XCTAssertFalse(director.tap(at: CGPoint(x: hud.hit[0] + hud.hit[2] * 1.05, y: hud.hit[1])), "just past its edge")
        XCTAssertFalse(director.tap(at: CGPoint(x: hud.hit[0], y: hud.hit[1] - hud.hit[3] * 1.05)), "just above it")
        for s in 1..<hud.seats {
            let far = StageDirector.glass(CGPoint(x: hud.cupX[s], y: hud.cupY[s]), hud)
            XCTAssertFalse(director.tap(at: far), "seat \(s)'s cup is not mine to tip")
        }
        XCTAssertFalse(director.peekOpen, "nothing but my cup moved it")

        // the tween is the kernel's: halfway through CN_PEEK_MS, cn_api_peek_ease(.5)
        director.setPeek(open: true, animated: true)
        let now = CACurrentMediaTime()
        let half = director.peekValue(now + Double(CN_PEEK_MS) / 2000)
        XCTAssertEqual(half, director.stage.peekEase(0.5), accuracy: 0.02, "halfway, the kernel's ease")
        XCTAssertEqual(director.peekValue(now + Double(CN_PEEK_MS) / 1000 + 0.01), 1, "then all the way")
    }

    // MARK: the clock

    func testTheThrowsClockIsTheStudysAndNothingIsStagedBeforeMyDiceRest() throws {
        // six seats on a tall drawer: the far cups whose held cup stays inside throw too (package V2), so
        // everything comes to rest after my dice do
        let me = try started(seats: 6)
        let director = StageDirector(stage: KernelSeam.stage())
        var done = 0
        director.onRollDone = { done += 1 }
        director.begin(request(CGSize(width: 375, height: 900), table: me.table), planMs: nil)
        let hud = try XCTUnwrap(director.hud)
        XCTAssertEqual(hud.rolls, 1)
        XCTAssertGreaterThan(hud.totalMs, hud.restMs, "far cups throw at 375 by 900 and outlast mine")
        XCTAssertEqual(director.clockMs, Double(hud.rollAtMs), "the throw starts at the HUD's roll_at")
        XCTAssertFalse(director.atRest, "my dice are in the air")
        XCTAssertFalse(director.tap(at: CGPoint(x: hud.hit[0], y: hud.hit[1])), "no peek while they are")
        XCTAssertTrue(director.live)

        // a hitch of a whole second moves the clock 50 ms, as the study's
        director.advance(1.0)
        XCTAssertEqual(director.clockMs, Double(hud.rollAtMs) + 50, accuracy: 0.001, "dt clamped to .05")

        while director.clockMs < Double(hud.restMs) - 20 { director.advance(0.016) }
        XCTAssertFalse(director.atRest, "just before rest_ms nothing may be staged")
        while director.clockMs < Double(hud.restMs) { director.advance(0.016) }
        XCTAssertTrue(director.atRest, "at rest_ms my dice are still")
        XCTAssertEqual(done, 0, "the far cups may still be rolling")
        while director.live { director.advance(0.016) }
        XCTAssertGreaterThanOrEqual(director.clockMs, Double(hud.totalMs))
        XCTAssertEqual(done, 1, "the throw is reported played once")
        director.advance(0.016)
        XCTAssertEqual(done, 1)
    }

    /// REDUCE MOTION: the throw is at its end from the begin, so the director
    /// reports it watched there (TableScreen hands that to the kernel; there
    /// is no second report of its own), once.
    func testUnderReduceMotionTheThrowIsReportedWatchedAtTheBegin() throws {
        let me = try started(seats: 2)
        let director = StageDirector(stage: KernelSeam.stage())
        director.reduceMotion = true
        var done = 0
        director.onRollDone = { done += 1 }
        let r = request(CGSize(width: 390, height: 718), table: me.table)
        XCTAssertTrue(r.roll, "the round is pending")
        director.begin(r, planMs: nil)
        XCTAssertEqual(try XCTUnwrap(director.hud).rolls, 1)
        XCTAssertEqual(done, 1, "reported from the begin")
        XCTAssertTrue(director.atRest)
        director.advance(0.016)
        XCTAssertEqual(done, 1, "once")
        director.stage.purge()
    }

    // MARK: the bands

    /// The frame drawn in CN_STAGE_BANDS bands with concurrentPerform (the
    /// bridge's) is byte for byte the frame cn_api_stage_frame draws on one
    /// thread: a throw frame and a peeking still frame.
    func testTheFrameInBandsIsTheFrameOnOneThread() throws {
        try started(seats: 6)
        let stage = KernelSeam.stage()
        let hud = try XCTUnwrap(stage.begin(.table, drawer: CGSize(width: 375, height: 541), scale: 3))
        for (ms, peek) in [(hud.rollAtMs + 1200, 0.0), (hud.totalMs, 1.0)] {
            let banded = try XCTUnwrap(stage.frame(atMs: ms, peek: peek))
            let bytes = banded.shot.w * banded.shot.h * 4
            let a = try XCTUnwrap(banded.image.dataProvider?.data as Data?)
            let one = try XCTUnwrap(stage.frameOnOneThread(atMs: ms, peek: peek))
            let b = try XCTUnwrap(one.image.dataProvider?.data as Data?)
            XCTAssertEqual(a.count, bytes)
            XCTAssertTrue(a == b, "at \(ms) ms, peek \(peek): the bands' bytes are one thread's")
        }
        stage.purge()
    }

    // MARK: off the main thread

    /// THE STAGE'S QUEUE: a submitted frame is drawn off the main thread (the
    /// submit returns before it lands, and it lands on the main thread), one
    /// at a time, in the order asked, each the bytes `frame` draws at once at
    /// the same clock; and the picture is Core Animation's own form,
    /// premultiplied BGRA, tagged so.
    func testSubmittedFramesAreDrawnOffTheMainThreadOneAtATimeInOrder() throws {
        try started(seats: 6)
        let stage = try XCTUnwrap(KernelSeam.stage() as? BridgeStage)
        let hud = try XCTUnwrap(stage.begin(.table, drawer: CGSize(width: 375, height: 541), scale: 3))
        _ = stage.drawsAtOnce
        let asked = (0..<4).map { hud.rollAtMs + 150 * $0 }
        var landed: [(ms: Int, frame: StageFrame?, onMain: Bool)] = []
        for ms in asked {
            stage.submit(atMs: ms, peek: 0) { f in landed.append((ms, f, Thread.isMainThread)) }
        }
        XCTAssertTrue(landed.isEmpty, "the submits returned before any frame was drawn")
        let until = Date().addingTimeInterval(20)
        while landed.count < asked.count, Date() < until { RunLoop.main.run(until: Date().addingTimeInterval(0.002)) }
        XCTAssertEqual(landed.map(\.ms), asked, "landed in the order asked")
        XCTAssertTrue(landed.allSatisfy(\.onMain), "each lands on the main thread")
        XCTAssertEqual(stage.drawsAtOnce, 1, "never two frames drawing at once")
        for l in landed {
            let now = try XCTUnwrap(stage.frame(atMs: l.ms, peek: 0))
            XCTAssertTrue(Self.bytes(l.frame) == Self.bytes(now) && !Self.bytes(now).isEmpty, "at \(l.ms) ms: the queue's bytes are the bytes drawn at once")
        }
        // premultiplied: tagged so, and no colour past its pixel's alpha
        let f = try XCTUnwrap(landed.last?.frame)
        XCTAssertEqual(f.image.alphaInfo, .premultipliedFirst)
        XCTAssertEqual(f.image.byteOrderInfo, .order32Little)
        let px = [UInt8](Self.bytes(f))
        var over = 0, partial = 0
        for i in stride(from: 0, to: px.count, by: 4) {
            let a = px[i + 3]
            if px[i] > a || px[i + 1] > a || px[i + 2] > a { over += 1 }
            if a > 0 && a < 255 { partial += 1 }
        }
        XCTAssertEqual(over, 0, "premultiplied: no colour past its alpha")
        XCTAssertGreaterThan(partial, 1000, "the table's shadow and the edges are part-covered")
        stage.purge()
    }

    /// A PURGE WHILE A FRAME DRAWS waits for it: the frame lands whole, the
    /// arena is gone after, and the next frame (a new arena) draws the same
    /// bytes.
    func testAPurgeDuringADrawWaitsForItAndTheNextFrameIsTheSame() throws {
        try started(seats: 6)
        let stage = KernelSeam.stage()
        let hud = try XCTUnwrap(stage.begin(.table, drawer: CGSize(width: 430, height: 830), scale: 3))
        let ms = hud.rollAtMs + 600
        var landed: StageFrame??
        stage.submit(atMs: ms, peek: 0) { landed = .some($0) }
        stage.purge()
        XCTAssertFalse(stage.holdsArena, "purged")
        let until = Date().addingTimeInterval(20)
        while landed == nil, Date() < until { RunLoop.main.run(until: Date().addingTimeInterval(0.002)) }
        let drawn = try XCTUnwrap(landed ?? nil, "the frame in flight was drawn before the arena went")
        let again = try XCTUnwrap(stage.frame(atMs: ms, peek: 0))
        XCTAssertTrue(stage.holdsArena, "the next frame took a new arena")
        XCTAssertEqual(Self.bytes(drawn), Self.bytes(again), "the same bytes")
        stage.purge()
    }

    /// THE DIRECTOR KEEPS ONE FRAME IN FLIGHT, at the clock it asked: a second
    /// ask while one draws asks nothing, the frame that lands is the one asked
    /// at the clock as it was then (however far the clock has run since), and
    /// it is the bytes the stage draws on one thread at that clock, for a
    /// throw and for a still, peeking frame.
    func testTheDirectorKeepsOneFrameInFlightAtTheClockItAsked() throws {
        let me = try started(seats: 4)
        let director = StageDirector(stage: KernelSeam.stage())
        director.begin(request(CGSize(width: 390, height: 718), table: me.table), planMs: nil)
        let hud = try XCTUnwrap(director.hud)
        while director.clockMs < Double(hud.rollAtMs) + 400 { director.advance(0.016) }
        XCTAssertTrue(director.requestFrame(), "a frame is asked")
        let asked = Int(director.clockMs.rounded(.down))
        director.advance(0.016)
        XCTAssertTrue(director.needsFrame, "the clock moved: another frame is due")
        XCTAssertFalse(director.requestFrame(), "but not while one is in flight")
        XCTAssertEqual(director.framesAsked, 1)
        director.advance(0.016)
        drain(director)
        XCTAssertEqual(director.landedMs, [asked], "the frame landed is the one asked, at the clock it was asked at")
        let throwFrame = try XCTUnwrap(director.last)
        XCTAssertEqual(Self.bytes(throwFrame), Self.bytes(director.stage.frameOnOneThread(atMs: asked, peek: 0)), "the throw frame's bytes")
        XCTAssertTrue(director.requestFrame(), "landed: the next one may be asked")
        drain(director)

        // the still frame, peeking
        while director.live { director.advance(0.016) }
        director.setPeek(open: true, animated: false)
        XCTAssertTrue(director.requestFrame())
        let still = Int(director.clockMs.rounded(.down))
        drain(director)
        XCTAssertEqual(director.landedMs.last, still)
        XCTAssertEqual(Self.bytes(director.last), Self.bytes(director.stage.frameOnOneThread(atMs: still, peek: 1)), "the still frame's bytes")
        director.stage.purge()
    }

    /// A FRAME OF AN OLDER BEGIN IS DROPPED: begun again (the drawer changed)
    /// while a frame draws, the frame that lands is not put up; the new
    /// screen's is.
    func testAFrameAskedBeforeABeginIsDropped() throws {
        let me = try started(seats: 3)
        me.rollSeen(rollID: me.table.rollID)            // the round's throw already watched: still frames
        let director = StageDirector(stage: KernelSeam.stage())
        var shown: [StageFrame] = []
        director.onFrame = { shown.append($0) }
        director.begin(request(CGSize(width: 390, height: 718), table: me.table), planMs: nil)
        XCTAssertTrue(director.requestFrame())
        director.begin(request(CGSize(width: 375, height: 541), table: me.table), planMs: nil)
        drain(director)
        XCTAssertTrue(shown.isEmpty, "the 390 by 718 frame landed after the begin and was not put up")
        XCTAssertNil(director.last)
        XCTAssertTrue(director.requestFrame(), "the new screen's frame")
        drain(director)
        XCTAssertEqual(shown.count, 1)
        let f = try XCTUnwrap(director.last)
        XCTAssertEqual(Self.bytes(f), Self.bytes(director.stage.frameOnOneThread(atMs: 0, peek: 0)), "it is the 375 by 541 table's")
        director.stage.purge()
    }

    // MARK: the names on the table (package N)

    /// A NAME'S PICTURE (NameDecal.render): the block the old layers laid out
    /// (the letters' box, a 3-point gap, the 2-point bar) with the halo round
    /// it, three texels a point, inside the stage's maxima, premultiplied, not
    /// empty; the turn's (bright, with its bar) is not the same name dim, and
    /// only the turn's has the bar's glow under the letters.
    func testANamesPictureIsItsLettersAndItsBar() throws {
        func look(_ turn: Bool, _ name: String = "Alex", short: Bool = false) -> NameDecal.Look {
            NameDecal.look(StageName(seat: SeatModel(id: 1, name: name, dice: 5, alive: true, isTurn: turn, isMe: false)), short: short, how: CN_NAME_BOX)
        }
        let halo = CGFloat(CN_STAGE_NAME_HALO)
        let bright = try XCTUnwrap(NameDecal.render(look(true))), dim = try XCTUnwrap(NameDecal.render(look(false)))
        let (textW, textH) = NameDecal.block("Alex", short: false)
        XCTAssertEqual(bright.wPt, textW + 2 * halo, "the letters' box and the halo either side")
        XCTAssertEqual(bright.hPt, textH + NameDecal.gap + NameDecal.barH + 2 * halo, "the letters, the gap, the bar and the halo")
        XCTAssertEqual(bright.w, Int((bright.wPt * 3).rounded(.up)), "three texels a point")
        XCTAssertEqual(bright.h, Int((bright.hPt * 3).rounded(.up)))
        XCTAssertEqual(bright.rgba.count, bright.w * bright.h * 4)
        XCTAssertEqual(dim.w, bright.w, "the same block bright or dim")
        func stats(_ b: NameBitmap, rows: Range<Int>) -> (lit: Int, over: Int) {
            var lit = 0, over = 0
            for y in rows { for x in 0..<b.w {
                let i = (y * b.w + x) * 4, a = b.rgba[i + 3]
                if a > 0 { lit += 1 }
                if b.rgba[i] > a || b.rgba[i + 1] > a || b.rgba[i + 2] > a { over += 1 }
            } }
            return (lit, over)
        }
        let all = stats(bright, rows: 0..<bright.h)
        XCTAssertGreaterThan(all.lit, 500, "the letters are drawn (\(all.lit) texels)")
        XCTAssertEqual(all.over, 0, "premultiplied: no colour past its alpha")
        XCTAssertNotEqual(bright.rgba, dim.rgba, "the turn's name is not the dim one")
        // the bar's rows: under the letters, between the gap and the halo
        let k = CGFloat(bright.h) / bright.hPt
        let barRows = Int(((halo + textH + NameDecal.gap) * k).rounded(.down))..<Int(((halo + textH + NameDecal.gap + NameDecal.barH) * k).rounded(.up))
        XCTAssertGreaterThan(stats(bright, rows: barRows).lit, 60, "the turn's bar")
        XCTAssertEqual(stats(dim, rows: barRows).lit, 0, "no bar off the turn")
        // a short board's 12 is a smaller block; a long name keeps inside the maxima
        XCTAssertLessThan(try XCTUnwrap(NameDecal.render(look(true, short: true))).hPt, bright.hPt, "12 on a short board")
        let long = try XCTUnwrap(NameDecal.render(look(true, String(repeating: "Maximilian", count: 4))))
        XCTAssertLessThanOrEqual(long.w, CN_STAGE_NAME_W_MAX)
        XCTAssertLessThanOrEqual(long.h, CN_STAGE_NAME_H_MAX)
        XCTAssertEqual(long.wPt, 150 + 2 * halo, "a long name's box is 150 at most, as the old layer's")
        // for the eye: the bright name as a picture
        if let provider = CGDataProvider(data: Data(bright.rgba) as CFData),
           let image = CGImage(width: bright.w, height: bright.h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: bright.w * 4,
                               space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                               provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent),
           let png = UIImage(cgImage: image).pngData() {
            let url = FileManager.default.temporaryDirectory.appendingPathComponent("chuiniu_name_bright.png")
            try? png.write(to: url)
            print("the bright name: \(url.path)")
        }
    }

    /// THE SEAM: each seat's name is handed to the stage once, and again only
    /// when its look changes (the turn moves: two seats); a seat that goes is
    /// taken away; the same names again hand over nothing. And through the
    /// real stage the name is in the frame, and taken away the frame is the
    /// one before it.
    func testANameIsHandedToTheStageOnlyWhenItChanges() throws {
        let me = try started(seats: 3)
        let spy = Counting()
        let director = StageDirector(stage: spy)
        let view = StageUIView(director: director)
        director.begin(request(CGSize(width: 390, height: 718), table: me.table), planMs: nil)
        let hud = try XCTUnwrap(director.hud)
        let before = Self.bytes(spy.frameOnOneThread(atMs: 0, peek: 0))
        var names = me.table.seats.map { StageName(seat: $0) }
        view.update(names: names, outWord: "out", hud: hud)
        XCTAssertEqual(spy.named, 3, "three names handed over")
        view.update(names: names, outWord: "out", hud: hud)
        XCTAssertFalse(view.decals.update(names, hud: hud, stage: spy), "the same names: nothing to hand over")
        XCTAssertEqual(spy.named, 3, "the same names again: none")
        let named = Self.bytes(spy.frameOnOneThread(atMs: 0, peek: 0))
        XCTAssertNotEqual(named, before, "the names are in the frame")
        names[0].isTurn = false
        names[1].isTurn = true
        view.update(names: names, outWord: "out", hud: hud)
        XCTAssertEqual(spy.named, 5, "the turn moved: the two seats' names again")
        view.update(names: Array(names.prefix(2)), outWord: "out", hud: hud)
        XCTAssertEqual(spy.named, 6, "a seat gone: its name taken away")
        for s in 0..<2 { spy.name(seat: s, bitmap: nil) }
        XCTAssertEqual(Self.bytes(spy.frameOnOneThread(atMs: 0, peek: 0)), before, "every name taken away: the frame before them")
        spy.purge()
    }

    // MARK: the loser's stamp (package S)

    func testTheStampStaysInsideTheDrawerAndOffTheOtherNames() {
        let bounds = CGRect(x: 0, y: 0, width: 390, height: 340)
        let size = CGSize(width: 120, height: 22)
        let edge = CGFloat(CN_STAGE_EDGE)
        // a seat at the left: centred under its name it would start at -30
        let left = StageUIView.stampFrame(under: CGRect(x: 10, y: 100, width: 40, height: 20), size: size, bounds: bounds, avoid: [])
        XCTAssertEqual(left.minX, edge, "pushed right to lie inside the drawer's left edge")
        XCTAssertEqual(left.minY, 124, "still 4 under its name")
        // and at the right
        let right = StageUIView.stampFrame(under: CGRect(x: 350, y: 100, width: 40, height: 20), size: size, bounds: bounds, avoid: [])
        XCTAssertEqual(right.maxX, bounds.maxX - edge, "pushed left to lie inside the drawer's right edge")
        // under it, a name in the way: the stamp goes under that one
        let other = CGRect(x: 120, y: 118, width: 40, height: 20)
        let mid = StageUIView.stampFrame(under: CGRect(x: 160, y: 96, width: 40, height: 20), size: size, bounds: bounds, avoid: [other])
        XCTAssertFalse(mid.intersects(other), "off the other name (\(mid))")
        XCTAssertEqual(mid.minY, other.maxY + 2)
        XCTAssertEqual(mid.midX, 180, "still centred under its own name")
    }
}
