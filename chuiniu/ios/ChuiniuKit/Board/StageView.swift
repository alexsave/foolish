// StageView.swift - THE TABLE IS THE KERNEL'S PICTURE (cn_stage.h). This file
// owns a clock, a display link, the layers the picture goes on and one tap;
// every place, every turn of the camera, every frame's pixels and every time
// (when the roll starts, when my dice rest, when everything is still) comes
// from the stage through the seam (`TableStage`, `CnStageHudSnap`).
//
// THE LAYERS (chuiniu/docs/UI.html, `.scr`, `.stagewrap`, `.hud`):
//   tilt     the planks, the names and the picture's canvas, turned together
//            by the HUD's `ca_screen` about the drawer's (0, 0): the study's
//            `perspective(D) rotateX(theta) scale(zoom)` about my cup
//   planks   the baked wood under it all, 1.9 by 2.2 of the drawer (the
//            study's overdraw), so the turn uncovers planks and never a gap
//   canvas   the kernel's frame at the shot's canvas (flat points)
//   names    on the planks under the cups (tilted with them)
// The HUD (the plate, the picker, Next round) is SwiftUI over this view and
// is never turned.
//
// THE CLOCK is the study's: each display frame adds the time since the last,
// clamped to 50 ms, so a hitch slows the throw instead of skipping it. It is
// the stage's clock (DECISIONS I21): the throw starts at the HUD's roll_at_ms
// and my dice are at rest at rest_ms; nothing is staged before that.
//
// WHEN IT DRAWS: while a throw moves, while the peek turns, while a reveal's
// cups lift; then one still frame, and the display link stops. A view with
// no size never asks for a frame. A purge (memory warning) keeps the last
// picture on screen; a frame the stage cannot draw keeps the previous one.

// SEAM REQUEST (package E1): cn_api_peek_ease is the one C call here, the
// peek's tween; it belongs on `TableStage` (as `peekEase(_:)`) and this import
// goes when it is there.
import CChuiniu
import QuartzCore
import SwiftUI
import UIKit

/// What a screen asks the stage to draw.
public struct StageRequest: Equatable {
    public var screen: StageScreen
    public var drawer: CGSize
    public var scale: CGFloat
    /// Throw the round (the screen has not played this round's roll yet).
    public var roll: Bool
    /// The round, so the same roll re-begun (a drawer that changed size
    /// mid-throw) keeps its clock.
    public var rollID: Int
    /// Anything the kernel's stage input depends on, so a change re-begins.
    public var table: StageTableKey

    public init(screen: StageScreen, drawer: CGSize, scale: CGFloat, roll: Bool, rollID: Int, table: StageTableKey) {
        self.screen = screen
        self.drawer = drawer
        self.scale = scale
        self.roll = roll
        self.rollID = rollID
        self.table = table
    }
}

/// The parts of the model the stage's input is made of (cn_api_stage_begin
/// reads them from the resident; this is only when to ask again).
public struct StageTableKey: Equatable {
    var phase: Phase
    var me: Int?
    var turn: Int?
    var counts: [Int]
    var myDice: [Int]
    var revealed: Bool

    public init(_ t: TableModel) {
        phase = t.phase
        me = t.me
        turn = t.seats.first(where: \.isTurn)?.id
        counts = t.seats.map(\.dice)
        myDice = t.myDice
        revealed = t.reveal != nil
    }
}

// MARK: - the director: the clock, the peek and the frames

@MainActor
public final class StageDirector: ObservableObject {
    public let stage: TableStage
    /// The begun screen's places, or nil (nothing begun, or the kernel had
    /// nothing to draw).
    @Published public private(set) var hud: CnStageHudSnap?
    /// My dice are at rest: a move may be staged (I21).
    @Published public private(set) var atRest = true
    /// My cup is tipped up (or tipping up).
    @Published public private(set) var peekOpen = false

