// FStrings.swift — localization access (§5.5: all text through the table from
// day one; en/ru/ko). v1 backs this with an in-code trilingual table so every
// string is localized immediately. Milestone E4 (scripts/gen_ios_strings.mjs)
// replaces this table with a generated Localizable.xcstrings String Catalog
// merged from the web's src/localization/strings.ts — the API (`FStrings.t`)
// stays, the backing store changes. Keys keep the web's names; iOS-only keys
// use the `ios.` prefix (§16.E4).

import Foundation

public enum AppLanguage: String, CaseIterable, Sendable {
    case system, en, ru, ko
    public var display: String {
        switch self {
        case .system: return "System"
        case .en: return "English"
        case .ru: return "Русский"
        case .ko: return "한국어"
        }
    }
}

public enum FStrings {
    /// Settings language override (§16.E3). Persisted; `.system` follows the OS.
    public static var override: AppLanguage {
        get { AppLanguage(rawValue: UserDefaults.standard.string(forKey: "ios.language") ?? "system") ?? .system }
        set { UserDefaults.standard.set(newValue.rawValue, forKey: "ios.language") }
    }

    private static var activeLang: String {
        if override != .system { return override.rawValue }
        let pref = Locale.preferredLanguages.first ?? "en"
        if pref.hasPrefix("ru") { return "ru" }
        if pref.hasPrefix("ko") { return "ko" }
        return "en"
    }

    /// Look up `key`, interpolating {name}-style placeholders from `args`.
    public static func t(_ key: String, _ args: [String: String] = [:]) -> String {
        let lang = activeLang
        var s = table[lang]?[key] ?? table["en"]?[key] ?? key
        for (k, v) in args { s = s.replacingOccurrences(of: "{\(k)}", with: v) }
        return s
    }

    // The seed table. Trimmed to what the app renders today; the generator will
    // supersede it with the full 113-key set from the web (§16.E4). All three
    // languages carry every key (CI check enforces identical key sets — §16.E4).
    private static let table: [String: [String: String]] = [
        "en": [
            "play": "Play", "offline": "Offline", "join_by_code": "Join by code",
            "resume": "Resume game", "replays": "Replays", "tutorial": "Tutorial",
            "settings": "Settings", "about": "About",
            "pass": "Pass", "pickup": "Take", "good": "Done", "attack": "Attack", "cover": "Cover",
            "game_over": "Game over", "you_win": "You win", "you_lose": "You are the fool",
            "rematch": "Rematch", "share_replay": "Share replay", "home": "Home",
            "choose_opponent": "Choose opponent", "start_game": "Start game",
            "players": "Players", "thinking": "Thinking…",
            "leave_game_title": "Leave this game?", "leave_game_body": "The game is still live.",
            "leave": "Leave", "cancel": "Cancel",
            "ios.online_soon": "Online play arrives in a later update.",
            "ios.reject": "That move isn’t allowed.",
            "ios.you": "You",
        ],
        "ru": [
            "play": "Играть", "offline": "Оффлайн", "join_by_code": "Войти по коду",
            "resume": "Продолжить игру", "replays": "Записи", "tutorial": "Обучение",
            "settings": "Настройки", "about": "О приложении",
            "pass": "Пас", "pickup": "Взять", "good": "Бито", "attack": "Атака", "cover": "Крыть",
            "game_over": "Игра окончена", "you_win": "Вы выиграли", "you_lose": "Вы дурак",
            "rematch": "Ещё раз", "share_replay": "Поделиться записью", "home": "Домой",
            "choose_opponent": "Выберите соперника", "start_game": "Начать игру",
            "players": "Игроки", "thinking": "Думает…",
            "leave_game_title": "Выйти из игры?", "leave_game_body": "Игра ещё идёт.",
            "leave": "Выйти", "cancel": "Отмена",
            "ios.online_soon": "Онлайн-игра появится в следующем обновлении.",
            "ios.reject": "Такой ход недопустим.",
            "ios.you": "Вы",
        ],
        "ko": [
            "play": "플레이", "offline": "오프라인", "join_by_code": "코드로 참가",
            "resume": "게임 계속하기", "replays": "리플레이", "tutorial": "튜토리얼",
            "settings": "설정", "about": "정보",
            "pass": "패스", "pickup": "가져오기", "good": "완료", "attack": "공격", "cover": "방어",
            "game_over": "게임 종료", "you_win": "승리", "you_lose": "당신이 바보입니다",
            "rematch": "재대결", "share_replay": "리플레이 공유", "home": "홈",
            "choose_opponent": "상대 선택", "start_game": "게임 시작",
            "players": "플레이어", "thinking": "생각 중…",
            "leave_game_title": "게임을 나갈까요?", "leave_game_body": "게임이 아직 진행 중입니다.",
            "leave": "나가기", "cancel": "취소",
            "ios.online_soon": "온라인 플레이는 다음 업데이트에서 제공됩니다.",
            "ios.reject": "허용되지 않는 수입니다.",
            "ios.you": "나",
        ],
    ]
}
