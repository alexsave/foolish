// MessageComposer — turn a sealed payload into the MSMessage we stage (§11.3).
//
// This is the ONLY file that builds an MSMessage. It never sends: `insert` merely
// stages the bubble into the input field and the human presses send (§11.4).
// Reusing `selectedMessage.session` is what makes Messages COLLAPSE the previous
// bubble instead of piling a new one per turn (§5.2 "one bubble per game").

import Messages
import UIKit
import FoolishKit

enum MessageComposer {
    /// Build the staged message. `url` is the tap target — a `/m/` game link for a
    /// live turn, or the §12 replay link for a finished game (the caller decides).
    /// `session` = the opened bubble's session to reuse its balloon, or nil for a
    /// brand-new game. `snapshot` is the §10 public table image (both hands as
    /// backs). `caption`/`summary` are localized.
    static func message(url: URL,
                        snapshot: UIImage?,
                        caption: String,
                        summary: String,
                        session: MSSession?) -> MSMessage {
        let msg = MSMessage(session: session ?? MSSession())
        msg.url = url

        let layout = MSMessageTemplateLayout()
        layout.image = snapshot
        layout.caption = caption
        msg.layout = layout
        msg.summaryText = summary          // the collapsed / notification line
        return msg
    }
}
