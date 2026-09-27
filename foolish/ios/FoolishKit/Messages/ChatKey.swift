// ChatKey — which CONVERSATION a cached game belongs to (the chat-scoping fix).
//
// Every MessageGameStore read is scoped by this string so a game cached in one
// thread can never resolve in another. Getting it wrong is not a cosmetic bug:
// with a device-wide key, opening the extension in a chat that has never had a
// game reopens some OTHER chat's board, offers to stage that chain's deal-seed-
// bearing payload into this thread, and hands its players' hands to people who
// were never in the game.
//
// THE THING THAT DID NOT WORK: `MSConversation.localParticipantIdentifier`
// alone. Apple describes it as an identifier for the local participant "in this
// conversation", which reads like a per-thread value — but on a real device it
// is the SAME UUID in every conversation (round-3 report: "every time I try to
// send a message it pulls up the same game for each chat, no matter who I'm
// texting"). It identifies the device's own iMessage participant, so scoping by
// it is scoping by device, which is exactly the bug it was meant to close.
//
// What IS per-conversation is the PARTICIPANT SET: `remoteParticipantIdentifiers`
// are UUIDs the system mints per conversation, so a different thread — even with
// the same people in it — carries different remote identifiers. Keying on the
// whole set (local + remotes, sorted so member order can never matter) is stable
// for the lifetime of a thread and distinct between threads.
//
// Pure, so it is testable without the Messages framework; the extension only
// wires MSConversation's two properties in (the same shape as SeatIdentity and
// StagedBubbleRouting).
import Foundation

public enum ChatKey {
    /// The scoping key for the conversation with these participants.
    ///
    /// Sorted, so it does not depend on the order Messages hands the remote
    /// identifiers back (nothing documents that order as stable). Duplicates are
    /// collapsed for the same reason: two equal inputs must not produce two keys.
    ///
    /// Empty input (a conversation with no identifiers at all, which should not
    /// happen) yields a non-empty constant rather than "", so a degenerate key
    /// can never accidentally equal a *missing* one somewhere downstream.
    public static func make(local: String, remotes: [String]) -> String {
        let parts = Set([local] + remotes).filter { !$0.isEmpty }.sorted()
        return parts.isEmpty ? "chat.unknown" : parts.joined(separator: "|")
    }
}
