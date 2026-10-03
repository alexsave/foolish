#if UTTT_BIG_BOARD
import CUttt
import Messages
import UIKit
import UIKit.UIGestureRecognizerSubclass

/// THE PICTURE DIAGNOSTICS (docs/BIG_BOARD.md, "The picture diagnostics").
///
/// The owner tests the 243 board on a second phone from TestFlight with no
/// cable, so the app has to say what a real send did to a bubble's picture.
/// In the 243 mode a 1.5 s hold on the rulebook opens a report on the bubble
/// the drawer was opened from, with Copy; every read of a big picture and
/// every big send adds a line to a history that outlives the extension.
///
/// EVERY RULE IS THE KERNEL'S (uttt/c/src/uttt_big_diag.h): the JPEG facts,
/// the greys, the expectations, the ring's layout and every line of the
/// report. This file gathers what only iOS knows - the layout, the file, the
/// participants, the device - into one struct, and keeps the ring's bytes.
public enum UtttBigDiag {

    /// How the drawer came to the message on screen.
    public enum From: Int32 {
        case none = 0, selected = 1, didSelect = 2, didReceive = 3
    }

    // ------------------------------------------------------------ the ring

    /// THE HISTORY, in the extension's own defaults (as UtttSeats and the
    /// mode are kept): the kernel's fixed little-endian ring, at most
    /// UTI_BIG_DIAG_RING_BYTES, never JSON. Messages ends the extension when
    /// a thread is left, so this is what lets a send on one phone be set
    /// beside the opening on the other afterwards.
    private static let ringKey = "uttt.big.diag.ring"

    static var ring: Data { UserDefaults.standard.data(forKey: ringKey) ?? Data() }

    /// Add what `facts` say to the ring, as a send or an opening.
    static func record(_ facts: Facts, role: Int32) {
        let old = ring
        var out = [UInt8](repeating: 0, count: Int(UTI_BIG_DIAG_RING_BYTES))
        let n = facts.withC { f in
            old.withUnsafeBytes { o in
                out.withUnsafeMutableBufferPointer { b in
                    uti_big_diag_record(&f, role, o.bindMemory(to: UInt8.self).baseAddress, Int32(old.count),
                                        b.baseAddress, Int32(b.count))
                }
            }
        }
        guard n > 0 else { return }
        UserDefaults.standard.set(Data(out.prefix(Int(n))), forKey: ringKey)
    }

    // ------------------------------------------------------- what iOS says

    /// Everything the report needs that only iOS can tell, about one message
    /// (or none), plus the reading of its picture.
    struct Facts {
        var from: From = .none
        var who: Int32 = UTI_BIG_DIAG_WHO_UNKNOWN
        var pending = false
        var session: String?
        var url: String?
        var layout: String?
        var caption: Int32 = -1, subcaption: Int32 = -1, summary: Int32 = -1
        var imageSize: (w: Int32, h: Int32, scalePct: Int32)?
        var fileExt: String?
        var file: Data?
        var reading: UtttBigBubble.Inspection?

        /// Run `body` with these facts as the kernel's struct; every pointer
        /// in it lives only for the call.
        func withC<R>(_ body: (inout UtiBigDiag) -> R) -> R {
            let info = Bundle.main.infoDictionary ?? [:]
            let strings: [String?] = [
                info["CFBundleShortVersionString"] as? String, info["CFBundleVersion"] as? String,
                UIDevice.current.systemVersion, UtttBigDiag.model, UtttBigDiag.install,
                session, url, layout, fileExt,
            ]
            /* strdup'd for the call, so no pointer outlives its string */
            let c = strings.map { $0.map { strdup($0) } ?? nil }
            defer { c.forEach { free($0) } }
            let ring = UtttBigDiag.ring
            let file = self.file ?? Data()
            let rgba = reading?.rgba ?? []
            let symbols = reading?.symbols ?? []
            return ring.withUnsafeBytes { ringBytes in
                file.withUnsafeBytes { fileBytes in
                    rgba.withUnsafeBufferPointer { px in
                        symbols.withUnsafeBufferPointer { sym in
                            var f = UtiBigDiag()
                            f.app_version = UnsafePointer(c[0]); f.app_build = UnsafePointer(c[1])
                            f.os_version = UnsafePointer(c[2]); f.model = UnsafePointer(c[3])
                            f.install = UnsafePointer(c[4]); f.session = UnsafePointer(c[5])
                            f.url = UnsafePointer(c[6])
                            f.layout = UnsafePointer(c[7]); f.file_ext = UnsafePointer(c[8])
                            f.now = Int64(Date().timeIntervalSince1970)
                            f.utc_offset = Int32(TimeZone.current.secondsFromGMT())
                            f.from = from.rawValue
                            f.who = who
                            f.pending = pending ? 1 : 0
                            f.caption_len = caption; f.subcaption_len = subcaption; f.summary_len = summary
                            if let s = imageSize {
                                f.has_image = 1; f.image_w = s.w; f.image_h = s.h; f.image_scale_pct = s.scalePct
                            }
                            if self.file != nil {
                                f.has_file = 1
                                f.file_bytes = Int64(file.count)
                                f.file = fileBytes.bindMemory(to: UInt8.self).baseAddress
                                f.file_n = Int64(file.count)
                            }
                            if let r = reading {
                                f.read_result = r.result
                                f.read_cells = Int32(r.cells); f.read_risky = Int32(r.risky)
                                f.read_min_margin = Int32(r.minMargin); f.read_us = Int32(r.micros)
                                if !rgba.isEmpty { f.rgba = px.baseAddress; f.rgba_w = Int32(r.width); f.rgba_h = Int32(r.height) }
                                if symbols.count == UtttBig.cellCount { f.symbols = sym.baseAddress }
                            } else {
                                f.read_result = UTI_BIG_DIAG_R_NOT_READ
                            }
                            f.ring = ringBytes.bindMemory(to: UInt8.self).baseAddress
                            f.ring_n = Int32(ring.count)
                            return body(&f)
                        }
                    }
                }
            }
        }
    }

