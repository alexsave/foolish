// The layout probe: what does a Messages app message's LAYOUT hand back to the
// extension that reads it? (../../README.md is the evidence.)
//
// A message's URL is capped at 5,000 characters. Its layout is a picture and
// seven strings, and nothing documents whether the reading side gets them. So
// this stages a message whose picture is a known pattern (CLayoutProbe: n x n
// cells of three states at p pixels each), whose strings have a known length
// and a known last character, and whose URL says what the picture is - and
// then reports, for every message the host hands it, how much came back.
//
// TWO WAYS TO ASK FOR A VARIANT. A person taps a preset button. A rig writes
// `dev.probe` into the App Group (`n=243 p=3 grey=1 str=40 media=0 url=200`,
// any subset) and opens the drawer; the probe takes the file and stages that.
//
// THE REPORT goes three places: the screen, the unified log (subsystem
// tools.layoutprobe) and `flight.txt` in the App Group, which is the file the
// rig's `flight` command prints.
import CLayoutProbe
import Messages
import UIKit
import os

private let dev = DevFlags(group: "group.tools.layoutprobe.msg")
private let log = Logger(subsystem: "tools.layoutprobe", category: "probe")

/// One thing to send: the picture's grid, the strings' length, the URL's.
struct Variant {
    var name = "custom"
    var n = 243          // cells a side
    var p = 3            // pixels per cell
    var grey = true      // luminance-only palette (false: the hue-separated one)
    var str = 40         // characters in each of the seven strings
    var media = false    // hand the picture over as a PNG file, not a UIImage
    var url = 200        // characters in the URL

    static let presets: [Variant] = [
        Variant(name: "243x3col", grey: false),
        Variant(name: "243x1col", p: 1, grey: false),
        Variant(name: "243x1gry", p: 1),
        Variant(name: "243x2gry", p: 2),
        Variant(name: "243x3gry"),
        Variant(name: "729x1gry", n: 729, p: 1),
        Variant(name: "729x2gry", n: 729, p: 2),
        Variant(name: "729x3gry", n: 729, p: 3),
        Variant(name: "1458x3gry", n: 1458, p: 3),
        Variant(name: "str1k", str: 1000),
        Variant(name: "str20k", str: 20000),
        Variant(name: "str200k", str: 200_000),
        Variant(name: "mediaPNG", media: true),
        Variant(name: "url4990", url: 4990),
        Variant(name: "url5200", url: 5200),
    ]

    /// `key=value` pairs from the rig's file; a key it does not name keeps its default.
    init(pairs: [(key: String, value: String)]) {
        for (k, v) in pairs {
            switch k {
            case "name": name = v
            case "n": n = Int(v) ?? n
            case "p": p = Int(v) ?? p
            case "grey": grey = v != "0"
            case "str": str = Int(v) ?? str
            case "media": media = v == "1"
            case "url": url = Int(v) ?? url
            default: break
            }
        }
    }
    init(name: String, n: Int = 243, p: Int = 3, grey: Bool = true, str: Int = 40, media: Bool = false, url: Int = 200) {
        self.name = name; self.n = n; self.p = p; self.grey = grey; self.str = str; self.media = media; self.url = url
    }
}

