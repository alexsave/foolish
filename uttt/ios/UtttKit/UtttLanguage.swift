import Foundation

/// WHICH LANGUAGE THE GAME SPEAKS: the phone's.
///
/// The words are the kernel's, in twenty-five languages (uttt/c/i18n), and
/// this only tells it which one. THE PHONE DECIDES, as it does for the sister
/// product's iMessage board: a player who reads Polish has already told their
/// phone so, and a drawer this size has no room to ask again. The phone's
/// whole ordered list goes in, not its first entry, so a phone set to Catalan
/// then Spanish reads Spanish rather than English.
///
/// A BUBBLE READS IN ITS SENDER'S LANGUAGE on every phone. Its caption and
/// the words in its image are composed by the kernel on the phone that stages
/// it, and Messages shows every phone the same message - so a French sender's
/// "X gagne en diagonale en 25 coups" is what a German receiver reads under
/// it. That is the sister product's rule too (MessageSummary.caption: "one
/// caption per bubble, baked by the sender for everyone"), and the only one a
/// message can keep: it has one caption, not one per reader. Everything drawn
/// for one phone - the sheet, the doors, the rules, VoiceOver - is in that
/// phone's own language.
public enum UtttLanguage {
    /// Tell the kernel the phone's language; returns the code it chose. Cheap,
    /// so it runs at every activation: iOS restarts an extension when the
    /// phone's language changes, and `dev.lang` may change between two.
    @discardableResult
    public static func apply() -> String {
        var tags = Locale.preferredLanguages
#if DEBUG
        if let forced = UtttDev.language { tags = [forced] }
#endif
        return Uttt.speak(tags)
    }
}