    /// The facts of `message` (nil: the drawer came from the + menu).
    /// `pixels`: also draw the picture for the greys (the report wants them;
    /// a send's record does not).
    static func facts(of message: MSMessage?, in conversation: MSConversation?, from: From,
                      read: Bool, pixels: Bool) -> Facts {
        var f = Facts()
        guard let message else { return f }
        f.from = from
        if let conversation {
            f.who = message.senderParticipantIdentifier == conversation.localParticipantIdentifier
                ? UTI_BIG_DIAG_WHO_ME : UTI_BIG_DIAG_WHO_OTHER
        }
        f.pending = message.isPending
        f.session = message.session.map { "\($0.hash)" }
        f.url = message.url?.absoluteString
        if let layout = message.layout {
            f.layout = String(describing: type(of: layout))
        }
        let template = (message.layout as? MSMessageTemplateLayout)
            ?? (message.layout as? MSMessageLiveLayout)?.alternateLayout
        if let t = template {
            func len(_ s: String?) -> Int32 { s.map { Int32($0.count) } ?? -1 }
            f.caption = len(t.caption); f.subcaption = len(t.subcaption)
            if let image = t.image {
                let cg = image.cgImage
                f.imageSize = (Int32(cg?.width ?? Int(image.size.width * image.scale)),
                               Int32(cg?.height ?? Int(image.size.height * image.scale)),
                               Int32((image.scale * 100).rounded()))
            }
            if let u = t.mediaFileURL {
                f.fileExt = u.pathExtension
                f.file = try? Data(contentsOf: u)
            }
        }
        f.summary = message.summaryText.map { Int32($0.count) } ?? -1
        if read { f.reading = UtttBigBubble.inspect(message, pixels: pixels) }
        return f
    }

    /// THE REPORT on `message`, and the opening it is recorded as.
    @MainActor
    public static func report(on message: MSMessage?, in conversation: MSConversation, from: From) -> String {
        let f = facts(of: message, in: conversation, from: message == nil ? .none : from,
                      read: message != nil, pixels: true)
        if message != nil { record(f, role: UTI_BIG_DIAG_OPENED) }
        var out = [CChar](repeating: 0, count: Int(UTI_BIG_DIAG_TEXT_MAX))
        _ = f.withC { c in out.withUnsafeMutableBufferPointer { uti_big_diag_report(&c, $0.baseAddress, Int32($0.count)) } }
        return String(cString: out)
    }

    /// A big bubble this device just sent, as its own extension saw it at
    /// didStartSending: the picture's size, the file and its quality. Not
    /// sampled - a send must not wait on a read.
    public static func recordSend(_ message: MSMessage, in conversation: MSConversation) {
        let f = facts(of: message, in: conversation, from: .none, read: false, pixels: false)
        record(f, role: UTI_BIG_DIAG_SENT)
        UtttLog.note("big-diag", "send recorded: image \(f.imageSize.map { "\($0.w)x\($0.h)" } ?? "none"), file \(f.file?.count ?? 0) B")
    }

    /// A read of a bubble's picture (UtttBigBubble.cells), as an opening.
    static func recordRead(_ message: MSMessage, _ reading: UtttBigBubble.Inspection) {
        var f = facts(of: message, in: nil, from: .selected, read: false, pixels: false)
        f.reading = reading
        record(f, role: UTI_BIG_DIAG_OPENED)
    }

    // -------------------------------------------------------- the device