/// The pattern as a picture, one image pixel per buffer pixel.
private func patternImage(n: Int, p: Int, grey: Bool) -> UIImage {
    let side = n * p
    var buf = [UInt8](repeating: 0, count: side * side * 4)
    lp_fill(&buf, Int32(n), Int32(p), grey ? LP_GREY : LP_COLOUR)
    let ctx = CGContext(data: &buf, width: side, height: side, bitsPerComponent: 8, bytesPerRow: side * 4,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    return UIImage(cgImage: ctx.makeImage()!, scale: 1, orientation: .up)
}

/// What survived of an n x n pattern in a picture that came back.
private func judge(_ img: UIImage, n: Int, grey: Bool) -> String {
    guard let cg = img.cgImage else { return "no bitmap (size \(img.size) scale \(img.scale))" }
    let w = cg.width, h = cg.height
    var buf = [UInt8](repeating: 0, count: w * h * 4)
    let ctx = CGContext(data: &buf, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
    let v = lp_judge(buf, Int32(w), Int32(h), Int32(n), grey ? LP_GREY : LP_COLOUR)
    return "\(w)x\(h) px wrong \(v.wrong)/\(v.cells) exact \(v.exact) max_err \(v.max_err)"
}

/// A string of exactly `n` characters that says what it is and ends in Z, so a
/// reader can tell a whole one from a truncated one.
private func filler(_ n: Int, _ tag: String) -> String {
    let head = "\(tag)\(n):"
    return head + String(repeating: "x", count: max(0, n - head.count - 1)) + "Z"
}

private func whole(_ name: String, _ s: String?) -> String {
    guard let s else { return "\(name) nil" }
    return "\(name) \(s.count)\(s.hasSuffix("Z") ? "" : " CUT")"
}

final class MessagesViewController: MSMessagesAppViewController {
    private let out = UITextView()
    private var lines: [String] = []
    private var asked: Variant?        // the rig's variant, held until the view is a drawer

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        out.isEditable = false
        out.backgroundColor = .black
        out.textColor = .green
        out.font = .monospacedSystemFont(ofSize: 8, weight: .regular)
        out.translatesAutoresizingMaskIntoConstraints = false
        let grid = UIStackView()
        grid.axis = .vertical
        grid.distribution = .fillEqually
        grid.spacing = 3
        grid.translatesAutoresizingMaskIntoConstraints = false
        let perRow = 5
        for r in 0..<(Variant.presets.count + perRow - 1) / perRow {
            let row = UIStackView()
            row.axis = .horizontal
            row.distribution = .fillEqually
            row.spacing = 3
            for i in (r * perRow)..<min(Variant.presets.count, (r + 1) * perRow) {
                let b = UIButton(type: .system)
                b.setTitle(Variant.presets[i].name, for: .normal)
                b.tag = i
                b.titleLabel?.font = .systemFont(ofSize: 11)
                b.backgroundColor = .darkGray
                b.setTitleColor(.white, for: .normal)
                b.addTarget(self, action: #selector(tapped(_:)), for: .touchUpInside)
                row.addArrangedSubview(b)
            }
            grid.addArrangedSubview(row)
        }
        view.addSubview(grid)
        view.addSubview(out)
        NSLayoutConstraint.activate([
            grid.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 4),
            grid.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 4),
            grid.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -4),
            grid.heightAnchor.constraint(equalToConstant: 120),
            out.topAnchor.constraint(equalTo: grid.bottomAnchor, constant: 4),
            out.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            out.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            out.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
    }

    private func say(_ s: String) {
        log.notice("\(s, privacy: .public)")
        lines.append(s)
        out.text = lines.suffix(40).joined(separator: "\n")
        // The whole report every time, appended to what earlier processes
        // wrote: leaving a thread ends this process, and the rig reads after.
        dev.write((dev.raw("flight.txt").map { $0 + "\n" } ?? "") + s, to: "flight.txt")
    }

    // MARK: - reading

    private func inspect(_ m: MSMessage?, _ why: String) {
        guard let m else { return }
        let q = m.url.flatMap { URLComponents(url: $0, resolvingAgainstBaseURL: false)?.queryItems } ?? []
        func item(_ k: String) -> String { q.first { $0.name == k }?.value ?? "?" }
        let n = Int(item("n")) ?? 243, grey = item("c") != "0"
        say("[\(why)] \(item("v")) iOS \(UIDevice.current.systemVersion) url \(m.url?.absoluteString.count ?? -1)")
        var t = m.layout as? MSMessageTemplateLayout
        if let live = m.layout as? MSMessageLiveLayout { t = live.alternateLayout }
        guard let t else { say("  layout \(m.layout.map { "\(type(of: $0))" } ?? "nil")"); return }
        say("  image " + (t.image.map { judge($0, n: n, grey: grey) } ?? "nil"))
        if let u = t.mediaFileURL {
            if let d = try? Data(contentsOf: u), let img = UIImage(data: d) {
                say("  media .\(u.pathExtension) \(d.count) B " + judge(img, n: n, grey: grey))
            } else {
                say("  media \(u.lastPathComponent) unreadable")
            }
        } else {
            say("  media nil")
        }
        say("  " + [("caption", t.caption), ("subcaption", t.subcaption), ("trailingCaption", t.trailingCaption),
                    ("trailingSubcaption", t.trailingSubcaption), ("imageTitle", t.imageTitle),
                    ("imageSubtitle", t.imageSubtitle), ("summaryText", m.summaryText)]
            .map { whole($0.0, $0.1) }.joined(separator: ", "))
    }

    // MARK: - sending

    @objc private func tapped(_ b: UIButton) { stage(Variant.presets[b.tag]) }

    private func stage(_ v: Variant) {
        guard let c = activeConversation else { say("no conversation"); return }
        let m = MSMessage(session: MSSession())
        let l = MSMessageTemplateLayout()
        l.caption = filler(v.str, "cap")
        l.subcaption = filler(v.str, "sub")
        l.trailingCaption = filler(v.str, "tcap")
        l.trailingSubcaption = filler(v.str, "tsub")
        l.imageTitle = filler(v.str, "ititle")
        l.imageSubtitle = filler(v.str, "isub")
        m.summaryText = filler(v.str, "sum")
        let img = patternImage(n: v.n, p: v.p, grey: v.grey)
        if v.media {
            let f = FileManager.default.temporaryDirectory.appendingPathComponent("pattern.png")
            try? img.pngData()?.write(to: f)
            l.mediaFileURL = f
        } else {
            l.image = img
        }
        m.layout = l
        // The message describes its own picture, so any reader can judge it.
        var comps = URLComponents()
        let head = [URLQueryItem(name: "v", value: v.name), URLQueryItem(name: "n", value: "\(v.n)"),
                    URLQueryItem(name: "p", value: "\(v.p)"), URLQueryItem(name: "c", value: v.grey ? "1" : "0")]
        comps.queryItems = head + [URLQueryItem(name: "m", value: "")]
        let fixed = comps.url?.absoluteString.count ?? 0
        comps.queryItems = head + [URLQueryItem(name: "m", value: filler(max(1, v.url - fixed), "u"))]
        m.url = comps.url
        say("STAGE \(v.name) picture \(v.n * v.p) px strings \(v.str) url \(m.url?.absoluteString.count ?? -1)")
        // An expanded drawer hides Messages' Send; the staged bubble needs compact.
        requestPresentationStyle(.compact)
        c.insert(m) { [weak self] e in
            DispatchQueue.main.async { self?.say("  insert \(e.map { "REFUSED \($0)" } ?? "ok")") }
        }
    }

    /// The rig's variant, once the view is really a drawer: from the + menu the
    /// first appearance is window-sized, and an insert made then is dropped
    /// with no callback (shared/c/msg_stage).
    private func stageAskedIfUp() {
        guard let v = asked, let win = view.window else { return }
        guard InsertStaging.drawerUp(window: Double(win.bounds.height), view: Double(view.bounds.height),
                                     expanded: presentationStyle == .expanded) else { return }
        asked = nil
        stage(v)
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        stageAskedIfUp()
    }

    // MARK: - what the host hands over

    /// A tapped bubble opens the drawer EXPANDED, and an expanded drawer hides
    /// the thread from the accessibility tree the rig steers by. The report is
    /// on file by now, so step back down; a person who wants it large drags up.
    private func opened(_ m: MSMessage, _ why: String) {
        inspect(m, why)
        requestPresentationStyle(.compact)
    }

    override func willBecomeActive(with conversation: MSConversation) {
        if let m = conversation.selectedMessage {
            opened(m, "opened")
        } else if dev.exists("dev.probe") {
            asked = Variant(pairs: dev.pairs("dev.probe"))
            dev.write(nil, to: "dev.probe")
        }
    }

    override func didBecomeActive(with conversation: MSConversation) { stageAskedIfUp() }

    override func didSelect(_ message: MSMessage, conversation: MSConversation) { opened(message, "selected") }
    override func didReceive(_ message: MSMessage, conversation: MSConversation) { inspect(message, "received") }
    override func didStartSending(_ message: MSMessage, conversation: MSConversation) { inspect(message, "sending") }
    override func didCancelSending(_ message: MSMessage, conversation: MSConversation) { say("send cancelled") }
}
