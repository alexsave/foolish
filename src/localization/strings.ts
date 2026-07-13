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
  | 'add_bot_named'
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
  | 'username_reserved'
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
  | 'playback_speed'
  | 'play'
  | 'pause'
  | 'replay_step_back'
  | 'replay_step_forward'
  | 'replay_bout_start'
  | 'replay_bout_next'
  | 'replay_draw'
  | 'replay_draw_clear'
  | 'leaderboard'
  | 'match_history'
  | 'rating'
  | 'games_label'
  | 'tab_all'
  | 'tab_humans'
  | 'tab_bots'
  | 'no_ranked_players'
  | 'result_survived'
  | 'result_fool'
  | 'watch_replay'
  | 'no_match_history'
  | 'your_stats'
  | 'survival_rate'
  | 'times_fool'
  | 'load_failed'
  | 'oracle_panel_title'
  | 'oracle_button_title'
  | 'oracle_no_decision'
  | 'oracle_analyzing'
  | 'oracle_converged'
  | 'oracle_exact'
  | 'oracle_memory'
  | 'oracle_memory_on'
  | 'oracle_memory_off'
  | 'oracle_played'
  | 'oracle_best'
  | 'oracle_pruned'
  | 'oracle_pruned_tip'
  | 'oracle_approx'
  | 'oracle_basis'
  | 'oracle_memory_off_endgame'
  | 'oracle_forced_loss'
  | 'oracle_forced_move'
  | 'oracle_ef_tip'
  | 'oracle_unavailable'
  | 'oracle_retry'
  | 'oracle_class_best'
  | 'oracle_class_excellent'
  | 'oracle_class_good'
  | 'oracle_class_inaccuracy'
  | 'oracle_class_mistake'
  | 'oracle_class_blunder';

// English strings
const strings_en: Record<StringId, string> = {
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
  add_bot_named: 'Add {name}',
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
  username_reserved: 'That username is reserved. Usernames can’t contain the “%” symbol.',
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
  play: 'Play',
  pause: 'Pause',
  replay_step_back: 'Step back',
  replay_step_forward: 'Step forward',
  replay_bout_start: 'Start of bout',
  replay_bout_next: 'Next bout',
  replay_draw: 'Draw (press C)',
  replay_draw_clear: 'Clear drawing (press C)',
  leaderboard: 'Leaderboard',
  match_history: 'Match History',
  rating: 'Rating',
  games_label: 'Games',
  tab_all: 'All',
  tab_humans: 'Humans',
  tab_bots: 'Bots',
  no_ranked_players: 'No rated players yet — finish a game to appear here.',
  result_survived: 'Survived',
  result_fool: 'The fool',
  watch_replay: 'Watch replay',
  no_match_history: 'No recorded games yet. Games you finish will show up here with a replay.',
  your_stats: 'Your stats (listed games)',
  survival_rate: 'Survival rate',
  times_fool: 'Times the fool',
  load_failed: 'Could not load — try again later',
  oracle_panel_title: 'Move Oracle',
  oracle_button_title: 'Oracle: move strength',
  oracle_no_decision: 'No move to analyze yet',
  oracle_analyzing: 'Analyzing… {n} worlds · {rate}/s',
  oracle_converged: 'Converged',
  oracle_exact: 'Exact (solved)',
  oracle_memory: 'Memory',
  oracle_memory_on: 'On — octogen remembers the whole game',
  oracle_memory_off: 'Off — octogen forgets the history (human-like)',
  oracle_played: 'played',
  oracle_best: 'best',
  oracle_pruned: 'not considered',
  oracle_pruned_tip: 'Octogen never sampled this move — it ranked below its consideration set.',
  oracle_approx: 'Approximate position (some hidden cards inferred).',
  oracle_basis: 'Based on the publicly visible record.',
  oracle_memory_off_endgame: 'Exact endgame proofs need Memory on.',
  oracle_forced_loss: 'proven loss',
  oracle_forced_move: 'Forced — no alternatives to compare',
  oracle_ef_tip: 'Expected finishing place over the sampled worlds (lower is better).',
  oracle_unavailable: 'Oracle failed to load',
  oracle_retry: 'Retry',
  oracle_class_best: 'best',
  oracle_class_excellent: 'excellent',
  oracle_class_good: 'good',
  oracle_class_inaccuracy: 'inaccuracy',
  oracle_class_mistake: 'mistake',
  oracle_class_blunder: 'blunder',
};

