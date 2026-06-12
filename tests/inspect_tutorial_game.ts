import { codeToGame } from "../src/replay/codec";
import { decodeReplay } from "../src/replay/decode";
import { buildReplaySteps, stepToGame } from "../src/replay/view";
import { LOG_TYPE } from "../src/common/types";
import { VALUE_MAP, SUIT_MAP } from "../src/common/constants";
import { readFileSync } from "node:fs";

const code =
  process.argv[2] ||
  JSON.parse(readFileSync("/tmp/tutorial_best.json", "utf8")).code;
const names = ["You", "Vera", "Boris"];
const cs = (c: any) =>
  c && c.suit >= 0 ? `${VALUE_MAP[c.value]}${SUIT_MAP[c.suit][0]}` : "?";

const d = decodeReplay(codeToGame(code));
const steps = buildReplaySteps(d as any);
console.log(
  `n=${d.playerCount} powerSuit=${SUIT_MAP[d.powerSuit]} firstAttacker=${names[d.firstAttacker]} fool=${names[d.fool]} trump=${cs(d.trumpCard)} steps=${steps.length}`,
);

let nullSelf = 0;
steps.forEach((s, i) => {
  const g = stepToGame(d as any, s, "t", names);
  const h0 = g.replay_hands[0];
  const nNull = h0.filter((c) => c === null).length;
  if (nNull > 0) nullSelf++;
  const who = s.seat !== null ? names[s.seat] : "—";
  const cards = s.cards.map(cs).join(",");
  const tgt = s.target ? "->" + cs(s.target) : "";
  const tag =
    s.seat === 0 &&
    [LOG_TYPE.ATTACK, LOG_TYPE.COVER, LOG_TYPE.PASS, LOG_TYPE.PICKUP, LOG_TYPE.GOOD].includes(
      s.kind as any,
    )
      ? "  <== YOUR MOVE"
      : "";
  console.log(
    `${String(i).padStart(2)} ${String(s.kind).padEnd(16)} ${who.padEnd(6)} ${cards}${tgt} [deck=${s.deckCount} disc=${s.discard} you=${h0.filter(Boolean).map(cs).join(" ")}]${tag}`,
  );
});
console.log(`steps where seat-0 hand had unknown (null) cards: ${nullSelf}`);
