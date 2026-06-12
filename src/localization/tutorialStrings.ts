/* Tutorial narration strings, kept separate from the main string table so the
 * teaching script can grow without bloating the shared StringId union. Indexed
 * by the active Language from LocalizationContext. */
import { Language } from '../contexts/LocalizationContext';

export type TutKey =
  | 'title'
  | 'subtitle'
  | 'start'
  | 'skip'
  | 'next'
  | 'replay'
  | 'exit'
  | 'your_move'
  | 'press_button'
  | 'press_or_drag'
  | 'drag_tip'
  // narration beats
  | 'intro'
  | 'goal'
  | 'deck_low'
  | 'trump'
  | 'first_attacker'
  | 'first_attacker_you'
  | 'attack'
  | 'cover'
  | 'trump_cover'
  | 'stack_rule'
  | 'throw_in'
  | 'capacity'
  | 'pass'
  | 'pickup'
  | 'pickup_skip'
  | 'good'
  | 'discard'
  | 'draw'
  | 'deck_empty'
  | 'out'
  | 'fool'
  | 'done';

type Dict = Record<TutKey, string>;

const en: Dict = {
  title: 'How to Play Foolish',
  subtitle: 'A quick hands-on game of дурак. Green shows your move.',
  start: 'Start',
  skip: 'Skip tutorial',
  next: 'Next',
  replay: 'Play the tutorial again',
  exit: 'Go to a real game',
  your_move: 'Your move',
  press_or_drag: 'Press the highlighted button below — or drag the green card onto the table.',
  press_button: 'Press the highlighted button below.',
  drag_tip: 'You play by tapping a card and pressing a button. You can also drag a card onto the table.',

  intro:
    'You attack and defend with cards. First to empty their hand is safe; the last one still holding cards is the fool (дурак).',
  goal:
    'With four players or fewer the 2s–5s are removed — only 6 and up are used. Everyone is dealt 6 cards.',
  deck_low:
    'One more card is flipped under the deck. This is the draw pile, and its suit is the trump (power) suit.',
  trump:
    'The flipped card sets the trump suit. A trump beats any card of another suit, no matter its number.',
  first_attacker:
    'Play starts with the lowest trump. {name} leads the first attack.',
  first_attacker_you:
    'You hold the lowest trump, so you lead the first attack.',
  attack:
    'An attack is a card placed toward the player clockwise — the defender ({name}). They must answer it.',
  cover:
    'To cover, the defender beats a card with a higher card of the same suit.',
  trump_cover:
    'A trump covers any non-trump card — even a low trump beats a high card of another suit.',
  stack_rule:
    'Each stack stays at two cards: an attack on the bottom, its cover on top. A covered pair is done.',
  throw_in:
    'Any attacker may throw in another card whose value already sits on the table. Things can blow up fast — that’s the fun.',
  capacity:
    'But the uncovered cards can never outnumber the cards left in the defender’s hand.',
  pass:
    'Instead of covering, the defender can add a card of the same value and pass the whole attack to the next player.',
  pickup:
    'If the defender can’t — or won’t — answer, they pick up every card on the table.',
  pickup_skip:
    '{name} picked up, so their turn is skipped and the next player leads.',
  good:
    'Everything is covered and no one throws in more, so the attackers say “Good”.',
  discard:
    'The covered cards go to the discard pile face down. The defender held and now leads the next round.',
  draw:
    'Between rounds everyone refills back to six from the draw pile — attackers first, the defender last.',
  deck_empty:
    'The draw pile is empty. From here, whoever plays their last card is safe and out of the game.',
  out: '{name} played their final card — safe, and out of the game!',
  fool:
    '{name} is left holding cards — the fool! That’s the whole game. Ready to play for real?',
  done: 'You’ve learned every move. Nice work!',
};

const ru: Dict = {
  title: 'Как играть в «Дурацкий»',
  subtitle: 'Быстрая партия в дурака. Зелёным показан ваш ход.',
  start: 'Начать',
  skip: 'Пропустить обучение',
  next: 'Далее',
  replay: 'Пройти обучение снова',
  exit: 'К настоящей игре',
  your_move: 'Ваш ход',
  press_or_drag: 'Нажмите выделенную кнопку ниже — или перетащите зелёную карту на стол.',
  press_button: 'Нажмите выделенную кнопку ниже.',
  drag_tip: 'Вы играете, выбирая карту и нажимая кнопку. Также можно перетащить карту на стол.',

  intro:
    'Вы атакуете и отбиваетесь картами. Кто первым избавился от карт — в безопасности; последний с картами — дурак.',
  goal:
    'При четырёх игроках и меньше двойки–пятёрки убираются — играют только с шестёрок. Каждому раздают по 6 карт.',
  deck_low:
    'Ещё одну карту кладут под колоду. Это колода добора, и её масть — козырная.',
  trump:
    'Открытая карта задаёт козырную масть. Козырь бьёт любую карту другой масти, каким бы ни был номинал.',
  first_attacker:
    'Ходит первым тот, у кого младший козырь. {name} заходит первым.',
  first_attacker_you:
    'У вас младший козырь, поэтому вы заходите первым.',
  attack:
    'Атака — карта, положенная игроку по часовой стрелке: отбивающемуся ({name}). Он должен ответить.',
  cover:
    'Чтобы отбиться, защищающийся бьёт карту старшей картой той же масти.',
  trump_cover:
    'Козырь кроет любую некозырную карту — даже младший козырь бьёт старшую карту другой масти.',
  stack_rule:
    'В каждой стопке только две карты: атака снизу, защита сверху. Покрытая пара завершена.',
  throw_in:
    'Любой атакующий может подкинуть карту того же номинала, что уже на столе. Карт может стать очень много — в этом и веселье.',
  capacity:
    'Но непокрытых карт не может быть больше, чем карт в руке у отбивающегося.',
  pass:
    'Вместо защиты отбивающийся может положить карту того же номинала и перевести всю атаку на следующего игрока.',
  pickup:
    'Если защищающийся не может или не хочет отвечать, он забирает все карты со стола.',
  pickup_skip:
    '{name} забрал карты, поэтому его ход пропускается, и заходит следующий.',
  good:
    'Всё покрыто и больше никто не подкидывает, поэтому атакующие говорят «Бито».',
  discard:
    'Покрытые карты уходят в отбой рубашкой вверх. Защитник отбился и теперь заходит первым.',
  draw:
    'Между конами все добирают до шести из колоды — сначала атакующие, отбивающийся последним.',
  deck_empty:
    'Колода добора пуста. Теперь тот, кто сыграл последнюю карту, в безопасности и выходит из игры.',
  out: '{name} сыграл последнюю карту — в безопасности и вышел из игры!',
  fool:
    '{name} остался с картами — дурак! Вот и вся игра. Готовы сыграть по-настоящему?',
  done: 'Вы освоили все ходы. Отлично!',
};