    /// The hardware's model identifier (iPhone16,2), or the simulated one.
    static var model: String {
        if let sim = ProcessInfo.processInfo.environment["SIMULATOR_MODEL_IDENTIFIER"] { return sim + " sim" }
        var u = utsname()
        uname(&u)
        return withUnsafeBytes(of: &u.machine) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
    }

    /// Which build this is: a debug build, a TestFlight install, or neither.
    static var install: String {
#if DEBUG
        "debug"
#else
        Bundle.main.appStoreReceiptURL?.lastPathComponent == "sandboxReceipt" ? "TestFlight" : "release"
#endif
    }
}

/// The report on screen: scrollable monospaced text, Copy and Close. A page
/// sheet over the drawer, as the debug diagnostics are (UtttDiagnostics.swift,
/// which is DEBUG-only and carries the seat claim; this is neither).
public final class UtttBigDiagSheet: UIViewController {
    private let text: String
    private let body = UITextView()

    public init(text: String) {
        self.text = text
        super.init(nibName: nil, bundle: nil)
        modalPresentationStyle = .pageSheet
    }
    required init?(coder: NSCoder) { fatalError() }

    public override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemBackground
        body.isEditable = false
        body.isSelectable = true
        body.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        body.textColor = .label
        body.backgroundColor = .clear
        body.text = text
        body.accessibilityIdentifier = "big-diag-text"

        let title = UILabel()
        title.text = "243 picture"
        title.font = .systemFont(ofSize: 17, weight: .bold)

        let copy = button("Copy") { [weak self] b in
            UIPasteboard.general.string = self?.text
            b.setTitle("Copied", for: .normal)
        }
        copy.accessibilityIdentifier = "big-diag-copy"
        let row = UIStackView(arrangedSubviews: [
            copy,
            button("Close") { [weak self] _ in self?.dismiss(animated: true) },
        ])
        row.axis = .horizontal; row.spacing = 8; row.distribution = .fillEqually
        let stack = UIStackView(arrangedSubviews: [title, row, body])
        stack.axis = .vertical
        stack.spacing = 8
        stack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stack)
        let g = view.safeAreaLayoutGuide
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: g.leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: g.trailingAnchor, constant: -16),
            stack.topAnchor.constraint(equalTo: g.topAnchor, constant: 16),
            stack.bottomAnchor.constraint(equalTo: g.bottomAnchor, constant: -8),
        ])
    }

    private func button(_ title: String, _ tap: @escaping (UIButton) -> Void) -> UIButton {
        var c = UIButton.Configuration.bordered()
        c.title = title
        let b = UIButton(configuration: c)
        b.addAction(UIAction { a in tap(a.sender as! UIButton) }, for: .touchUpInside)
        return b
    }
}

/// THE DIAGNOSTICS' DOOR: a 1.5 s hold on the rulebook, IN THE 243 MODE ONLY.
///
/// Outside the mode it fails the moment a finger lands, so the rulebook is
/// exactly the shipped door: a tap opens the rules, and in a debug build its
/// own hold still opens the debug diagnostics (that hold waits on this one,
/// which has already failed). In the mode it fires at 1.5 s and cancels the
/// touch, so the release never also opens the rules, and the debug hold,
/// waiting on this one, never fires. The grid's 4 s hold is on the board, a
/// different view: neither hold sees the other's touches.
public final class UtttBigDiagHold: UILongPressGestureRecognizer {
    /// The rulebook's own hold, 1.5 s (UtttRulebookButton.holdSeconds is
    /// DEBUG-only, so the number is stated here for every build).
    static let seconds: TimeInterval = 1.5
    var action: () -> Void

    init(_ action: @escaping () -> Void) {
        self.action = action
        super.init(target: nil, action: nil)
        addTarget(self, action: #selector(held))
        minimumPressDuration = Self.seconds
    }

    public override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) {
        guard UtttBigMode.on else {
            state = .failed
            return
        }
        super.touchesBegan(touches, with: event)
    }

    @objc private func held() {
        guard state == .began else { return }
        action()
    }

    /// Put the door on every rulebook in `screen`, once each; a second call
    /// only changes the action. Any other hold already on a rulebook (the
    /// debug diagnostics') waits for this one to fail.
    public static func install(in screen: UIView, _ action: @escaping () -> Void) {
        for book in rulebooks(in: screen) {
            if let h = book.gestureRecognizers?.first(where: { $0 is UtttBigDiagHold }) as? UtttBigDiagHold {
                h.action = action
                continue
            }
            let h = UtttBigDiagHold(action)
            for other in book.gestureRecognizers ?? [] where other is UILongPressGestureRecognizer {
                other.require(toFail: h)
            }
            book.addGestureRecognizer(h)
        }
    }

    private static func rulebooks(in v: UIView) -> [UtttRulebookButton] {
        if let b = v as? UtttRulebookButton { return [b] }
        return v.subviews.flatMap { rulebooks(in: $0) }
    }
}
#endif
