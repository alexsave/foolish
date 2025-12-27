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
  | 'loading';

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
};

// Combined strings object for easy access
export const strings: Record<string, Record<StringId, string>> = {
  en: strings_en,
  ru: strings_ru,
  ko: strings_ko,
};