// Russian strings
const strings_ru: Record<StringId, string> = {
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
  add_bot_named: 'Добавить {name}',
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
  username_reserved: 'Это имя пользователя зарезервировано. Имя не может содержать символ «%».',
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
  play: 'Воспроизвести',
  pause: 'Пауза',
  replay_step_back: 'Шаг назад',
  replay_step_forward: 'Шаг вперёд',
  replay_bout_start: 'Начало кона',
  replay_bout_next: 'Следующий кон',
  replay_draw: 'Рисовать (клавиша C)',
  replay_draw_clear: 'Стереть рисунок (клавиша C)',
  leaderboard: 'Таблица лидеров',
  match_history: 'История игр',
  rating: 'Рейтинг',
  games_label: 'Игры',
  tab_all: 'Все',
  tab_humans: 'Люди',
  tab_bots: 'Боты',
  no_ranked_players: 'Пока нет игроков с рейтингом — доиграйте партию, чтобы попасть сюда.',
  result_survived: 'Не дурак',
  result_fool: 'Дурак',
  watch_replay: 'Смотреть повтор',
  no_match_history: 'Пока нет записанных игр. Завершённые партии появятся здесь с повтором.',
  your_stats: 'Ваша статистика (по списку игр)',
  survival_rate: 'Процент выживания',
  times_fool: 'Раз дураком',
  load_failed: 'Не удалось загрузить — попробуйте позже',
  oracle_panel_title: 'Оракул хода',
  oracle_button_title: 'Оракул: сила хода',
  oracle_no_decision: 'Пока нечего анализировать',
  oracle_analyzing: 'Анализ… {n} миров · {rate}/с',
  oracle_converged: 'Сошлось',
  oracle_exact: 'Точно (решено)',
  oracle_memory: 'Память',
  oracle_memory_on: 'Вкл — октоген помнит всю партию',
  oracle_memory_off: 'Выкл — октоген забыл историю (как человек)',
  oracle_played: 'сыграно',
  oracle_best: 'лучший',
  oracle_pruned: 'не рассмотрен',
  oracle_pruned_tip: 'Октоген не пробовал этот ход — он не вошёл в число рассматриваемых.',
  oracle_approx: 'Приблизительная позиция (часть скрытых карт выведена).',
  oracle_basis: 'На основе публично видимой записи.',
  oracle_memory_off_endgame: 'Точные эндшпильные доказательства требуют включённой памяти.',
  oracle_forced_loss: 'доказанный проигрыш',
  oracle_forced_move: 'Вынужденно — не с чем сравнивать',
  oracle_ef_tip: 'Ожидаемое место в сыгранных мирах (меньше — лучше).',
  oracle_unavailable: 'Не удалось загрузить оракула',
  oracle_retry: 'Повторить',
  oracle_class_best: 'лучший',
  oracle_class_excellent: 'отличный',
  oracle_class_good: 'хороший',
  oracle_class_inaccuracy: 'неточность',
  oracle_class_mistake: 'ошибка',
  oracle_class_blunder: 'грубая ошибка',
};

// Korean strings
const strings_ko: Record<StringId, string> = {
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
  add_bot_named: '{name} 추가',
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
  username_reserved: '사용할 수 없는 사용자 이름입니다. 이름에 “%” 기호를 포함할 수 없습니다.',
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
  play: '재생',
  pause: '일시정지',
  replay_step_back: '뒤로 한 단계',
  replay_step_forward: '앞으로 한 단계',
  replay_bout_start: '판 시작',
  replay_bout_next: '다음 판',
  replay_draw: '그리기 (C 키)',
  replay_draw_clear: '그림 지우기 (C 키)',
  leaderboard: '리더보드',
  match_history: '경기 기록',
  rating: '레이팅',
  games_label: '게임 수',
  tab_all: '전체',
  tab_humans: '사람',
  tab_bots: '봇',
  no_ranked_players: '아직 랭킹에 오른 플레이어가 없습니다. 게임을 끝내면 여기에 표시됩니다.',
  result_survived: '생존',
  result_fool: '바보',
  watch_replay: '리플레이 보기',
  no_match_history: '아직 기록된 게임이 없습니다. 완료한 게임이 리플레이와 함께 여기에 표시됩니다.',
  your_stats: '내 통계 (목록의 게임 기준)',
  survival_rate: '생존율',
  times_fool: '바보가 된 횟수',
  load_failed: '불러오지 못했습니다. 나중에 다시 시도하세요',
  oracle_panel_title: '수 오라클',
  oracle_button_title: '오라클: 수의 강도',
  oracle_no_decision: '아직 분석할 수가 없습니다',
  oracle_analyzing: '분석 중… {n}개 월드 · 초당 {rate}',
  oracle_converged: '수렴됨',
  oracle_exact: '정확 (풀이됨)',
  oracle_memory: '기억',
  oracle_memory_on: '켬 — 옥토겐이 전체 게임을 기억',
  oracle_memory_off: '끔 — 옥토겐이 기록을 잊음 (사람처럼)',
  oracle_played: '실제 수',
  oracle_best: '최선',
  oracle_pruned: '고려 안 됨',
  oracle_pruned_tip: '옥토겐이 이 수를 표본으로 삼지 않았습니다 — 고려 대상에 들지 못했습니다.',
  oracle_approx: '근사 포지션 (일부 숨겨진 카드는 추론됨).',
  oracle_basis: '공개된 기록을 기반으로 함.',
  oracle_memory_off_endgame: '정확한 엔드게임 증명에는 기억이 켜져 있어야 합니다.',
  oracle_forced_loss: '증명된 패배',
  oracle_forced_move: '강제 — 비교할 대안 없음',
  oracle_ef_tip: '표본 월드에서의 예상 순위 (낮을수록 좋음).',
  oracle_unavailable: '오라클을 불러오지 못했습니다',
  oracle_retry: '다시 시도',
  oracle_class_best: '최선',
  oracle_class_excellent: '훌륭함',
  oracle_class_good: '좋음',
  oracle_class_inaccuracy: '부정확',
  oracle_class_mistake: '실수',
  oracle_class_blunder: '대실수',
};

// Combined strings object for easy access
export const strings: Record<string, Record<StringId, string>> = {
  en: strings_en,
  ru: strings_ru,
  ko: strings_ko,
};