const ko: Dict = {
  title: '풀리시 게임 방법',
  subtitle: '직접 해보는 두락(дурак) 한 판. 초록색이 당신의 차례입니다.',
  start: '시작',
  skip: '튜토리얼 건너뛰기',
  next: '다음',
  replay: '튜토리얼 다시 하기',
  exit: '실제 게임으로',
  your_move: '당신의 차례',
  press_or_drag: '아래의 강조된 버튼을 누르세요 — 또는 초록색 카드를 테이블로 끌어다 놓으세요.',
  press_button: '아래의 강조된 버튼을 누르세요.',
  drag_tip: '카드를 탭하고 버튼을 눌러 플레이합니다. 카드를 테이블로 드래그할 수도 있습니다.',

  intro:
    '카드로 공격하고 방어합니다. 먼저 손을 비우면 안전하고, 마지막까지 카드를 든 사람이 바보(두락)입니다.',
  goal:
    '4명 이하일 때는 2~5는 빼고 6 이상만 사용합니다. 각자 6장을 받습니다.',
  deck_low:
    '한 장을 더 뒤집어 덱 아래에 둡니다. 이것이 드로우 더미이며, 그 무늬가 으뜸패(트럼프)입니다.',
  trump:
    '뒤집힌 카드가 으뜸패 무늬를 정합니다. 으뜸패는 숫자와 상관없이 다른 무늬의 어떤 카드도 이깁니다.',
  first_attacker:
    '가장 낮은 으뜸패를 가진 사람이 먼저 시작합니다. {name}이(가) 첫 공격을 합니다.',
  first_attacker_you:
    '당신이 가장 낮은 으뜸패를 가졌으므로 첫 공격을 합니다.',
  attack:
    '공격은 시계 방향 다음 사람, 즉 수비자({name})에게 카드를 내미는 것입니다. 수비자는 답해야 합니다.',
  cover:
    '방어하려면 같은 무늬의 더 높은 카드로 공격 카드를 이깁니다.',
  trump_cover:
    '으뜸패는 다른 무늬의 어떤 카드든 방어합니다 — 낮은 으뜸패라도 다른 무늬의 높은 카드를 이깁니다.',
  stack_rule:
    '각 더미는 두 장으로 유지합니다: 아래에 공격, 위에 방어. 방어된 짝은 끝난 것입니다.',
  throw_in:
    '공격자는 누구나 이미 테이블에 있는 숫자의 카드를 추가로 던질 수 있습니다. 카드가 순식간에 불어나죠 — 그게 재미입니다.',
  capacity:
    '하지만 방어되지 않은 카드 수가 수비자의 남은 손 카드 수를 넘을 수는 없습니다.',
  pass:
    '방어 대신, 수비자는 같은 숫자의 카드를 더해 공격 전체를 다음 사람에게 넘길 수 있습니다.',
  pickup:
    '수비자가 답할 수 없거나 원치 않으면, 테이블의 모든 카드를 가져갑니다.',
  pickup_skip:
    '{name}이(가) 가져갔으므로 차례를 건너뛰고 다음 사람이 공격합니다.',
  good:
    '모두 방어되었고 아무도 더 던지지 않으니, 공격자들이 “방어 완료”라고 말합니다.',
  discard:
    '방어된 카드는 뒷면으로 버림 더미에 갑니다. 수비자는 막아냈고 이제 다음 판을 시작합니다.',
  draw:
    '판 사이에 모두 덱에서 여섯 장으로 다시 채웁니다 — 공격자가 먼저, 수비자가 마지막입니다.',
  deck_empty:
    '드로우 더미가 비었습니다. 이제 마지막 카드를 낸 사람이 안전하게 게임에서 빠집니다.',
  out: '{name}이(가) 마지막 카드를 냈습니다 — 안전하게 탈출!',
  fool:
    '{name}이(가) 카드를 들고 남았습니다 — 바보! 이게 게임 전부입니다. 실제로 해볼까요?',
  done: '모든 동작을 익혔습니다. 잘했어요!',
};

export const tutorialStrings: Record<Language, Dict> = { en, ru, ko };

/** tiny {placeholder} substitution */
export function tfmt(s: string, params?: Record<string, string>): string {
  if (!params) return s;
  return Object.entries(params).reduce(
    (acc, [k, v]) => acc.replace(new RegExp(`\\{${k}\\}`, 'g'), v),
    s,
  );
}
