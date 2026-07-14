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
            "ios.tut_next": "Got it",
            "ios.tut_done": "Start playing",
            "ios.tut_1": "Welcome to Durak. The lowest card of the trump suit decides who attacks first. Trump beats every other suit.",
            "ios.tut_2": "To attack, tap a card in your hand. Your opponent must beat it — or pick it all up.",
            "ios.tut_3": "To defend, tap one of your cards, then tap the attack it beats. A higher card of the same suit, or any trump, covers it.",
            "ios.tut_4": "Can’t or won’t defend? Tap Take to pick up the cards. Finished attacking? Tap Done.",
            "ios.tut_5": "Empty your hand before everyone else. The last player holding cards is the fool. Good luck!",
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
            "ios.tut_next": "Понятно",
            "ios.tut_done": "Начать игру",
            "ios.tut_1": "Добро пожаловать в Дурак. Младшая козырная карта определяет, кто атакует первым. Козырь бьёт любую другую масть.",
            "ios.tut_2": "Чтобы атаковать, коснитесь карты в руке. Соперник должен её побить — или взять всё.",
            "ios.tut_3": "Чтобы защищаться, коснитесь своей карты, затем атаки, которую она бьёт. Побить можно старшей картой той же масти или козырем.",
            "ios.tut_4": "Не можете или не хотите защищаться? Нажмите «Взять». Закончили атаку? Нажмите «Бито».",
            "ios.tut_5": "Избавьтесь от карт раньше всех. Последний с картами — дурак. Удачи!",
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
            "ios.tut_next": "알겠어요",
            "ios.tut_done": "게임 시작",
            "ios.tut_1": "두락에 오신 것을 환영합니다. 으뜸패의 가장 낮은 카드가 첫 공격자를 정합니다. 으뜸패는 다른 모든 무늬를 이깁니다.",
            "ios.tut_2": "공격하려면 손패의 카드를 탭하세요. 상대는 그것을 이기거나 전부 가져가야 합니다.",
            "ios.tut_3": "방어하려면 자신의 카드를 탭한 뒤, 이길 공격 카드를 탭하세요. 같은 무늬의 더 높은 카드나 으뜸패로 막습니다.",
            "ios.tut_4": "막을 수 없거나 막지 않으려면 ‘가져오기’를 탭하세요. 공격을 마쳤으면 ‘완료’를 탭하세요.",
            "ios.tut_5": "누구보다 먼저 손패를 비우세요. 마지막까지 카드를 든 사람이 바보입니다. 행운을 빕니다!",
        ],
    ]
}