    /// The system's Reduce Motion: no throw plays (the dice are where it
    /// leaves them) and the peek snaps.
    public var reduceMotion = false
    /// A reveal's plan still moves the cups at this stage time.
    public var liveAt: ((Int) -> Bool)?
    /// The throw ran to its end (every cup down, every die still).
    public var onRollDone: (() -> Void)?
    /// The view: something changed, the display link should run.
    var onWake: (() -> Void)?

    private(set) var request: StageRequest?
    /// The stage's clock, ms.
    private(set) var clockMs: Double = 0
    private var rollReported = false
    private var peekFrom = 0.0, peekTo = 0.0
    private var peekStart: CFTimeInterval?
    /// The newest picture the stage drew; kept when a frame cannot be drawn.
    private(set) var last: StageFrame?
    private var dirty = true
    private var wasLive = false
    /// How many frames this director asked the stage for (tests).
    private(set) var framesAsked = 0

    /// ONE STAGE A PROCESS (the renderer is one): the director that began it
    /// last owns it, and another never draws from it until it begins again.
    private static weak var owner: StageDirector?
    var owns: Bool { Self.owner === self }

    private var memoryObserver: NSObjectProtocol?

    public init(stage: TableStage) {
        self.stage = stage
        memoryObserver = NotificationCenter.default.addObserver(
            forName: UIApplication.didReceiveMemoryWarningNotification, object: nil, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.memoryWarning() }
        }
    }

    deinit {
        if let memoryObserver { NotificationCenter.default.removeObserver(memoryObserver) }
    }

    // MARK: begin

    /// Begin `r` (or keep what is begun when it is the same request). `planMs`
    /// is how far the kernel's current plan has run (nil: no plan playing).
    public func begin(_ r: StageRequest, planMs: Int?) { begin(r, planMs: planMs, again: false) }

    private func begin(_ r: StageRequest, planMs: Int?, again: Bool) {
        guard r.drawer.width >= 1, r.drawer.height >= 1 else { return }
        if r == request, owns, hud != nil { return }
        let previous = again ? r : request
        let sameRoll = previous?.rollID == r.rollID && previous?.screen == r.screen
        request = r
        Self.owner = self
        dirty = true
        let h = stage.begin(r.screen, drawer: r.drawer, scale: r.scale, roll: r.roll)
        hud = h
        guard let h else { atRest = true; return }

        if h.rolls == 1 {
            // the throw: from where the plan is when it has not reached the
            // SHAKE beat yet (every beat before it plays uncut), else from the
            // roll's start now; the same throw re-begun keeps its clock
            if !(sameRoll && previous?.roll == true && (again || clockMs >= Double(h.rollAtMs))) {
                clockMs = Double(min(planMs ?? Int.max, h.rollAtMs))
                rollReported = false
            }
            if reduceMotion { clockMs = max(clockMs, Double(h.totalMs)) }
        } else if r.screen == .reveal {
            // the lift's clock is the plan's; with no plan, the cups are up
            if !again { clockMs = Double(planMs ?? Self.longAfter) }
        } else {
            clockMs = 0
            rollReported = true
        }
        if !sameRoll || r.screen != .table { setPeek(open: false, animated: false) }
        updateRest()
        onWake?()
    }

    /// A ms past any plan's end: the settled look.
    static let longAfter = 600_000

    /// The view is on screen again: draw from the stage only after owning it.
    func claim() {
        guard !owns, let r = request else { return }
        begin(r, planMs: nil, again: true)
    }

    // MARK: the clock

    /// Something still moves: the display link runs.
    var live: Bool {
        guard let h = hud, owns else { return false }
        if h.rolls == 1, clockMs < Double(h.totalMs) { return true }
        if peekMoving(CACurrentMediaTime()) { return true }
        return liveAt?(Int(clockMs)) ?? false
    }

    /// One display frame `dt` seconds after the last. Whether a frame is due.
    @discardableResult
    func advance(_ dt: CFTimeInterval) -> Bool {
        guard hud != nil else { return false }
        let wasRolling = live
        clockMs += Self.clamp(dt) * 1000
        updateRest()
        let nowLive = live
        // a frame while anything moves, and one more as it comes to rest
        if wasRolling || nowLive || wasLive { dirty = true }
        wasLive = nowLive
        return dirty
    }

    /// The study's clamp: at most 50 ms a frame.
    static func clamp(_ dt: CFTimeInterval) -> CFTimeInterval { max(0, min(0.05, dt)) }

    private func updateRest() {
        guard let h = hud else { if !atRest { atRest = true }; return }
        let rest = h.rolls == 0 || clockMs >= Double(h.restMs)
        if rest != atRest { atRest = rest }
        if h.rolls == 1, !rollReported, clockMs >= Double(h.totalMs) {
            rollReported = true
            onRollDone?()
        }
    }

    /// The picture now, drawn if anything changed; the last one otherwise (and
    /// when the stage could not draw).
    func frame() -> StageFrame? {
        guard dirty, owns, let r = request, r.drawer.width >= 1, r.drawer.height >= 1, hud != nil else { return last }
        dirty = false
        framesAsked += 1
        if let f = stage.frame(atMs: Int(clockMs.rounded(.down)), peek: peekValue(CACurrentMediaTime())) { last = f }
        return last
    }

    var needsFrame: Bool { dirty && owns && hud != nil }

    // MARK: the peek

    /// The tap on the glass: my cup's ellipse toggles the peek, nothing else
    /// on the table does anything. Whether it was my cup.
    @discardableResult
    public func tap(at p: CGPoint) -> Bool {
        guard let h = hud, h.kind == StageScreen.table.rawValue, atRest, Self.inside(p, hit: h.hit) else { return false }
        setPeek(open: !peekOpen, animated: !reduceMotion)
        return true
    }

    /// The tap target: the ellipse round my cup on the glass (`hit`: centre x
    /// y, radii x y).
    static func inside(_ p: CGPoint, hit: [Double]) -> Bool {
        guard hit.count == 4, hit[2] > 0, hit[3] > 0 else { return false }
        let dx = (p.x - hit[0]) / hit[2], dy = (p.y - hit[1]) / hit[3]
        return dx * dx + dy * dy <= 1
    }

    func setPeek(open: Bool, animated: Bool) {
        let now = CACurrentMediaTime()
        let from = peekValue(now)
        peekFrom = from
        peekTo = open ? 1 : 0
        peekStart = animated && from != peekTo ? now : nil
        if peekOpen != open { peekOpen = open }
        dirty = true
        onWake?()
    }

    private static var peekSeconds: Double { Double(CN_PEEK_MS) / 1000 }

    func peekMoving(_ now: CFTimeInterval) -> Bool {
        guard let s = peekStart else { return false }
        return now - s < Self.peekSeconds
    }

    /// My cup's tip now, 0 shut .. 1 the HUD's peek_target, eased by the
    /// kernel's tween.
    func peekValue(_ now: CFTimeInterval) -> Double {
        guard let s = peekStart else { return peekTo }
        let t = min(1, max(0, (now - s) / Self.peekSeconds))
        if t >= 1 { return peekTo }
        return peekFrom + (peekTo - peekFrom) * Double(cn_api_peek_ease(Float(t)))
    }

    // MARK: memory

    private func memoryWarning() {
        guard owns else { return }
        // the arena goes; the picture on screen stays, and the next frame
        // (only if something moves) takes a new arena
        stage.purge()
    }

    // MARK: the camera, for the layers

    /// The HUD's turn as Core Animation's matrix: `ca_screen`, about the
    /// drawer's (0, 0), m11 .. m44 in order.
    public static func tilt(_ h: CnStageHudSnap) -> CATransform3D {
        let m = h.caScreen
        guard m.count == 16 else { return CATransform3DIdentity }
        return CATransform3D(m11: m[0], m12: m[1], m13: m[2], m14: m[3],
                             m21: m[4], m22: m[5], m23: m[6], m24: m[7],
                             m31: m[8], m32: m[9], m33: m[10], m34: m[11],
                             m41: m[12], m42: m[13], m43: m[14], m44: m[15])
    }

    /// A flat point on the glass through the HUD's homography (what
    /// cn_cam_map does): where the turned layer puts it.
    public static func glass(_ p: CGPoint, _ h: CnStageHudSnap) -> CGPoint {
        let m = h.hom
        guard m.count == 9 else { return p }
        let w = m[6] * p.x + m[7] * p.y + m[8]
        return CGPoint(x: (m[0] * p.x + m[1] * p.y + m[2]) / w, y: (m[3] * p.x + m[4] * p.y + m[5]) / w)
    }
}

