// String resource IDs for localization
export type StringId = 
  | 'playing'
  | 'waiting'
  | 'ready'
  | 'dashboard'
  | 'dashboard_title'
  | 'create_new_game'
  | 'enter_game_id'
  | 'join'
  | 'deck_cards'
  | 'username'
  | 'password'
  | 'login'
  | 'sign_up'
  | 'foolish'
  | 'pickup'
  | 'good'
  | 'pass'
  | 'cover'
  | 'attack'
  | 'game_over'
  | 'join_game'
  | 'add_bot'
  | 'remove_bot'
  | 'exit_game'
  | 'id'
  | 'no_games_available'
  | 'click_to_edit'
  | 'defender'
  | 'first_attacker'
  | 'winner'
  | 'spectating'
  | 'loading'
  | 'about'
  | 'about_foolish'
  | 'about_paragraph_1'
  | 'about_paragraph_2'
  | 'about_paragraph_3'
  | 'back_to_home'
  | 'chat'
  | 'send'
  | 'type_message'
  | 'final_rankings'
  | 'you'
  | 'continue_to_lobby'
  | 'weak_password'
  | 'login_failed'
  | 'signup_failed'
  | 'share_replay'
  | 'copy_code'
  | 'copied'
  | 'replay_unavailable'
  | 'replay_title'
  | 'invalid_replay'
  | 'is_the_fool'
  | 'is_out'
  | 'discarded'
  | 'draws'
  | 'trump'
  | 'reveal_cards'
  | 'hide_cards'
  | 'with_names_time'
  | 'playback_speed';

// English strings
export const strings_en: Record<StringId, string> = {
  playing: 'Playing',
  waiting: 'Waiting',
  ready: 'Ready',
  dashboard: 'Dashboard',
  dashboard_title: '{username}\'s Dashboard',
  create_new_game: 'Create New Game',
  enter_game_id: 'Enter existing game ID',
  join: 'Join',
  deck_cards: 'Cards in deck',
  username: 'Username',
  password: 'Password',
  login: 'Login',
  sign_up: 'Sign Up',
  foolish: 'FOOLISH',
  pickup: 'Pickup',
  good: 'Good',
  pass: 'Pass',
  cover: 'Cover',
  attack: 'Attack',
  game_over: 'Game Over',
  join_game: 'Join Game',
  add_bot: 'Add Bot',
  remove_bot: 'Remove Bot',
  exit_game: 'Exit Game',
  id: 'ID',
  no_games_available: 'No games available. Create a new game to get started!',
  click_to_edit: 'Click to edit game name',
  defender: 'Defender',
  first_attacker: 'First Attacker',
  winner: 'Winner',
  spectating: 'Spectating',
  loading: 'Loading...',
  about: 'About',
  about_foolish: 'About FOOLISH',
  about_paragraph_1: 'This is a classic Russian card game known as дурак. I brought this game to school years ago. My parents taught me one version. But what I taught to my classmates got a bit muddled up. The game evolved a bit as we played at lunch, usually with 8 players at a time. As a result, the core rules still stand but with a few chaotic changes.',
  about_paragraph_2: 'Passing (Переводная) is always enabled. When throwing in attacks, it\'s first come first serve as to who gets the card in. And when attacking, you can put down as many cards as the defender has even if it\'s more than 6. You can imagine this gets very crazy and very fun with 8 players. It adds an element of fast decision making. Go big or go home. I call this American Fool. Hence the website name Foolish.',
  about_paragraph_3: 'What started as a simple project to play the game with friends across the world turned into a lot of learning for me. This has game AIs, optimistic animations with reverts, and workarounds for the limits of Supabase\'s free tier.',
  back_to_home: 'Back to Home',
  chat: 'Chat',
  send: 'Send',
  type_message: 'Type message...',
  final_rankings: 'Final Rankings',
  you: 'You',
  continue_to_lobby: 'Continue to Lobby',
  weak_password: 'Weak password',
  login_failed: 'Login failed',
  signup_failed: 'Sign up failed',
  share_replay: 'Share Replay',
  copy_code: 'Copy code',
  copied: 'Copied!',
  replay_unavailable: 'Replay code unavailable for this game',
  replay_title: 'Replay',
  invalid_replay: 'This replay link is invalid',
  is_the_fool: 'is the fool',
  is_out: 'is out',
  discarded: 'discarded',
  draws: 'draws',
  trump: 'Trump',
  reveal_cards: 'Show all hands',
  hide_cards: 'Hide hands',
  with_names_time: 'Names + timing',
  playback_speed: 'Playback speed (× real timing)',
};

