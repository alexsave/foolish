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
            "pass": "Pass", "pickup": "Pickup", "good": "Good", "attack": "Attack", "cover": "Cover",
            "game_over": "Game over", "you_win": "You win", "you_lose": "You are the fool",
            "rematch": "Rematch", "share_replay": "Share replay", "home": "Home",
            "choose_opponent": "Choose opponent", "start_game": "Start game",
            "players": "Players", "thinking": "Thinking…",
            "leave_game_title": "Leave this game?", "leave_game_body": "The game is still live.",
            "leave": "Leave", "cancel": "Cancel",
            "ios.lobby": "Lobby", "ios.game_code": "Game code", "ios.ready": "Ready",
            "ios.add_bot": "Add bot", "ios.share_invite": "Share invite", "join_game": "Join game",
            "ios.dashboard": "Dashboard", "ios.create_game": "Create game", "ios.sign_out": "Sign out",
            "ios.online_soon": "Online play arrives in a later update.",
            "ios.reject": "That move isn’t allowed.",
            "ios.you": "You",
            "ios.fool": "Fool",
            "ios.nobattle": "no battle",
            "ios.msg.yourmove": "Your move",
            "ios.msg.staged": "Move staged - hit Send",
            "ios.msg.waiting": "Waiting for the others",
            "ios.msg.waitingfor": "Waiting for {name}",
            "ios.msg.send": "Send move",
            "ios.msg.sending": "Sending…",
            "ios.msg.undo": "Undo",
            "ios.msg.newgame": "New game",
            "ios.msg.pickseat": "Which player are you?",
            "ios.msg.spectating": "Spectating — open the game from your own bubble to play",
            "ios.msg.thread": "A game in this thread",
            "ios.msg.tap": "Durak - tap to play",
            "ios.msg.damaged": "This game link is damaged.",
            "ios.msg.open": "Open the game",
            "ios.msg.fool": "{name} is the fool - tap to watch",
            "ios.msg.isfool": "{name} is the fool",
            "ios.msg.moved": "This game has moved on.",
            "ios.msg.opennewest": "Open the latest",
            "ios.msg.viewanyway": "View this anyway",
            "ios.msg.rebased": "Your move was re-applied - hit Send to confirm",
            "ios.msg.superseded": "Your move was superseded",
            "ios.msg.yourname": "Your name",
            "ios.msg.nameprompt": "What should we call you?",
            "ios.msg.continue": "Continue",
            "ios.msg.seatopen": "Open seat",
            "ios.msg.joinas": "Join as {name}",
            "ios.msg.waitingjoin": "Waiting for {n} more",
            "ios.msg.lobbyfull": "All seats taken",
            "ios.msg.creategame": "Create game",
            "ios.msg.startgame": "Start game",
            "ios.msg.gameon": "Foolish - game on! Tap to play",
            "ios.msg.joininvite": "Foolish - tap to join",
            "ios.msg.invite": "Send invite",
            "ios.tut_next": "Got it",
            "ios.tut_done": "Start playing",
            "ios.tut_1": "Welcome to Durak. The lowest card of the trump suit decides who attacks first. Trump beats every other suit.",
            "ios.tut_2": "To attack, tap a card in your hand. Your opponent must beat it - or pick it all up.",
            "ios.tut_3": "To defend, tap one of your cards, then tap the attack it beats. A higher card of the same suit, or any trump, covers it.",
            "ios.tut_4": "Can’t or won’t defend? Tap Pickup to pick up the cards. Finished attacking? Tap Done.",
            "ios.tut_5": "Empty your hand before everyone else. The last player holding cards is the fool. Good luck!",
            "ios.bot.random": "Miami", "ios.bot.handwritten": "New York",
            "ios.bot.robusta": "Seoul", "ios.bot.firecracker": "Madrid",
            "ios.bot.blackpowder": "Vienna", "ios.bot.cordite": "St. Petersburg",
            "ios.bot.octogen": "Moscow",
            "ios.bot.max": "Max", "ios.bot.km": "{km} km from Moscow",
            "ios.bot.km0": "The Kremlin itself",
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
            "ios.lobby": "Лобби", "ios.game_code": "Код игры", "ios.ready": "Готов",
            "ios.add_bot": "Добавить бота", "ios.share_invite": "Поделиться", "join_game": "Войти",
            "ios.dashboard": "Панель", "ios.create_game": "Создать игру", "ios.sign_out": "Выйти",
            "ios.online_soon": "Онлайн-игра появится в следующем обновлении.",
            "ios.reject": "Такой ход недопустим.",
            "ios.you": "Вы",
            "ios.fool": "Дурак",
            "ios.nobattle": "нет боя",
            "ios.msg.yourmove": "Ваш ход",
            "ios.msg.staged": "Ход готов - нажмите «Отправить»",
            "ios.msg.waiting": "Ждём других игроков",
            "ios.msg.waitingfor": "Ждём: {name}",
            "ios.msg.send": "Отправить ход",
            "ios.msg.sending": "Отправка…",
            "ios.msg.undo": "Отменить",
            "ios.msg.newgame": "Новая игра",
            "ios.msg.pickseat": "Кто вы из игроков?",
            "ios.msg.spectating": "Вы наблюдаете — чтобы играть, откройте игру из своего сообщения",
            "ios.msg.thread": "Игра в этой переписке",
            "ios.msg.tap": "Дурак - нажмите, чтобы играть",
            "ios.msg.damaged": "Ссылка на игру повреждена.",
            "ios.msg.open": "Открыть игру",
            "ios.msg.fool": "{name} - дурак - нажмите, чтобы посмотреть",
            "ios.msg.isfool": "{name} - дурак",
            "ios.msg.moved": "Игра ушла вперёд.",
            "ios.msg.opennewest": "Открыть последнее",
            "ios.msg.viewanyway": "Всё равно посмотреть",
            "ios.msg.rebased": "Ваш ход применён заново - нажмите «Отправить»",
            "ios.msg.superseded": "Ваш ход был перекрыт",
            "ios.msg.yourname": "Ваше имя",
            "ios.msg.nameprompt": "Как вас называть?",
            "ios.msg.continue": "Продолжить",
            "ios.msg.seatopen": "Свободное место",
            "ios.msg.joinas": "Войти как {name}",
            "ios.msg.waitingjoin": "Ждём ещё: {n}",
            "ios.msg.lobbyfull": "Все места заняты",
            "ios.msg.creategame": "Создать игру",
            "ios.msg.startgame": "Начать игру",
            "ios.msg.gameon": "Дурак - игра началась! Нажмите, чтобы играть",
            "ios.msg.joininvite": "Дурак - нажмите, чтобы присоединиться",
            "ios.msg.invite": "Отправить приглашение",
            "ios.tut_next": "Понятно",
            "ios.tut_done": "Начать игру",
            "ios.tut_1": "Добро пожаловать в Дурак. Младшая козырная карта определяет, кто атакует первым. Козырь бьёт любую другую масть.",
            "ios.tut_2": "Чтобы атаковать, коснитесь карты в руке. Соперник должен её побить - или взять всё.",
            "ios.tut_3": "Чтобы защищаться, коснитесь своей карты, затем атаки, которую она бьёт. Побить можно старшей картой той же масти или козырем.",
            "ios.tut_4": "Не можете или не хотите защищаться? Нажмите «Взять». Закончили атаку? Нажмите «Бито».",
            "ios.tut_5": "Избавьтесь от карт раньше всех. Последний с картами - дурак. Удачи!",
            "ios.bot.random": "Майами", "ios.bot.handwritten": "Нью-Йорк",
            "ios.bot.robusta": "Сеул", "ios.bot.firecracker": "Мадрид",
            "ios.bot.blackpowder": "Вена", "ios.bot.cordite": "Петербург",
            "ios.bot.octogen": "Москва",
            "ios.bot.max": "Макс", "ios.bot.km": "{km} км от Москвы",
            "ios.bot.km0": "Сам Кремль",
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
            "ios.lobby": "로비", "ios.game_code": "게임 코드", "ios.ready": "준비",
            "ios.add_bot": "봇 추가", "ios.share_invite": "초대 공유", "join_game": "참가",
            "ios.dashboard": "대시보드", "ios.create_game": "게임 만들기", "ios.sign_out": "로그아웃",
            "ios.online_soon": "온라인 플레이는 다음 업데이트에서 제공됩니다.",
            "ios.reject": "허용되지 않는 수입니다.",
            "ios.you": "나",
            "ios.fool": "바보",
            "ios.nobattle": "전투 없음",
            "ios.msg.yourmove": "당신 차례",
            "ios.msg.staged": "수 준비됨 - 전송하세요",
            "ios.msg.waiting": "다른 플레이어 대기 중",
            "ios.msg.waitingfor": "{name} 대기 중",
            "ios.msg.send": "수 보내기",
            "ios.msg.sending": "보내는 중…",
            "ios.msg.undo": "취소",
            "ios.msg.newgame": "새 게임",
            "ios.msg.pickseat": "당신은 어느 플레이어인가요?",
            "ios.msg.spectating": "관전 중 — 플레이하려면 내 말풍선에서 게임을 여세요",
            "ios.msg.thread": "이 대화의 게임",
            "ios.msg.tap": "두락 - 탭하여 플레이",
            "ios.msg.damaged": "게임 링크가 손상되었습니다.",
            "ios.msg.open": "게임 열기",
            "ios.msg.fool": "{name}이(가) 바보입니다 - 탭하여 보기",
            "ios.msg.isfool": "{name}이(가) 바보입니다",
            "ios.msg.moved": "이 게임은 이미 진행되었습니다.",
            "ios.msg.opennewest": "최신 상태 열기",
            "ios.msg.viewanyway": "그래도 이 상태 보기",
            "ios.msg.rebased": "당신의 수가 다시 적용되었습니다 - 전송하세요",
            "ios.msg.superseded": "당신의 수가 대체되었습니다",
            "ios.msg.yourname": "이름",
            "ios.msg.nameprompt": "이름을 알려주세요",
            "ios.msg.continue": "계속",
            "ios.msg.seatopen": "빈 자리",
            "ios.msg.joinas": "{name}(으)로 참가",
            "ios.msg.waitingjoin": "{n}명 더 기다리는 중",
            "ios.msg.lobbyfull": "모든 자리가 찼습니다",
            "ios.msg.creategame": "게임 만들기",
            "ios.msg.startgame": "게임 시작",
            "ios.msg.gameon": "두락 - 게임 시작! 탭하여 플레이",
            "ios.msg.joininvite": "두락 - 탭하여 참가",
            "ios.msg.invite": "초대 보내기",
            "ios.tut_next": "알겠어요",
            "ios.tut_done": "게임 시작",
            "ios.tut_1": "두락에 오신 것을 환영합니다. 으뜸패의 가장 낮은 카드가 첫 공격자를 정합니다. 으뜸패는 다른 모든 무늬를 이깁니다.",
            "ios.tut_2": "공격하려면 손패의 카드를 탭하세요. 상대는 그것을 이기거나 전부 가져가야 합니다.",
            "ios.tut_3": "방어하려면 자신의 카드를 탭한 뒤, 이길 공격 카드를 탭하세요. 같은 무늬의 더 높은 카드나 으뜸패로 막습니다.",
            "ios.tut_4": "막을 수 없거나 막지 않으려면 ‘가져오기’를 탭하세요. 공격을 마쳤으면 ‘완료’를 탭하세요.",
            "ios.tut_5": "누구보다 먼저 손패를 비우세요. 마지막까지 카드를 든 사람이 바보입니다. 행운을 빕니다!",
            "ios.bot.random": "마이애미", "ios.bot.handwritten": "뉴욕",
            "ios.bot.robusta": "서울", "ios.bot.firecracker": "마드리드",
            "ios.bot.blackpowder": "빈", "ios.bot.cordite": "상트페테르부르크",
            "ios.bot.octogen": "모스크바",
            "ios.bot.max": "맥스", "ios.bot.km": "모스크바에서 {km} km",
            "ios.bot.km0": "크렘린 그 자체",
        ],
    ]
}