// MARK: - the view

/// One seat's name on the planks, as the screen wants it drawn.
public struct StageName: Equatable {
    public var seat: Int
    public var name: String
    public var isTurn: Bool
    public var alive: Bool
    public var dice: Int
    /// A word under the name (the reveal's loser stamp), "" for none.
    public var stamp: String
    public var won: Bool

    public init(seat: SeatModel, stamp: String = "", won: Bool = false) {
        self.seat = seat.id
        name = seat.name
        isTurn = seat.isTurn
        alive = seat.alive
        dice = seat.dice
        self.stamp = stamp
        self.won = won
    }
}

public struct StageView: UIViewRepresentable {
    @ObservedObject var director: StageDirector
    var names: [StageName]
    var outWord: String

    public init(director: StageDirector, names: [StageName], outWord: String) {
        self.director = director
        self.names = names
        self.outWord = outWord
    }

    public func makeUIView(context: Context) -> StageUIView { StageUIView(director: director) }

    public func updateUIView(_ v: StageUIView, context: Context) {
        v.update(names: names, outWord: outWord, hud: director.hud)
    }
}

public final class StageUIView: UIView {
    let director: StageDirector
    let tilt = CALayer()
    let planks = CALayer()
    let canvas = CALayer()
    private var nameLayers: [Int: (text: CATextLayer, bar: CALayer, stamp: CATextLayer)] = [:]
    private var names: [StageName] = []
    private var outWord = ""
    private var hud: CnStageHudSnap?
    private var link: CADisplayLink?
    private var lastStamp: CFTimeInterval?
    private var woodStyle: UIUserInterfaceStyle?

