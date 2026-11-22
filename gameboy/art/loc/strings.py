# The Game Boy's words in each language (design mockups only, nothing live).
# Source of each term: "repo:KEY" = foolish/c/i18n/strings_<lang>.c, the same
# table the website and the iOS app use; "chosen" = picked here (no repo key).
#
# Keys: title, subtitle, continue, pick_flight, departures, fly, boss,
# miami, new_york, seoul, moscow, thinks, defend, pick_up, target, cover,
# lose_big, lose_1, lose_2, rematch, home, win_big, is_durak

S = {
"en": {
    "title": "DURAK", "subtitle": "FOOLISH", "continue": "CONTINUE", "pick_flight": "PICK A FLIGHT",
    "departures": "DEPARTURES", "fly": "FLY", "boss": "BOSS",
    "miami": "MIAMI", "new_york": "NEW YORK", "seoul": "SEOUL", "moscow": "MOSCOW",
    "thinks": "THINKS", "defend": "YOU DEFEND", "pick_up": "PICK UP", "target": "TARGET", "cover": "COVER",
    "lose_big": "DURAK!", "lose_1": "YOU ARE", "lose_2": "THE FOOL", "rematch": "REMATCH", "home": "HOME",
    "win_big": "YOU WIN", "is_durak": "IS THE DURAK",
},
"ru": {
    "title": "ДУРАК",                 # repo:IOS_FOOL
    "subtitle": "ДУРАЦКИЙ",           # repo:FOOLISH
    "continue": "ПРОДОЛЖИТЬ",         # repo:IOS_MSG_CONTINUE
    "pick_flight": "ВЫБЕРИТЕ РЕЙС",   # chosen
    "departures": "ВЫЛЕТЫ",           # chosen (an airport board's own word)
    "fly": "В ПУТЬ",                  # chosen
    "boss": "БОСС",                   # chosen
    "miami": "МАЙАМИ", "new_york": "НЬЮ-ЙОРК", "seoul": "СЕУЛ", "moscow": "МОСКВА",   # repo:BOT_*
    "thinks": "ДУМАЕТ",               # repo:IOS_A11Y_THINKING
    "defend": "ЗАЩИТА",               # repo:IOS_RULES_DEFEND_H ("ВЫ ЗАЩИЩАЕТЕСЬ" overflows the rail)
    "pick_up": "ВЗЯТЬ",               # repo:PICKUP
    "target": "ЦЕЛЬ",                 # chosen
    "cover": "КРЫТЬ",                 # repo:COVER
    "lose_big": "ДУРАК!",             # repo:IOS_FOOL
    "lose_1": "ВЫ ОСТАЛИСЬ", "lose_2": "В ДУРАКАХ",   # chosen: the idiom "to be left the fool"
    "rematch": "ЕЩЁ РАЗ",             # repo:REMATCH
    "home": "НА ГЛАВНУЮ",             # repo:HOME
    "win_big": "ПОБЕДА!",             # chosen (repo:WINNER is "Победитель")
    "is_durak": "В ДУРАКАХ",          # chosen idiom; repo:IOS_MSG_ISFOOL is "{name} - дурак"
},
"ko": {
    "title": "두락",                  # repo:IOS_MSG_TAP (the game's name in Korean)
    "subtitle": "바보같은",           # repo:FOOLISH
    "continue": "계속",               # repo:IOS_MSG_CONTINUE
    "pick_flight": "항공편 선택",     # chosen
    "departures": "출발",             # chosen
    "fly": "탑승",                    # chosen ("board")
    "boss": "보스",                   # chosen
    "miami": "마이애미", "new_york": "뉴욕", "seoul": "서울", "moscow": "모스크바",   # repo:BOT_*
    "thinks": "생각 중",              # repo:IOS_A11Y_THINKING
    "defend": "방어 중",              # repo:IOS_A11Y_DEFENDING
    "pick_up": "가져오기",            # repo:PICKUP
    "target": "목표",                 # chosen
    "cover": "막기",                  # repo:COVER
    "lose_big": "바보!",              # repo:IOS_FOOL
    "lose_1": "당신이", "lose_2": "바보입니다",   # repo:IOS_MSG_ISFOOL pattern ("{name} 님이 바보입니다")
    "rematch": "재대결",              # repo:REMATCH
    "home": "홈",                     # repo:HOME
    "win_big": "승리!",               # chosen
    "is_durak": "님이 바보입니다",    # repo:IOS_MSG_ISFOOL
},
"ja": {
    "title": "ドゥラーク",            # repo:IOS_FOOL
    "subtitle": "おバカ",             # repo:FOOLISH
    "continue": "続ける",             # repo:IOS_MSG_CONTINUE
    "pick_flight": "便を選ぶ",        # chosen
    "departures": "出発",             # chosen
    "fly": "のる",                    # chosen (kana for 乗る)
    "boss": "ボス",                   # chosen
    "miami": "マイアミ", "new_york": "ニューヨーク", "seoul": "ソウル", "moscow": "モスクワ",   # repo:BOT_*
    "thinks": "考え中",               # repo:IOS_A11Y_THINKING
    "defend": "防御中",               # repo:IOS_A11Y_DEFENDING
    "pick_up": "取る",                # repo:PICKUP
    "target": "ねらう",               # chosen (kana)
    "cover": "受ける",                # repo:COVER
    "lose_big": "ドゥラーク!",        # repo:IOS_FOOL
    "lose_1": "あなたが", "lose_2": "ドゥラーク",   # repo:IOS_MSG_ISFOOL pattern ("{name} がドゥラーク")
    "rematch": "もう一度",            # repo:REMATCH
    "home": "ホーム",                 # repo:HOME
    "win_big": "勝利!",               # chosen
    "is_durak": "がドゥラーク",       # repo:IOS_MSG_ISFOOL
},
"zh": {
    "title": "傻瓜",                  # repo:IOS_FOOL (the game is 傻瓜牌, repo:IOS_MSG_TAP)
    "subtitle": "傻乎乎",             # repo:FOOLISH
    "continue": "继续",               # repo:IOS_MSG_CONTINUE
    "pick_flight": "选择航班",        # chosen
    "departures": "出发",             # chosen
    "fly": "登机",                    # chosen
    "boss": "头目",                   # chosen
    "miami": "迈阿密", "new_york": "纽约", "seoul": "首尔", "moscow": "莫斯科",   # repo:BOT_*
    "thinks": "思考中",               # repo:IOS_A11Y_THINKING
    "defend": "防守中",               # repo:IOS_A11Y_DEFENDING
    "pick_up": "拿牌",                # repo:PICKUP
    "target": "目标",                 # chosen
    "cover": "盖牌",                  # repo:COVER
    "lose_big": "傻瓜!",              # repo:IOS_FOOL
    "lose_1": "你是", "lose_2": "傻瓜",   # repo:IOS_MSG_ISFOOL pattern ("{name} 是傻瓜")
    "rematch": "再来一局",            # repo:REMATCH
    "home": "主页",                   # repo:HOME
    "win_big": "胜利!",               # chosen
    "is_durak": "是傻瓜",             # repo:IOS_MSG_ISFOOL
},
}

# Traditional Chinese would differ in: 继续->繼續, 选择航班->選擇航班, 出发->出發,
# 登机->登機, 头目->頭目, 迈阿密->邁阿密, 纽约->紐約, 首尔->首爾, 盖牌->蓋牌,
# 目标->目標, 再来一局->再來一局, 主页->主頁, 胜利->勝利.  傻瓜, 莫斯科, 拿牌,
# 思考中, 防守中, 是 are the same.
