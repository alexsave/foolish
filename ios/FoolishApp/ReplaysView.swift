// ReplaysView.swift — §6 screen 5 / §16.C. The saved-replays list (local codes,
// ReplayStore) plus paste-a-code, which validates by decoding natively
// (EngineC.replayDecode → the shared replay.c). A web-generated code decodes and
// plays here. The Oracle button slot is behind Flags.oracleUI (hidden in v1,
// §10.3). Camera QR scan (§16.C4) is a follow-up — the paste path is wired now.
//
// Playback: the decoded step list feeds a transport (play/pause/step). Rendering
// each step on the full board via the B4 diff engine is the remaining C UI work;
// this screen shows the decoded event stream stepping under the transport so the
// data path is proven end-to-end.

import SwiftUI
import FoolishKit
import FoolishNet

struct ReplaysView: View {
    /// Re-render when a setting changes (see FPrefs). Only the OBSERVATION
    /// matters - the strings still come from FStrings.t.
    @ObservedObject private var prefs = FPrefs.shared
    @Environment(\.dismiss) private var dismiss
    @State private var records = ReplayStore.shared.all()
    @State private var pasteCode = ""
    @State private var decoded: DecodedReplay?
    @State private var error: String?
    @State private var showPlayer = false

    private let engine = EngineC()

    var body: some View {
        NavigationStack {
            List {
                Section {
                    HStack {
                        TextField("Paste a replay code", text: $pasteCode)
                            .textInputAutocapitalization(.characters)
                            .autocorrectionDisabled()
                        Button("Load") { Task { await load(pasteCode) } }
                            .disabled(pasteCode.isEmpty)
                            .tint(FColor.accent)
                    }
                    if let error {
                        Text(error).font(FType.body(12)).foregroundColor(FColor.accent)
                    }
                }

                Section(FStrings.t("replays")) {
                    if records.isEmpty {
                        Text("No saved replays yet.").foregroundColor(FColor.textDim)
                    }
                    ForEach(records) { rec in
                        Button { Task { await load(rec.code) } } label: { row(rec) }
                    }
                    .onDelete { idx in
                        for i in idx { ReplayStore.shared.delete(code: records[i].code) }
                        records = ReplayStore.shared.all()
                    }
                }
            }
            .scrollContentBackground(.hidden)
            .background(FColor.table.ignoresSafeArea())
            .navigationTitle(FStrings.t("replays"))
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button(FStrings.t("home")) { dismiss() }.tint(FColor.accent)
                }
            }
            .navigationDestination(isPresented: $showPlayer) {
                if let decoded { ReplayPlayerView(replay: decoded) }
            }
        }
        .preferredColorScheme(.dark)
        .onAppear { records = ReplayStore.shared.all() }
    }

    private func row(_ rec: ReplayRecord) -> some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(rec.code.prefix(16) + (rec.code.count > 16 ? "…" : ""))
                    .font(FType.body(14)).foregroundColor(FColor.textPrimary)
                Text("\(rec.players) players")
                    .font(FType.body(12)).foregroundColor(FColor.textDim)
            }
            Spacer()
            if let r = rec.myResult {
                Text(r == "win" ? FStrings.t("you_win") : FStrings.t("you_lose"))
                    .font(FType.body(12))
                    .foregroundColor(r == "win" ? FColor.win : FColor.accent)
            }
        }
    }

    private func load(_ code: String) async {
        let cleaned = code.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "https://foolish.cards/", with: "")
            .replacingOccurrences(of: "http://foolish.cards/", with: "")
        do {
            decoded = try await engine.replayDecode(code: cleaned)
            error = nil
            showPlayer = true
        } catch {
            self.error = "Couldn’t read that code."
        }
    }
}

/// Minimal replay transport over the decoded event stream (§16.C1). Board
/// projection through the diff engine is the remaining UI step; this proves the
/// decode + transport path and reads the events out under play/pause/step/scrub.
struct ReplayPlayerView: View {
    let replay: DecodedReplay
    @State private var index = 0
    @State private var playing = false

    private var moves: [ReplayLog] {
        replay.logs.filter { [1, 2, 3, 4].contains($0.type) } // attack/cover/pass/pickup
    }

    var body: some View {
        VStack(spacing: FSpace.l) {
            header
            Spacer()
            eventCard
            Spacer()
            transport
        }
        .padding(FSpace.xl)
        .background(FColor.table.ignoresSafeArea())
        .navigationTitle("Replay")
        .task(id: playing) { await autoplay() }
    }

    private var header: some View {
        VStack(spacing: FSpace.xs) {
            HStack(spacing: FSpace.m) {
                Text("\(replay.nPlayers) players").foregroundColor(FColor.textDim)
                if let t = replay.trumpSuit {
                    Text("trump \(t.glyph)").foregroundColor(FColor.suitColor(t))
                }
            }
            .font(FType.body(14))
            if replay.isComplete {
                Text("Fool: seat \(replay.fool)")
                    .font(FType.body(13)).foregroundColor(FColor.accent)
            }
        }
    }

    @ViewBuilder
    private var eventCard: some View {
        if moves.indices.contains(index) {
            let log = moves[index]
            VStack(spacing: FSpace.s) {
                Text("Move \(index + 1) / \(moves.count)")
                    .font(FType.numeral(28)).foregroundColor(FColor.textPrimary)
                Text(describe(log))
                    .font(FType.body(15)).foregroundColor(FColor.textDim)
                    .multilineTextAlignment(.center)
            }
            .padding(FSpace.xl)
            .frame(maxWidth: .infinity)
            .background(FColor.surface)
            .clipShape(RoundedRectangle(cornerRadius: FRadius.sheet))
        } else {
            Text("No moves").foregroundColor(FColor.textDim)
        }
    }

    private var transport: some View {
        VStack(spacing: FSpace.m) {
            if moves.count > 1 {
                Slider(value: Binding(
                    get: { Double(index) },
                    set: { index = Int($0.rounded()) }
                ), in: 0...Double(moves.count - 1), step: 1)
                .tint(FColor.accent)
            }
            HStack(spacing: FSpace.xl) {
                transportButton("backward.fill") { index = max(0, index - 1) }
                transportButton(playing ? "pause.fill" : "play.fill") { playing.toggle() }
                transportButton("forward.fill") { index = min(moves.count - 1, index + 1) }
            }
        }
    }

    private func transportButton(_ system: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: system).font(.system(size: 22))
                .foregroundColor(FColor.textPrimary).frame(width: 56, height: 44)
        }
    }

    private func autoplay() async {
        while playing && index < moves.count - 1 {
            try? await Task.sleep(nanoseconds: 700_000_000)
            if !playing { break }
            index = min(moves.count - 1, index + 1)
        }
        playing = false
    }

    private func describe(_ log: ReplayLog) -> String {
        let action = ["game start", "attack", "cover", "pass", "pickup", "good",
                      "discard", "defender change", "player out", "draw"]
        let name = log.type < action.count ? action[log.type] : "event"
        let cards = log.pairs.map { pair -> String in
            cardLabel(pair.primary) + (pair.target != nil ? "→\(cardLabel(pair.target!))" : "")
        }.joined(separator: " ")
        return "seat \(log.seat) — \(name)\(cards.isEmpty ? "" : ": \(cards)")"
    }

    private func cardLabel(_ c: Card) -> String {
        guard let s = c.suit else { return "??" }
        return CardRank.label(c.v) + s.glyph
    }
}
