// MessageComposer — turn a sealed payload into the MSMessage we stage (§11.3).
//
// This is the ONLY file that builds an MSMessage. It never sends: `insert` merely
// stages the bubble into the input field and the human presses send (§11.4).
// Reusing `selectedMessage.session` is what makes Messages COLLAPSE the previous
// bubble instead of piling a new one per turn (§5.2 "one bubble per game").
//
// 1.0(7): the balloon is an MSMessageLiveLayout, so each RECEIVING device draws
// the bubble itself (TranscriptBubbleView) in ITS OWN language and light/dark
// scheme, instead of everyone seeing the sender's baked caption and appearance.
// The template below rides along as the `alternateLayout` — what Messages shows
// anyone WITHOUT the app installed, in notifications, and on the lock screen,
// where the extension never runs. So the baked image + caption + summaryText are
// still exactly as before for those surfaces; app users just get the live one.

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

        // The fallback balloon (non-app users, notifications, lock screen): the
        // baked snapshot + caption, exactly as 1.0(6) shipped it.
        let template = MSMessageTemplateLayout()
        template.image = snapshot
        template.caption = caption
        // The app-user balloon: our own view, drawn on THEIR device in THEIR
        // locale/scheme. `alternateLayout` is the required non-app fallback.
        msg.layout = MSMessageLiveLayout(alternateLayout: template)
        msg.summaryText = summary          // the collapsed / notification line
        return msg
    }
}
