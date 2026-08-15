// FStrings.swift — localization access (§5.5: all text through the table from
// day one; en/ru/ko). v1 backs this with an in-code trilingual table so every
// string is localized immediately. Milestone E4 (scripts/gen_ios_strings.mjs)
// replaces this table with a generated Localizable.xcstrings String Catalog
// merged from the web's src/localization/strings.ts — the API (`FStrings.t`)
// stays, the backing store changes. Keys keep the web's names; iOS-only keys
// use the `ios.` prefix (§16.E4).

import Foundation

public enum AppLanguage: String, CaseIterable, Sendable {
    // No "System": the owner found it confusing. We resolve the OS locale ONCE
    // (default English) and land on a concrete language, which the user can then
    // switch among these three (see FStrings.override).
    case en, ru, ko
    public var display: String {
        switch self {
        case .en: return "English"
        case .ru: return "Русский"
        case .ko: return "한국어"
        }
    }
}

public enum FStrings {
    /// The active language (§16.E3). On first use we detect it from the OS locale
    /// (English fallback) and land on a concrete choice; once the user picks one
    /// it is persisted. There is no "System" option any more.
    public static var override: AppLanguage {
        get {
            if let raw = UserDefaults.standard.string(forKey: "ios.language"),
               let lang = AppLanguage(rawValue: raw) { return lang }
            return systemDetected
        }
        set { UserDefaults.standard.set(newValue.rawValue, forKey: "ios.language") }
    }

    /// The OS locale mapped onto our three languages, English by default. This is
    /// the initial `override` until the user makes an explicit choice.
    private static var systemDetected: AppLanguage {
        let pref = Locale.preferredLanguages.first ?? "en"
        if pref.hasPrefix("ru") { return .ru }
        if pref.hasPrefix("ko") { return .ko }
        return .en
    }

    private static var activeLang: String { override.rawValue }

    /// Look up `key`, interpolating {name}-style placeholders from `args`.
    public static func t(_ key: String, _ args: [String: String] = [:]) -> String {
        let lang = activeLang
        var s = table[lang]?[key] ?? table["en"]?[key] ?? key
        for (k, v) in args { s = s.replacingOccurrences(of: "{\(k)}", with: v) }
        return s
    }

    /// A human reason for a rejected move (1.0(4)). The kernel's 21
    /// ENGINE_REJECT_* codes (c/src/game.h, surfaced by fio_last_reject) fold
    /// into a handful of clear, non-code-y sentences. `code` 0 / unknown falls
    /// back to the generic "That move isn't allowed."
    public static func rejectReason(_ code: Int) -> String {
        switch code {
        case 1, 4, 8, 18: return t("ios.rej.turn")     // NOT_PLAYING/NOT_DEFENDER/NOT_FIRST_ATTACKER/NOT_IN_STATUS
        case 2:            return t("ios.rej.pickone")  // EMPTY
        case 3:            return t("ios.rej.defending")// IS_DEFENDER
        case 5, 6:         return t("ios.rej.notyours") // NOT_IN_HAND/DUPLICATES
        case 7, 9:         return t("ios.rej.addrank")  // NOT_SAME_VALUE/VALUE_NOT_ON_TABLE
        case 11, 12, 13, 15: return t("ios.rej.cover")  // NO_UNCOVERED/ATTACK_NOT_ON_TABLE/CANNOT_COVER/COVER_PRESENT
        case 10, 17, 21:   return t("ios.rej.capacity") // DEFENDER_CAPACITY/PASS_CAPACITY/PASS_OVERFLOW
        case 16:           return t("ios.rej.passrank") // PASS_VALUES
        case 20:           return t("ios.rej.mustattack")// FIRST_MUST_ATTACK
        case 19:           return t("ios.rej.alreadygood")// ALREADY_GOOD
        case 14:           return t("ios.rej.notake")   // NO_TABLE_CARDS
        default:           return t("ios.reject")
        }
    }

