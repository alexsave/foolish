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
            "ios.rules.goal.b": "The ultimate goal is to get rid of all your cards as soon as possible. Whoever is left holding cards at the end is the дурак - the fool, in Russian.",
            "ios.rules.setup.h": "Game setup",
            "ios.rules.setup.b": "With four or fewer players, the game is played with only the cards 6 through Ace - the 2s, 3s, 4s and 5s are removed. Aces are high. Each player is dealt six cards, then one more card is drawn, flipped face up, and placed under the draw pile. The SUIT of that flipped card is the power suit. The power suit is special - see Covering.",
            "ios.rules.setup.cap": "One card is flipped under the draw pile - its suit is the power suit. Hearts, in every example on this page.",
            "ios.rules.start.h": "Game start",
            "ios.rules.start.b": "Whoever holds the lowest power-suit card goes first. In-person games settle this by asking around - anyone have the 6? a 7? - here the game works it out automatically. That player is the round’s first attacker.",
            "ios.rules.sword": "The sword marks the attacker who opens the round.",
            "ios.rules.shield": "The shield marks the defender.",
            "ios.rules.attack.h": "Attacking",
            "ios.rules.attack.b": "The first attacker may play any card, or any set of cards of the same VALUE, toward the player on their left (clockwise). That player is now the defender and must deal with the attack. Tap cards to select them and hit the Attack button that appears when the selection is a valid attack - or simply drag cards from your hand to the table.",
            "ios.rules.attack.ok1": "The Queen alone is a fine attack.",
            "ios.rules.attack.ok2": "Or both Kings at once - same value.",
            "ios.rules.attack.no1": "Never the 7 and the 10 together - different values.",
            "ios.rules.defend.h": "Defending",
            "ios.rules.defend.b": "There is only one defender at a time, and every card the other players put down is the defender’s to deal with. The defender may only cover, pick up, or pass (when passing is allowed) - they can never attack themselves. Everyone else is an attacker, and attackers can never cover.",
            "ios.rules.cover.h": "Covering",
            "ios.rules.cover.b": "The defender covers each attack card with a higher card of the same suit, or with ANY power-suit card - that is what makes the power suit special. Select a card and tap Cover, or drag it onto the card you want covered. A common mistake: a card that is already covered cannot be covered again - you will never see a stack of three. Think of an attack as a sword and its cover as a shield: a blocked attack is finished. New attacks start their own pile.",
            "ios.rules.cover.ok1": "A higher card of the same suit covers.",
            "ios.rules.cover.ok2": "Any power card covers any other suit - even a lower value.",
            "ios.rules.cover.no1": "A power attack needs a higher power card - this 7 is too low.",
            "ios.rules.cover.no2": "Higher value, wrong suit - that covers nothing.",
            "ios.rules.throw.h": "Throwing in",
            "ios.rules.throw.b": "While the defender covers, every other player may throw in more attacks. The rule is simple but takes a moment to grasp: if the VALUE of a card in your hand is anywhere on the table, you may add that card as a new, separate attack. Don’t worry about suits AT ALL. The only limit is the defender’s hand: uncovered attacks may never outnumber the cards the defender holds - they must be able to answer each attack with a card. You may wonder whether this makes the table multiply quickly. It does, and that is part of the fun.",
            "ios.rules.throw.ok1": "A 9 is on the table, so your 9 can go in - suits don’t matter.",
            "ios.rules.throw.ok2": "Both 6s at once - two new attacks.",
            "ios.rules.throw.ok3": "Or the 6 and the 9 together - each value is already out there.",
            "ios.rules.throw.no1": "No 8 and no Jack on the table - nothing to throw in.",
            "ios.rules.throw.no2": "The defender holds one card and one attack is already waiting - no room for another.",
            "ios.rules.pickup.h": "Picking up",
            "ios.rules.pickup.b": "If the defender cannot cover everything - or doesn’t want to - they press Pickup and take EVERY card on the table into their hand, covered ones included. Their next turn is skipped: the player to their left opens the next round. So think carefully before you take.",
            "ios.rules.pickup.cap": "Pick up here and all three cards go to your hand - the covered pair too.",
            "ios.rules.round.h": "Round end",
            "ios.rules.round.b": "When every attack is covered and the attackers agree to stop throwing in, the table goes to the discard pile, out of the game. Then everyone draws back up to six cards - the first attacker first, the other attackers clockwise after, the defender last. The game handles this for you. A pickup ends the round the same way, the drawing included. Once the draw pile is empty, you play with what you hold.",
            "ios.rules.pass.h": "Passing",
            "ios.rules.pass.b": "In a game where переводной - passing - is allowed, the defender has one more option: pass the attack along. If nothing on the table is covered yet and you hold a card of the same VALUE as the attack cards, you may lay it down - the shield moves to the player on your left. The familiar limit applies: no passing if the next player holds fewer cards than the attack would then contain. Tap the matching card and press Pass, or drag it to an empty spot on the table.",
            "ios.rules.pass.ok1": "An 8 is attacking and you hold an 8 - lay it down and the shield moves on.",
            "ios.rules.pass.no1": "Something is already covered - too late to pass.",
            "ios.rules.pass.no2": "No 8 in your hand - this one is yours to defend.",
            "ios.rules.pass.no3": "Passing would make three attacks, and the next player only holds two cards.",
            "ios.rules.end.h": "Game end",
            "ios.rules.end.b": "Once the draw pile is empty, the game plays on. Empty your hand and you are safe. Whoever is last left holding cards is the fool.",
            "ios.rules.lbl.table": "on the table",
            "ios.rules.lbl.hand": "your hand",
            "ios.rules.defender": "Defender",
            "ios.rules.nextplayer": "Next player",
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
            "ios.rules.goal.h": "Цель игры",
            "ios.rules.goal.b": "Главная цель - как можно скорее избавиться от всех своих карт. Кто остался с картами в конце, тот и есть дурак.",
            "ios.rules.setup.h": "Подготовка",
            "ios.rules.setup.b": "Если игроков четверо или меньше, играют картами от шестёрки до туза - двойки, тройки, четвёрки и пятёрки убирают из колоды. Туз - старшая карта. Каждому сдают по шесть карт, затем ещё одну открывают и кладут под колоду. МАСТЬ этой карты - козырная. Чем козырь особенный - в разделе «Крыть».",
            "ios.rules.setup.cap": "Одну карту открывают и кладут под колоду - её масть и есть козырь. Во всех примерах на этой странице козыри червы.",
            "ios.rules.start.h": "Начало игры",
            "ios.rules.start.b": "Первым ходит тот, у кого младший козырь. Вживую это выясняют вопросами - у кого шестёрка? а семёрка? - здесь игра определяет это сама. Этот игрок - первый атакующий раунда.",
            "ios.rules.sword": "Меч - атакующий, который открывает раунд.",
            "ios.rules.shield": "Щит - защитник.",
            "ios.rules.attack.h": "Атака",
            "ios.rules.attack.b": "Первый атакующий может выложить любую карту - или несколько карт ОДНОГО достоинства - в сторону игрока слева (по часовой). Тот становится защитником и должен разобраться с атакой. Коснитесь карт, чтобы выбрать их, и нажмите «Атака», когда выбор складывается в допустимый ход, - или просто перетащите карты из руки на стол.",
            "ios.rules.attack.ok1": "Дама сама по себе - отличная атака.",
            "ios.rules.attack.ok2": "Или оба короля разом - достоинство одно.",
            "ios.rules.attack.no1": "Но не семёрка с десяткой вместе - достоинства разные.",
            "ios.rules.defend.h": "Защита",
            "ios.rules.defend.b": "Защитник всегда один, и все карты, выложенные остальными, - его забота. Защитник может только крыть, взять или перевести (если перевод разрешён) - сам он не атакует. Все остальные - атакующие, и крыть они не могут.",
            "ios.rules.cover.h": "Крыть",
            "ios.rules.cover.b": "Защитник кроет каждую карту атаки старшей картой той же масти - или ЛЮБЫМ козырем: в этом и сила козырной масти. Выберите карту и нажмите «Крыть» или перетащите её на карту, которую хотите покрыть. Частая ошибка: уже покрытую карту нельзя покрыть ещё раз - стопок из трёх карт не бывает. Атака - меч, покрытие - щит: отбитая атака кончена. Новые атаки кладутся отдельными стопками.",
            "ios.rules.cover.ok1": "Кроет старшая карта той же масти.",
            "ios.rules.cover.ok2": "Любой козырь кроет любую другую масть - даже младший.",
            "ios.rules.cover.no1": "Козырную атаку бьёт только старший козырь - эта семёрка мала.",
            "ios.rules.cover.no2": "Старше, но не той масти - ничего не кроет.",
            "ios.rules.throw.h": "Подкидывание",
            "ios.rules.throw.b": "Пока защитник отбивается, остальные могут подкидывать. Правило простое, но к нему привыкают: если ДОСТОИНСТВО карты из вашей руки уже есть где-то на столе, её можно подкинуть как новую отдельную атаку. О мастях не думайте ВООБЩЕ. Ограничение одно - рука защитника: непокрытых атак не может быть больше, чем у него карт, - на каждую атаку он должен суметь ответить картой. Может показаться, что так стол разрастается стремительно. Так и есть - и в этом часть азарта.",
            "ios.rules.throw.ok1": "Девятка есть на столе - значит, ваша девятка идёт в ход, масть не важна.",
            "ios.rules.throw.ok2": "Обе шестёрки разом - две новые атаки.",
            "ios.rules.throw.ok3": "Или шестёрка с девяткой вместе - оба достоинства уже на столе.",
            "ios.rules.throw.no1": "Ни восьмёрки, ни валета на столе - подкинуть нечего.",
            "ios.rules.throw.no2": "У защитника одна карта, и одна атака уже ждёт - больше некуда.",
            "ios.rules.pickup.h": "Взять",
            "ios.rules.pickup.b": "Если защитник не может покрыть всё - или не хочет - он нажимает «Взять» и забирает в руку ВСЕ карты со стола, включая уже покрытые. Его ход пропускается: следующий раунд открывает игрок слева. Так что брать или нет - решайте с умом.",
            "ios.rules.pickup.cap": "Возьмёте здесь - и все три карты уйдут в руку, покрытая пара тоже.",
            "ios.rules.round.h": "Конец раунда",
            "ios.rules.round.b": "Когда все атаки покрыты и никто больше не подкидывает, карты со стола уходят в отбой - они выбыли из игры. Затем все добирают до шести карт: сначала первый атакующий, за ним остальные атакующие по часовой, защитник - последним. Игра делает это сама. Если защитник взял, раунд кончается так же, и добор тот же. Когда колода опустела, играют тем, что на руках.",
            "ios.rules.pass.h": "Перевод",
            "ios.rules.pass.b": "В игре с переводом у защитника есть ещё один выход: перевести атаку дальше. Если на столе ещё ничего не покрыто и у вас есть карта ТОГО ЖЕ достоинства, что и карты атаки, положите её - и щит перейдёт к игроку слева от вас. Ограничение знакомое: перевести нельзя, если у следующего игрока меньше карт, чем станет атак после перевода. Коснитесь подходящей карты и нажмите «Пас» - или перетащите её на свободное место стола.",
            "ios.rules.pass.ok1": "Атакуют восьмёркой, и у вас есть восьмёрка - кладите, щит двинется дальше.",
            "ios.rules.pass.no1": "Что-то уже покрыто - переводить поздно.",
            "ios.rules.pass.no2": "Восьмёрки в руке нет - эта атака ваша.",
            "ios.rules.pass.no3": "После перевода атак станет три, а у следующего игрока всего две карты.",
            "ios.rules.end.h": "Конец игры",
            "ios.rules.end.b": "Когда колода пуста, игра продолжается. Избавились от карт - вы в безопасности. Последний, кто остался с картами, - дурак.",
            "ios.rules.lbl.table": "на столе",
            "ios.rules.lbl.hand": "ваша рука",
            "ios.rules.defender": "Защитник",
            "ios.rules.nextplayer": "Следующий игрок",
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
            "ios.rules.goal.h": "게임의 목표",
            "ios.rules.goal.b": "궁극의 목표는 손의 카드를 최대한 빨리 없애는 것입니다. 끝까지 카드를 들고 있는 사람이 дурак - 러시아어로 ‘바보’입니다.",
            "ios.rules.setup.h": "게임 준비",
            "ios.rules.setup.b": "플레이어가 네 명 이하면 6부터 에이스까지의 카드만 씁니다 - 2, 3, 4, 5는 뺍니다. 에이스가 가장 높습니다. 각자 여섯 장을 받은 뒤, 카드 한 장을 더 뽑아 앞면으로 뒤집어 뽑을 더미 아래에 둡니다. 그 카드의 무늬가 으뜸 무늬입니다. 으뜸 무늬가 왜 특별한지는 ‘막기’에서 설명합니다.",
            "ios.rules.setup.cap": "뽑을 더미 아래에 카드 한 장을 뒤집어 둡니다 - 그 무늬가 으뜸 무늬입니다. 이 페이지의 모든 예시는 하트입니다.",
            "ios.rules.start.h": "게임 시작",
            "ios.rules.start.b": "가장 낮은 으뜸 무늬 카드를 든 사람이 먼저 합니다. 실제 게임에서는 6 있나요? 7은요? 하고 물어 가며 정하지만, 여기서는 게임이 자동으로 정합니다. 그 사람이 이번 라운드의 첫 공격자입니다.",
            "ios.rules.sword": "검은 라운드를 여는 공격자를 나타냅니다.",
            "ios.rules.shield": "방패는 방어자를 나타냅니다.",
            "ios.rules.attack.h": "공격",
            "ios.rules.attack.b": "첫 공격자는 아무 카드 한 장, 또는 값이 같은 카드 여러 장을 왼쪽(시계 방향) 플레이어 쪽으로 낼 수 있습니다. 그 플레이어가 방어자가 되어 공격을 처리해야 합니다. 카드를 탭해 고르고, 유효한 공격이 되면 나타나는 공격 버튼을 누르세요 - 아니면 손에서 테이블로 끌어다 놓아도 됩니다.",
            "ios.rules.attack.ok1": "퀸 한 장으로도 훌륭한 공격입니다.",
            "ios.rules.attack.ok2": "킹 두 장을 한꺼번에 - 값이 같으니까요.",
            "ios.rules.attack.no1": "7과 10을 같이는 안 됩니다 - 값이 다릅니다.",
            "ios.rules.defend.h": "수비",
            "ios.rules.defend.b": "방어자는 한 번에 한 명뿐이고, 다른 플레이어들이 낸 카드는 전부 방어자의 몫입니다. 방어자는 막기, 가져오기, 넘기기(허용된 경우)만 할 수 있고 - 스스로 공격할 수는 없습니다. 나머지는 모두 공격자이며, 공격자는 막을 수 없습니다.",
            "ios.rules.cover.h": "막기",
            "ios.rules.cover.b": "방어자는 각 공격 카드를 같은 무늬의 더 높은 카드, 또는 아무 으뜸패로나 막습니다 - 이것이 으뜸 무늬의 힘입니다. 카드를 선택하고 방어 버튼을 누르거나, 막고 싶은 카드 위로 끌어다 놓으세요. 흔한 오해 하나: 이미 막힌 카드는 다시 막을 수 없습니다 - 석 장짜리 더미는 없습니다. 공격은 검, 막은 카드는 방패입니다: 막힌 공격은 끝난 것입니다. 새 공격은 새 더미로 놓입니다.",
            "ios.rules.cover.ok1": "같은 무늬의 더 높은 카드로 막습니다.",
            "ios.rules.cover.ok2": "으뜸패는 값이 낮아도 다른 무늬를 다 막습니다.",
            "ios.rules.cover.no1": "으뜸패 공격은 더 높은 으뜸패로만 - 이 7은 낮습니다.",
            "ios.rules.cover.no2": "값은 높아도 무늬가 다르면 못 막습니다.",
            "ios.rules.throw.h": "추가 공격",
            "ios.rules.throw.b": "방어자가 막는 동안 다른 플레이어들은 공격을 더 던져 넣을 수 있습니다. 규칙은 간단하지만 익숙해지는 데 시간이 좀 걸립니다: 손에 든 카드의 값이 테이블 위 어디든 이미 있다면, 그 카드를 새로운 별도의 공격으로 낼 수 있습니다. 무늬는 전혀 신경 쓰지 마세요. 한도는 방어자의 손 하나뿐입니다: 막지 않은 공격 수가 방어자가 든 카드 수를 넘을 수 없습니다 - 공격마다 카드 한 장으로 답할 수 있어야 하니까요. 이러면 테이블이 순식간에 불어나지 않을까요? 맞습니다. 그게 이 게임의 재미입니다.",
            "ios.rules.throw.ok1": "테이블에 9가 있으니 당신의 9를 낼 수 있습니다 - 무늬는 상관없습니다.",
            "ios.rules.throw.ok2": "6 두 장을 한꺼번에 - 새 공격 둘.",
            "ios.rules.throw.ok3": "6과 9를 같이 - 두 값 모두 이미 나와 있습니다.",
            "ios.rules.throw.no1": "테이블에 8도 잭도 없으니 던질 게 없습니다.",
            "ios.rules.throw.no2": "방어자 손엔 한 장, 이미 공격 하나가 기다립니다 - 더는 안 됩니다.",
            "ios.rules.pickup.h": "가져오기",
            "ios.rules.pickup.b": "다 막을 수 없거나 막고 싶지 않다면, 방어자는 가져오기 버튼을 누릅니다. 그러면 테이블의 모든 카드를 - 이미 막힌 것까지 - 손에 가져오고, 다음 차례를 건너뜁니다: 다음 라운드는 왼쪽 플레이어가 엽니다. 그러니 가져올지는 신중히 정하세요.",
            "ios.rules.pickup.cap": "여기서 가져오면 석 장 모두 - 막힌 쌍까지 - 손에 들어옵니다.",
            "ios.rules.round.h": "라운드 종료",
            "ios.rules.round.b": "모든 공격이 막히고 공격자들이 더 던지지 않기로 하면, 테이블의 카드는 전부 버린 더미로 갑니다. 그리고 모두 여섯 장이 될 때까지 뽑습니다 - 첫 공격자부터, 나머지 공격자들이 시계 방향으로, 방어자가 마지막. 게임이 자동으로 처리합니다. 방어자가 가져와도 라운드는 같은 방식으로 끝나고, 뽑기도 같습니다. 뽑을 더미가 비면 손에 든 카드로 승부합니다.",
            "ios.rules.pass.h": "넘기기",
            "ios.rules.pass.b": "переводной - ‘넘기기’가 허용된 게임에서는 방어자에게 선택지가 하나 더 있습니다: 공격을 옆으로 넘기는 것. 테이블에 막힌 카드가 하나도 없고, 공격 카드들과 같은 값의 카드를 들고 있다면, 그 카드를 내려놓으세요 - 방패가 왼쪽 플레이어에게 넘어갑니다. 익숙한 한도가 그대로 적용됩니다: 넘긴 뒤의 공격 수가 다음 플레이어의 카드 수보다 많아지면 넘길 수 없습니다. 값이 맞는 카드를 탭하고 패스를 누르거나, 테이블의 빈자리로 끌어다 놓으세요.",
            "ios.rules.pass.ok1": "8이 공격 중인데 8을 들고 있다면 - 내려놓으세요, 방패가 넘어갑니다.",
            "ios.rules.pass.no1": "이미 막힌 카드가 있으면 - 넘기기엔 늦었습니다.",
            "ios.rules.pass.no2": "손에 8이 없으니 - 이번엔 직접 막아야 합니다.",
            "ios.rules.pass.no3": "넘기면 공격이 셋, 다음 플레이어 카드는 두 장뿐입니다.",
            "ios.rules.end.h": "게임 종료",
            "ios.rules.end.b": "뽑을 더미가 비어도 게임은 계속됩니다. 손을 다 비우면 바보가 될 일은 없습니다. 마지막까지 카드를 들고 있는 사람이 바보입니다.",
            "ios.rules.lbl.table": "테이블",
            "ios.rules.lbl.hand": "내 손",
            "ios.rules.defender": "방어자",
            "ios.rules.nextplayer": "다음 플레이어",
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