    init(director: StageDirector) {
        self.director = director
        super.init(frame: .zero)
        backgroundColor = .clear
        clipsToBounds = true
        isAccessibilityElement = false
        tilt.anchorPoint = .zero
        tilt.position = .zero
        layer.addSublayer(tilt)
        tilt.addSublayer(planks)
        tilt.addSublayer(canvas)
        canvas.contentsGravity = .resize
        canvas.magnificationFilter = .linear
        addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(tapped(_:))))
        director.onWake = { [weak self] in self?.wake() }
        registerForTraitChanges([UITraitUserInterfaceStyle.self]) { (v: StageUIView, _: UITraitCollection) in
            v.setNeedsLayout()
        }
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    // MARK: the model

    func update(names: [StageName], outWord: String, hud: CnStageHudSnap?) {
        let changedHud = hud != self.hud
        if names != self.names || outWord != self.outWord || changedHud {
            self.names = names
            self.outWord = outWord
            self.hud = hud
            setNeedsLayout()
            accessibilityElements = nil
        }
        if changedHud { wake() }
    }

    // MARK: layout

    public override func layoutSubviews() {
        super.layoutSubviews()
        let size = bounds.size
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        tilt.bounds = CGRect(origin: .zero, size: size)
        tilt.transform = hud.map(StageDirector.tilt) ?? CATransform3DIdentity
        // the study's overdraw: 190% by 220%, from -45% and -60%
        planks.frame = CGRect(x: -0.45 * size.width, y: -0.6 * size.height, width: 1.9 * size.width, height: 2.2 * size.height)
        paintWood()
        layoutNames()
        if let f = director.last, director.owns { place(f) }
        CATransaction.commit()
        if size.width >= 1, size.height >= 1 { wake() }
    }

    private func paintWood() {
        let style = traitCollection.userInterfaceStyle
        guard style != woodStyle else { return }
        woodStyle = style
        let scheme: ColorScheme = style == .dark ? .dark : .light
        if let img = CnTextures.wood(scheme) {
            planks.backgroundColor = UIColor(patternImage: img).cgColor
        } else {
            planks.backgroundColor = UIColor(Color(hex: (style == .dark ? WoodTexture.Palette.dark : WoodTexture.Palette.classic).fallbackHex)).cgColor
        }
    }


    // MARK: the names, on the planks

    private static let nameFont = UIFont.systemFont(ofSize: 13, weight: .semibold)
    private static let stampFont = UIFont.systemFont(ofSize: 10, weight: .heavy)

    private func layoutNames() {
        let shown = Set(names.map(\.seat))
        for (s, l) in nameLayers where !shown.contains(s) {
            l.text.removeFromSuperlayer(); l.bar.removeFromSuperlayer(); l.stamp.removeFromSuperlayer()
            nameLayers[s] = nil
        }
        guard let h = hud else {
            for l in nameLayers.values { l.text.isHidden = true; l.bar.isHidden = true; l.stamp.isHidden = true }
            return
        }
        let scale = window?.screen.scale ?? UIScreen.main.scale
        for n in names {
            let l = nameLayers[n.seat] ?? makeName(scale)
            nameLayers[n.seat] = l
            let visible = n.seat < h.seats && h.nameX.indices.contains(n.seat)
            l.text.isHidden = !visible
            l.bar.isHidden = !visible || !n.isTurn
            l.stamp.isHidden = !visible || n.stamp.isEmpty
            guard visible else { continue }
            let ink = n.won ? FColor.win : (n.isTurn ? FColor.textPrimary : FColor.textDim)
            l.text.string = NSAttributedString(string: n.name, attributes: [
                .font: Self.nameFont, .foregroundColor: UIColor(ink.opacity(n.alive ? 1 : 0.6)),
                .kern: 0.6,
            ])
            let textW = min(150, ceil((n.name as NSString).size(withAttributes: [.font: Self.nameFont]).width) + 8)
            let textH: CGFloat = 16, barH: CGFloat = 2, gap: CGFloat = 3
            let x = h.nameX[n.seat], y = h.nameY[n.seat]
            var top: CGFloat
            var left: CGFloat
            switch h.nameHow[n.seat] {
            case CN_NAME_FOOT:            // mine, tall: centred on x, the block's foot on y
                top = y - (textH + gap + barH); left = x - textW / 2
            case CN_NAME_LEFT:            // mine, short: its left edge on x, centred on y
                top = y - (textH + gap + barH) / 2; left = x
            default:                      // a far seat: centred on x, its top 8 above y
                top = y - 8; left = x - textW / 2
            }
            l.text.frame = CGRect(x: left, y: top, width: textW, height: textH)
            let barW: CGFloat = h.nameHow[n.seat] == CN_NAME_BOX ? 36 : 44
            l.bar.frame = CGRect(x: l.text.frame.midX - barW / 2, y: top + textH + gap, width: barW, height: barH)
            l.stamp.string = NSAttributedString(string: n.stamp, attributes: [
                .font: Self.stampFont, .foregroundColor: UIColor.white,
            ])
            let stampW = ceil((n.stamp as NSString).size(withAttributes: [.font: Self.stampFont]).width) + 12
            l.stamp.frame = CGRect(x: l.text.frame.midX - stampW / 2, y: top + textH + gap + barH + 3, width: stampW, height: 16)
        }
    }

    private func makeName(_ scale: CGFloat) -> (text: CATextLayer, bar: CALayer, stamp: CATextLayer) {
        let text = CATextLayer()
        text.contentsScale = scale
        text.alignmentMode = .center
        text.truncationMode = .end
        text.shadowColor = UIColor.black.cgColor
        text.shadowOpacity = 0.9
        text.shadowRadius = 1.5
        text.shadowOffset = CGSize(width: 0, height: 1)
        let bar = CALayer()
        bar.backgroundColor = UIColor(FColor.win).cgColor
        bar.cornerRadius = 1
        bar.shadowColor = UIColor(FColor.win).cgColor
        bar.shadowOpacity = 0.7
        bar.shadowRadius = 4
        bar.shadowOffset = .zero
        let stamp = CATextLayer()
        stamp.contentsScale = scale
        stamp.alignmentMode = .center
        stamp.backgroundColor = UIColor(FColor.red).cgColor
        stamp.cornerRadius = 4
        stamp.masksToBounds = true
        tilt.addSublayer(text)
        tilt.addSublayer(bar)
        tilt.addSublayer(stamp)
        return (text, bar, stamp)
    }

    // MARK: the display link

    public override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil {
            stop()
        } else {
            director.claim()
            wake()
        }
    }

    func wake() {
        guard window != nil, bounds.width >= 1, bounds.height >= 1 else { return }
        if director.needsFrame { draw() }
        guard director.live, link == nil else { return }
        let l = CADisplayLink(target: Ticker(self), selector: #selector(Ticker.tick(_:)))
        l.add(to: .main, forMode: .common)
        link = l
        lastStamp = nil
    }

    func stop() {
        link?.invalidate()
        link = nil
        lastStamp = nil
    }

    fileprivate func tick(_ l: CADisplayLink) {
        let dt = lastStamp.map { l.timestamp - $0 } ?? 0
        lastStamp = l.timestamp
        guard bounds.width >= 1, bounds.height >= 1 else { stop(); return }
        if director.advance(dt) { draw() }
        if !director.live, !director.needsFrame { stop() }
    }

    /// Only from wake() and tick(), which both hold a view with no size back.
    private func draw() {
        guard let f = director.frame() else { return }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        place(f)
        CATransaction.commit()
    }

    private func place(_ f: StageFrame) {
        let c = f.shot.canvas
        guard c.count == 4 else { return }
        canvas.frame = CGRect(x: c[0], y: c[1], width: c[2], height: c[3])
        canvas.contents = f.image
    }

    // MARK: touch

    @objc private func tapped(_ g: UITapGestureRecognizer) {
        if director.tap(at: g.location(in: self)) { Haptics.fire(.pickUp) }
    }

    // MARK: accessibility: one element a seat, the kernel's names

    public override var accessibilityElements: [Any]? {
        get {
            guard let h = hud else { return [] }
            return names.compactMap { n -> UIAccessibilityElement? in
                guard n.seat < h.seats, h.cupX.indices.contains(n.seat) else { return nil }
                let mine = n.seat == h.me
                let e = mine ? MyCupElement(accessibilityContainer: self) : UIAccessibilityElement(accessibilityContainer: self)
                e.accessibilityLabel = n.name
                // a count is a number, not a sentence; an out seat is the kernel's word
                e.accessibilityValue = n.alive ? String(n.dice) : outWord
                let centre: CGPoint
                let r: CGFloat
                if mine, h.hit.count == 4 {
                    centre = CGPoint(x: h.hit[0], y: h.hit[1]); r = max(h.hit[2], h.hit[3])
                } else {
                    centre = StageDirector.glass(CGPoint(x: h.cupX[n.seat], y: h.cupY[n.seat]), h); r = h.cupR
                }
                e.accessibilityFrameInContainerSpace = CGRect(x: centre.x - r, y: centre.y - r, width: 2 * r, height: 2 * r)
                if let m = e as? MyCupElement {
                    m.accessibilityTraits = .button
                    m.activate = { [weak self] in
                        guard let self, let h = self.hud, h.hit.count == 4 else { return false }
                        return self.director.tap(at: CGPoint(x: h.hit[0], y: h.hit[1]))
                    }
                }
                return e
            }
        }
        set {}
    }
}

/// My cup to VoiceOver: activating it is the tap.
private final class MyCupElement: UIAccessibilityElement {
    var activate: (() -> Bool)?
    override func accessibilityActivate() -> Bool { activate?() ?? false }
}

/// The display link holds its target strongly; this breaks the cycle.
private final class Ticker: NSObject {
    weak var view: StageUIView?
    init(_ v: StageUIView) { view = v }
    @objc func tick(_ l: CADisplayLink) {
        guard let view else { l.invalidate(); return }
        MainActor.assumeIsolated { view.tick(l) }
    }
}