    // Spoken card names for VoiceOver (round-5 m2): the visible strings were
    // localized while every accessibilityLabel was English, so a ru/ko VoiceOver
    // user got an English board. One builder, shared by every board component,
    // so "queen of spades" / "дама, пики" / "스페이드 퀸" cannot drift apart
    // between the hand, the battles and the deck. Rank indices are the kernel's
    // (9='10' … 13='A', CardRank.label's mapping); numeric ranks stay digits,
    // which VoiceOver reads in its own language already.

    public static func spokenRank(_ value: Int) -> String {
        switch value {
        case 13: return t("ios.rank.ace")
        case 12: return t("ios.rank.king")
        case 11: return t("ios.rank.queen")
        case 10: return t("ios.rank.jack")
        case 9:  return t("ios.rank.ten")
        default: return String(value + 1)
        }
    }

    public static func spokenSuit(_ suit: Suit) -> String {
        switch suit {
        case .spades: return t("ios.suit.spades")
        case .hearts: return t("ios.suit.hearts")
        case .clubs: return t("ios.suit.clubs")
        case .diamonds: return t("ios.suit.diamonds")
        }
    }

    public static func spokenCard(_ value: Int, _ suit: Suit) -> String {
        t("ios.a11y.card", ["rank": spokenRank(value), "suit": spokenSuit(suit)])
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
            // "Start playing", not "Start game" — round-5 m10: the lobby's two
            // buttons read as near-synonyms ("Create game" / "Start game"); the
            // verb keeps them distinct.
            "ios.msg.startgame": "Start playing",
            "ios.msg.gameon": "Foolish - game on! Tap to play",
            "ios.msg.joininvite": "Foolish - tap to join",
            "ios.msg.invite": "Send invite",
            "ios.msg.nickname_ph": "your nickname",
            // Round-6 #18: the owner wrote it "Enter Nickname" - Title Case,
            // unlike this file's usual sentence case ("Create game") - so this
            // ONE string breaks the convention on purpose. Leave the rest alone.
            "ios.msg.entername": "Enter Nickname",
            "ios.msg.nametoolong": "nickname too long",
            // 1.0(4): descriptive bubble summaries - the collapsed / notification
            // line describes the move the bubble carries ("Alex attacks with K of
            // ♠"), not a generic "tap to play". The move facts come from the
            // kernel's own evwire (MessageSummary), the words are localized here.
            "ios.msg.cardfmt": "{rank} of {suit}",
            "ios.msg.seatn": "Seat {n}",
            "ios.msg.mv.attack": "{name} attacks with {cards}",
            "ios.msg.mv.pass": "{name} passes {cards}",
            "ios.msg.mv.cover": "{name} covers {target} with {card}",
            "ios.msg.mv.pickup": "{name} took the cards",
            "ios.msg.mv.out": "{name} is out!",
            "ios.msg.mv.roundover": "Round over - {name} attacks next",
            "ios.msg.started": "{name} started the game - tap to play",
            "ios.msg.joined": "{name} joined - tap to join",
            // 1.0(4): descriptive reject reasons (the plain white flash on the
            // message board). The 21 ENGINE_REJECT_* codes fold into a few clear,
            // non-code-y reasons (FStrings.rejectReason).
            "ios.rej.turn": "It’s not your turn.",
            "ios.rej.pickone": "Pick a card first.",
            "ios.rej.defending": "You’re defending - beat the attack or take it.",
            "ios.rej.notyours": "That card isn’t in your hand.",
            "ios.rej.addrank": "You can only add a rank already on the table.",
            "ios.rej.cover": "That won’t beat it - play higher, or a trump.",
            "ios.rej.capacity": "Too many attacks for the defender’s hand.",
            "ios.rej.passrank": "You can only pass with a matching rank.",
            "ios.rej.mustattack": "The first attacker has to attack.",
            "ios.rej.alreadygood": "You already said good.",
            "ios.rej.notake": "There’s nothing to take.",
            // 1.0(4): the board's left Settings/Help squares + the rules page.
            "ios.help": "Help",
            "ios.done": "Done",
            "ios.settings.title": "Settings",
            "ios.settings.language": "Language",
            "ios.rules.title": "How to play",
            "ios.rules.goal.h": "The goal",
            "ios.rules.goal.b": "You attack and defend with cards, and the goal is to get rid of all of yours. The last player still holding cards is the дурак - the fool.",
            "ios.rules.deck.h": "The deck & the power suit",
            "ios.rules.deck.b": "Four players or fewer use only 6 up to Ace - the 2s through 5s are out. Everyone gets six cards, then one more is flipped face-up under the draw pile. Its suit is the power suit: a power-suit card beats any other suit, whatever the value. Whoever holds the lowest power-suit card attacks first.",
            "ios.rules.attack.h": "Attacking",
            "ios.rules.attack.b": "To start a round, put a card on the table toward the next player - they’re the defender now. Tap a card then tap Attack, or just drag it onto the table.",
            "ios.rules.defend.h": "Defending",
            "ios.rules.defend.b": "As the defender, beat each attack with a higher card of the same suit. Tap your card then tap the attack it covers, or just drag it on. A stack never grows past two - the attack on the bottom, its cover on top.",
            "ios.rules.defend.trump": "…or any card from the power suit.",
            "ios.rules.throw.h": "Throwing in",
            "ios.rules.throw.b": "See a card on the table with the same value as one in your hand? Throw it in as another attack. The table can blow up fast - that’s part of the fun - but the uncovered attacks can never outnumber the defender’s hand.",
            "ios.rules.takegood.h": "Take, or good",
            "ios.rules.takegood.b": "Can’t or won’t cover it all? Take every card on the table into your hand - your turn is skipped. Once everything’s covered and nobody wants to throw in more, tap Good and the table goes to the discard, face down.",
            "ios.rules.pass.h": "Passing it on",
            "ios.rules.pass.b": "If nothing’s covered yet and you hold the same value, lay it down and pass the whole attack to the next player instead of defending. Now they’re the defender.",
            "ios.rules.win.h": "Drawing & winning",
            "ios.rules.win.b": "Between rounds everyone tops back up to six from the draw pile - attacker first, defender last. Once the pile is empty, clear your hand to get out. The last player still holding cards is the fool, and gets attacked first next game.",
            "ios.a11y.attackfirst": "You attack first",
            "ios.a11y.defending": "Defending",
            "ios.a11y.attacking": "attacking",
            "ios.a11y.saidgood": "said good",
            "ios.a11y.thinking": "thinking",
            "ios.a11y.out": "out of the game",
            "ios.a11y.cards": "{n} cards",
            "ios.a11y.deck": "{n} cards left in the deck",
            "ios.a11y.trump": "trump {suit}",
            "ios.a11y.trumpmark": "trump",
            "ios.a11y.discard": "{n} cards discarded",
            "ios.a11y.covered": "{attack}, covered by {defense}",
            "ios.a11y.uncovered": "{attack}, uncovered",
            "ios.a11y.hiddencard": "hidden card",
            "ios.a11y.facedown": "face down card",
            "ios.a11y.card": "{rank} of {suit}",
            "ios.suit.spades": "spades", "ios.suit.hearts": "hearts",
            "ios.suit.clubs": "clubs", "ios.suit.diamonds": "diamonds",
            "ios.rank.ace": "ace", "ios.rank.king": "king",
            "ios.rank.queen": "queen", "ios.rank.jack": "jack", "ios.rank.ten": "ten",
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
            "ios.msg.startgame": "Начать играть",
            "ios.msg.gameon": "Дурак - игра началась! Нажмите, чтобы играть",
            "ios.msg.joininvite": "Дурак - нажмите, чтобы присоединиться",
            "ios.msg.invite": "Отправить приглашение",
            "ios.msg.nickname_ph": "ваш ник",
            // Round-6 #18: capitalised to match the English "Enter Nickname" -
            // only the first letter, per Russian convention (title-casing every
            // word would be wrong here).
            "ios.msg.entername": "Введите ник",
            "ios.msg.nametoolong": "слишком длинный ник",
            "ios.msg.cardfmt": "{rank} {suit}",
            "ios.msg.seatn": "Игрок {n}",
            "ios.msg.mv.attack": "{name} атакует: {cards}",
            "ios.msg.mv.pass": "{name} переводит: {cards}",
            "ios.msg.mv.cover": "{name} кроет {target}: {card}",
            "ios.msg.mv.pickup": "{name} забирает карты",
            "ios.msg.mv.out": "{name} вышел!",
            "ios.msg.mv.roundover": "Раунд окончен - атакует {name}",
            "ios.msg.started": "{name} начал игру - нажмите, чтобы играть",
            "ios.msg.joined": "{name} присоединился - нажмите, чтобы войти",
            "ios.rej.turn": "Сейчас не ваш ход.",
            "ios.rej.pickone": "Сначала выберите карту.",
            "ios.rej.defending": "Вы защищаетесь - побейте атаку или возьмите.",
            "ios.rej.notyours": "Этой карты нет в вашей руке.",
            "ios.rej.addrank": "Подкинуть можно только карту достоинства, что уже на столе.",
            "ios.rej.cover": "Так не побить - сыграйте старше или козырем.",
            "ios.rej.capacity": "Слишком много карт для руки защищающегося.",
            "ios.rej.passrank": "Переводить можно только картой того же достоинства.",
            "ios.rej.mustattack": "Первый игрок должен атаковать.",
            "ios.rej.alreadygood": "Вы уже сказали «бито».",
            "ios.rej.notake": "Брать нечего.",
            "ios.help": "Помощь",
            "ios.done": "Готово",
            "ios.settings.title": "Настройки",
            "ios.settings.language": "Язык",
            "ios.rules.title": "Как играть",
            "ios.rules.goal.h": "Цель",
            "ios.rules.goal.b": "Вы атакуете и защищаетесь картами, а цель - избавиться от всех своих. Последний игрок с картами на руках - дурак.",
            "ios.rules.deck.h": "Колода и козырь",
            "ios.rules.deck.b": "Вчетвером и меньше играют только от шестёрки до туза - двойки-пятёрки убирают. Каждому сдают шесть карт, затем ещё одну кладут в открытую под колоду. Её масть - козырь: козырная карта бьёт любую другую масть, каким бы ни было достоинство. Первым ходит тот, у кого младший козырь.",
            "ios.rules.attack.h": "Атака",
            "ios.rules.attack.b": "Чтобы начать раунд, положите карту на стол в сторону следующего игрока - теперь он защищается. Коснитесь карты и нажмите «Атака» или просто перетащите её на стол.",
            "ios.rules.defend.h": "Защита",
            "ios.rules.defend.b": "Защищаясь, бейте каждую атаку старшей картой той же масти. Коснитесь своей карты и нажмите на атаку, которую кроете, или перетащите её. Стопка не бывает больше двух: снизу атака, сверху защита.",
            "ios.rules.defend.trump": "…или любым козырем.",
            "ios.rules.throw.h": "Подкидывание",
            "ios.rules.throw.b": "Видите на столе карту того же достоинства, что и у вас в руке? Подкиньте её как ещё одну атаку. Стол может разрастись быстро - в этом часть азарта - но открытых атак не может быть больше, чем карт у защищающегося.",
            "ios.rules.takegood.h": "Взять или бито",
            "ios.rules.takegood.b": "Не можете или не хотите отбиться? Заберите все карты со стола в руку - ваш ход пропускается. Когда всё покрыто и больше никто не подкидывает, нажмите «Бито», и стол уходит в отбой рубашкой вверх.",
            "ios.rules.pass.h": "Перевод",
            "ios.rules.pass.b": "Если ещё ничего не покрыто и у вас есть карта того же достоинства, положите её и переведите всю атаку на следующего игрока вместо защиты. Теперь защищается он.",
            "ios.rules.win.h": "Добор и победа",
            "ios.rules.win.b": "Между раундами все добирают до шести из колоды - сначала атакующий, защищающийся последним. Когда колода кончилась, избавьтесь от карт, чтобы выйти. Последний с картами - дурак, и в следующей игре его атакуют первым.",
            "ios.a11y.attackfirst": "Вы ходите первым",
            "ios.a11y.defending": "Защищается",
            "ios.a11y.attacking": "атакует",
            "ios.a11y.saidgood": "сказал бито",
            "ios.a11y.thinking": "думает",
            "ios.a11y.out": "вышел из игры",
            "ios.a11y.cards": "карт: {n}",
            "ios.a11y.deck": "карт в колоде: {n}",
            "ios.a11y.trump": "козырь: {suit}",
            "ios.a11y.trumpmark": "козырь",
            "ios.a11y.discard": "карт в отбое: {n}",
            "ios.a11y.covered": "{attack}, покрыта: {defense}",
            "ios.a11y.uncovered": "{attack}, не покрыта",
            "ios.a11y.hiddencard": "закрытая карта",
            "ios.a11y.facedown": "карта рубашкой вверх",
            "ios.a11y.card": "{rank}, {suit}",
            "ios.suit.spades": "пики", "ios.suit.hearts": "черви",
            "ios.suit.clubs": "трефы", "ios.suit.diamonds": "бубны",
            "ios.rank.ace": "туз", "ios.rank.king": "король",
            "ios.rank.queen": "дама", "ios.rank.jack": "валет", "ios.rank.ten": "десятка",
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
            "ios.msg.startgame": "플레이 시작",
            "ios.msg.gameon": "두락 - 게임 시작! 탭하여 플레이",
            "ios.msg.joininvite": "두락 - 탭하여 참가",
            "ios.msg.invite": "초대 보내기",
            "ios.msg.nickname_ph": "닉네임",
            "ios.msg.entername": "닉네임을 입력하세요",
            "ios.msg.nametoolong": "닉네임이 너무 깁니다",
            "ios.msg.cardfmt": "{rank} {suit}",
            "ios.msg.seatn": "{n}번 자리",
            "ios.msg.mv.attack": "{name} 공격: {cards}",
            "ios.msg.mv.pass": "{name} 넘김: {cards}",
            "ios.msg.mv.cover": "{name}, {target} 방어: {card}",
            "ios.msg.mv.pickup": "{name} 카드 가져감",
            "ios.msg.mv.out": "{name} 탈락!",
            "ios.msg.mv.roundover": "라운드 종료 - 다음 공격: {name}",
            "ios.msg.started": "{name} 게임 시작 - 탭하여 플레이",
            "ios.msg.joined": "{name} 참가 - 탭하여 참가",
            "ios.rej.turn": "당신 차례가 아닙니다.",
            "ios.rej.pickone": "먼저 카드를 선택하세요.",
            "ios.rej.defending": "방어 중입니다 - 공격을 막거나 가져오세요.",
            "ios.rej.notyours": "그 카드는 손패에 없습니다.",
            "ios.rej.addrank": "테이블에 이미 있는 숫자만 추가할 수 있습니다.",
            "ios.rej.cover": "그걸로는 못 이깁니다 - 더 높거나 으뜸패를 내세요.",
            "ios.rej.capacity": "방어자 손패에 비해 공격이 너무 많습니다.",
            "ios.rej.passrank": "같은 숫자로만 넘길 수 있습니다.",
            "ios.rej.mustattack": "첫 공격자는 공격해야 합니다.",
            "ios.rej.alreadygood": "이미 완료를 선언했습니다.",
            "ios.rej.notake": "가져올 카드가 없습니다.",
            "ios.help": "도움말",
            "ios.done": "완료",
            "ios.settings.title": "설정",
            "ios.settings.language": "언어",
            "ios.rules.title": "게임 방법",
            "ios.rules.goal.h": "목표",
            "ios.rules.goal.b": "카드로 공격하고 방어하며, 목표는 손패를 모두 없애는 것입니다. 마지막까지 카드를 든 사람이 바보(두락)입니다.",
            "ios.rules.deck.h": "덱과 으뜸패",
            "ios.rules.deck.b": "네 명 이하면 6부터 에이스까지만 씁니다 - 2~5는 뺍니다. 각자 여섯 장을 받고, 한 장을 더 뒤집어 뽑을 더미 아래에 둡니다. 그 무늬가 으뜸패입니다: 으뜸패 카드는 값에 상관없이 다른 모든 무늬를 이깁니다. 가장 낮은 으뜸패를 든 사람이 먼저 공격합니다.",
            "ios.rules.attack.h": "공격",
            "ios.rules.attack.b": "라운드를 시작하려면 다음 플레이어 쪽으로 카드를 냅니다 - 이제 그 사람이 방어자입니다. 카드를 탭한 뒤 공격을 누르거나, 그냥 테이블로 끌어다 놓으세요.",
            "ios.rules.defend.h": "방어",
            "ios.rules.defend.b": "방어자는 각 공격을 같은 무늬의 더 높은 카드로 막습니다. 자신의 카드를 탭한 뒤 막을 공격을 누르거나 끌어다 놓으세요. 한 더미는 둘을 넘지 않습니다 - 아래가 공격, 위가 방어.",
            "ios.rules.defend.trump": "…또는 아무 으뜸패로.",
            "ios.rules.throw.h": "추가 공격",
            "ios.rules.throw.b": "테이블에 손패와 같은 숫자의 카드가 보이나요? 또 다른 공격으로 던져 넣으세요. 테이블이 순식간에 커질 수 있는데 - 그게 재미의 일부입니다 - 다만 막지 않은 공격은 방어자의 손패 수를 넘을 수 없습니다.",
            "ios.rules.takegood.h": "가져오기 또는 완료",
            "ios.rules.takegood.b": "다 막을 수 없거나 막기 싫나요? 테이블의 카드를 전부 손에 가져오세요 - 이번 차례는 건너뜁니다. 모두 막히고 아무도 더 던지지 않으면 완료를 눌러 테이블을 뒷면으로 버립니다.",
            "ios.rules.pass.h": "넘기기",
            "ios.rules.pass.b": "아직 아무것도 막지 않았고 같은 숫자가 있다면, 그것을 내고 방어 대신 공격 전체를 다음 플레이어에게 넘기세요. 이제 그 사람이 방어자입니다.",
            "ios.rules.win.h": "보충과 승리",
            "ios.rules.win.b": "라운드 사이에 모두 뽑을 더미에서 여섯 장까지 채웁니다 - 공격자부터, 방어자가 마지막. 더미가 비면 손패를 비워야 빠져나갑니다. 마지막까지 카드를 든 사람이 바보이고, 다음 게임에서 먼저 공격받습니다.",
            "ios.a11y.attackfirst": "당신이 먼저 공격합니다",
            "ios.a11y.defending": "방어 중",
            "ios.a11y.attacking": "공격 중",
            "ios.a11y.saidgood": "완료 선언",
            "ios.a11y.thinking": "생각 중",
            "ios.a11y.out": "탈락",
            "ios.a11y.cards": "카드 {n}장",
            "ios.a11y.deck": "덱에 카드 {n}장 남음",
            "ios.a11y.trump": "으뜸패: {suit}",
            "ios.a11y.trumpmark": "으뜸패",
            "ios.a11y.discard": "버린 카드 {n}장",
            "ios.a11y.covered": "{attack}, {defense}(으)로 방어됨",
            "ios.a11y.uncovered": "{attack}, 방어되지 않음",
            "ios.a11y.hiddencard": "뒷면 카드",
            "ios.a11y.facedown": "뒷면 카드",
            "ios.a11y.card": "{suit} {rank}",
            "ios.suit.spades": "스페이드", "ios.suit.hearts": "하트",
            "ios.suit.clubs": "클럽", "ios.suit.diamonds": "다이아몬드",
            "ios.rank.ace": "에이스", "ios.rank.king": "킹",
            "ios.rank.queen": "퀸", "ios.rank.jack": "잭", "ios.rank.ten": "10",
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
