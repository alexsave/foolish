// UtttDiagnostics.swift - the hold-the-rulebook diagnostics panel, and the
// TEMPORARY seat claim it offers.
//
// WHY (1.0(9), 2026-09-25): on 1.0(8) the owner opened a game he created and
// got "watching" - the kernel hashed this device's participant id with the
// game's seed and the result was neither the O tag nor the X tag. The panel
// prints every input to that verdict so a screenshot says which one moved;
// the claim lets him keep playing that one game meanwhile.
//
// foolish's pattern (GameSurface.diagnosticPanel, FSquareButton's onHold): a
// hold on a door that is already there, a monospaced dump, in the build that
// shipped, over the game rather than instead of it. Nothing here is secret:
// a tic-tac-toe board has no hidden information.
//
// THE CLAIM IS TEMPORARY. It exists until the cause is fixed; delete this
// file's `UtttClaims`, the kernel's uti_claim* and the buttons together.

import UIKit

/// Claims this device holds, seed -> the seat tag it claims on that game.
/// The extension's own defaults (Release has no App Group); a claim for one
/// game never touches another (the kernel checks the seed).
public enum UtttClaims {
    private static let key = "uttt.claims.v1"

    public static var all: [Int32: Data] {
        guard let d = UserDefaults.standard.dictionary(forKey: key) as? [String: Data] else { return [:] }
        var out: [Int32: Data] = [:]
        for (k, v) in d { if let s = Int32(k) { out[s] = v } }
        return out
    }

    public static func set(_ tag: Data?, seed: Int32) {
        var d = (UserDefaults.standard.dictionary(forKey: key) as? [String: Data]) ?? [:]
        d[String(seed)] = tag
        UserDefaults.standard.set(d, forKey: key)
    }
}

/// The panel: scrollable monospaced text, Copy, and the claim buttons.
public final class UtttDiagnosticsSheet: UIViewController {
    public enum Action { case claimO, claimX, clearClaim }

    private let text: () -> String
    private let act: (Action) -> Void
    private let body = UITextView()
    private let canClaimX: Bool
    private let hasClaim: Bool

    public init(text: @escaping () -> String, canClaimX: Bool, hasClaim: Bool,
                act: @escaping (Action) -> Void) {
        self.text = text
        self.act = act
        self.canClaimX = canClaimX
        self.hasClaim = hasClaim
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
        body.text = text()
        body.translatesAutoresizingMaskIntoConstraints = false

        let title = UILabel()
        title.text = "Diagnostics"
        title.font = .systemFont(ofSize: 17, weight: .bold)

        let row1 = UIStackView(arrangedSubviews: [
            button("Copy") { [weak self] b in
                UIPasteboard.general.string = self?.body.text
                b.setTitle("Copied", for: .normal)
            },
            button("Close") { [weak self] _ in self?.dismiss(animated: true) },
        ])
        var claims = [button("Claim O") { [weak self] _ in self?.finish(.claimO) }]
        if canClaimX { claims.append(button("Claim X") { [weak self] _ in self?.finish(.claimX) }) }
        if hasClaim { claims.append(button("Clear claim") { [weak self] _ in self?.finish(.clearClaim) }) }
        let row2 = UIStackView(arrangedSubviews: claims)
        let note = UILabel()
        note.text = "Claim is temporary: it makes this device that seat on this game only."
        note.font = .systemFont(ofSize: 11)
        note.textColor = .secondaryLabel
        note.numberOfLines = 0

        for r in [row1, row2] { r.axis = .horizontal; r.spacing = 8; r.distribution = .fillEqually }
        let stack = UIStackView(arrangedSubviews: [title, row1, row2, note, body])
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

    private func finish(_ a: Action) {
        dismiss(animated: true) { [act] in act(a) }
    }

    private func button(_ title: String, _ tap: @escaping (UIButton) -> Void) -> UIButton {
        var c = UIButton.Configuration.bordered()
        c.title = title
        let b = UIButton(configuration: c)
        b.addAction(UIAction { a in tap(a.sender as! UIButton) }, for: .touchUpInside)
        return b
    }
}

public extension Data {
    /// Lower-case hex, for the diagnostics dump.
    var hex: String { map { String(format: "%02x", $0) }.joined() }
}
