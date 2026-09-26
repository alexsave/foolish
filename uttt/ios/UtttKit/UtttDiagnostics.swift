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
// THE CLAIM IS DEBUG ONLY (the store build, 1.0 release): the buttons compile
// out of Release, and the read-only dump and Copy stay, as foolish ships.
// THE CLAIM IS TEMPORARY. It writes this device's seat record for the game
// (UtttSeats, the kernel's utm_rec_*) - the same record a create, a join or
// a sender-resolved seat writes - so it needs no machinery of its own.
// Delete the buttons and uti_msg_claim together once 1.0(9) has shown the
// record and the sender fallback seat the owner by themselves.

import UIKit

/// The panel: scrollable monospaced text, Copy, and (DEBUG) the claim buttons.
public final class UtttDiagnosticsSheet: UIViewController {
    public enum Action { case claimO, claimX, clearClaim }

    private let text: () -> String
    private let body = UITextView()
    #if DEBUG
    private let act: (Action) -> Void
    private let canClaimX: Bool
    private let hasClaim: Bool
    #endif

    /// One signature in both builds, so the caller has no #if of its own; in
    /// Release the claim arguments are dropped and `act` is never called.
    public init(text: @escaping () -> String, canClaimX: Bool, hasClaim: Bool,
                act: @escaping (Action) -> Void) {
        self.text = text
        #if DEBUG
        self.act = act
        self.canClaimX = canClaimX
        self.hasClaim = hasClaim
        #endif
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
        var rows: [UIView] = [title, row1]
        row1.axis = .horizontal; row1.spacing = 8; row1.distribution = .fillEqually
        #if DEBUG
        /* The claim is DEBUG only: in a store build anyone who found the hold
         * could take the other player's seat and move for them. */
        var claims = [button("Claim O") { [weak self] _ in self?.finish(.claimO) }]
        if canClaimX { claims.append(button("Claim X") { [weak self] _ in self?.finish(.claimX) }) }
        if hasClaim { claims.append(button("Forget seat") { [weak self] _ in self?.finish(.clearClaim) }) }
        let row2 = UIStackView(arrangedSubviews: claims)
        row2.axis = .horizontal; row2.spacing = 8; row2.distribution = .fillEqually
        let note = UILabel()
        note.text = "Claim writes this device's seat record for this game only; Forget drops it."
        note.font = .systemFont(ofSize: 11)
        note.textColor = .secondaryLabel
        note.numberOfLines = 0
        rows += [row2, note]
        #endif
        rows.append(body)
        let stack = UIStackView(arrangedSubviews: rows)
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

    #if DEBUG
    private func finish(_ a: Action) {
        dismiss(animated: true) { [act] in act(a) }
    }
    #endif

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