// Russian strings
export const strings_ru: Record<StringId, string> = {
  playing: 'В игре',
  waiting: 'Ожидание',
  ready: 'Готов',
  dashboard: 'Главная',
  dashboard_title: 'Главная {username}',
  create_new_game: 'Создать новую игру',
  enter_game_id: 'Введите ID игры',
  join: 'Присоединиться',
  deck_cards: 'Карт в колоде',
  username: 'Имя пользователя',
  password: 'Пароль',
  login: 'Войти',
  sign_up: 'Регистрация',
  foolish: 'ДУРАЦКИЙ',
  pickup: 'Беру',
  good: 'Бито',
  pass: 'Перевожу',
  cover: 'Крою',
  attack: 'Подкидываю',
  game_over: 'Игра окончена',
  join_game: 'Присоединиться к игре',
  add_bot: 'Добавить бота',
  remove_bot: 'Удалить бота',
  exit_game: 'Выйти из игры',
  id: 'ID',
  no_games_available: 'Нет доступных игр. Создайте новую игру, чтобы начать!',
  click_to_edit: 'Нажмите, чтобы изменить название игры',
  defender: 'Отбивающийся',
  first_attacker: 'Заходящий',
  winner: 'Победитель',
  spectating: 'Наблюдение',
  loading: 'Загрузка...',
  about: 'О нас',
  about_foolish: 'О ДУРАЦКИЙ',
  about_paragraph_1: strings_en.about_paragraph_1,
  about_paragraph_2: strings_en.about_paragraph_2,
  about_paragraph_3: strings_en.about_paragraph_3,
  back_to_home: 'Вернуться на главную',
  chat: 'Чат',
  send: 'Отправить',
  type_message: 'Введите сообщение...',
  final_rankings: 'Итоговые результаты',
  you: 'Вы',
  continue_to_lobby: 'Продолжить в лобби',
  weak_password: 'Слабый пароль',
  login_failed: 'Не удалось войти',
  signup_failed: 'Не удалось зарегистрироваться',
  share_replay: 'Поделиться повтором',
  copy_code: 'Скопировать код',
  copied: 'Скопировано!',
  replay_unavailable: 'Код повтора недоступен для этой игры',
  replay_title: 'Повтор',
  invalid_replay: 'Эта ссылка на повтор недействительна',
  is_the_fool: '— дурак',
  is_out: 'вышел',
  discarded: 'бито',
  draws: 'берёт',
  trump: 'Козырь',
  reveal_cards: 'Показать все карты',
  hide_cards: 'Скрыть карты',
  with_names_time: 'Имена и время',
  playback_speed: 'Скорость воспроизведения (× реальное время)',
};

// Korean strings
export const strings_ko: Record<StringId, string> = {
  playing: '게임 중',
  waiting: '대기 중',
  ready: '준비 완료',
  dashboard: '대시보드',
  dashboard_title: '{username}의 대시보드',
  create_new_game: '새 게임 만들기',
  enter_game_id: '기존 게임 ID 입력',
  join: '참가',
  deck_cards: '덱 카드 수',
  username: '사용자 이름',
  password: '비밀번호',
  login: '로그인',
  sign_up: '가입',
  foolish: '바보같은',
  pickup: '가져가기',
  good: '방어 완료',
  pass: '패스',
  cover: '방어',
  attack: '공격',
  game_over: '게임 종료',
  join_game: '게임 참가',
  add_bot: '봇 추가',
  remove_bot: '봇 제거',
  exit_game: '게임 나가기',
  id: 'ID',
  no_games_available: '사용 가능한 게임이 없습니다. 새 게임을 만들어 시작하세요!',
  click_to_edit: '클릭하여 게임 이름 수정',
  defender: '수비자',
  first_attacker: '선공',
  winner: '승자',
  spectating: '관전 중',
  loading: '로딩 중...',
  about: '소개',
  about_foolish: 'FOOLISH 소개',
  about_paragraph_1: strings_en.about_paragraph_1,
  about_paragraph_2: strings_en.about_paragraph_2,
  about_paragraph_3: strings_en.about_paragraph_3,
  back_to_home: '홈으로 돌아가기',
  chat: '채팅',
  send: '전송',
  type_message: '메시지 입력...',
  final_rankings: '최종 순위',
  you: '나',
  continue_to_lobby: '로비로 이동',
  weak_password: '비밀번호가 너무 약합니다',
  login_failed: '로그인 실패',
  signup_failed: '가입 실패',
  share_replay: '리플레이 공유',
  copy_code: '코드 복사',
  copied: '복사됨!',
  replay_unavailable: '이 게임의 리플레이 코드를 사용할 수 없습니다',
  replay_title: '리플레이',
  invalid_replay: '이 리플레이 링크가 잘못되었습니다',
  is_the_fool: '바보입니다',
  is_out: '탈락',
  discarded: '버림',
  draws: '뽑기',
  trump: '으뜸패',
  reveal_cards: '모든 패 보기',
  hide_cards: '패 숨기기',
  with_names_time: '이름 + 시간',
  playback_speed: '재생 속도 (실제 시간 ×)',
};

// Combined strings object for easy access
export const strings: Record<string, Record<StringId, string>> = {
  en: strings_en,
  ru: strings_ru,
  ko: strings_ko,
};

