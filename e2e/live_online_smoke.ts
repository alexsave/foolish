// live_online_smoke.ts — drives a COMPLETE online game against a running local
// Supabase stack (`supabase start`) over the real edge functions, exactly as the
// iOS app would: signup → create → add-bot → start → play. Proves the online
// backend (auth + `create`/`meta`/`action` functions + bot loop + persistence)
// actually plays a game to a winner locally.
//
// The human seat's moves are chosen with the SAME enumeration the bots use
// (calculateLegalMoves) — move quality is irrelevant, we only need legal moves
// to keep the game advancing. Bot seats are driven by the server's own bot loop
// (woken by run_bots after each human move, plus explicit `bump`s).
//
// Run:  deno run -A e2e/live_online_smoke.ts
// Needs the local stack up; reads full game state via `docker exec ... psql`
// (the games.state column is REVOKEd from client roles, so we read it as the
// superuser rather than over PostgREST).

import { deserializeGameState } from '../supabase/functions/_shared/wasm/engine.ts';
import { hexToBytes } from '../supabase/functions/_shared/replay/codec.ts';
import { STRAT, wasmChooseMoveDirect } from '../supabase/functions/_shared/wasm/bots.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/bot_strategy.ts';

const moveKey = (m: any) => JSON.stringify({ t: m.type, c: m.cards ?? null, a: m.attack_cards ?? null });

const BASE = 'http://127.0.0.1:54321';
const ANON = Deno.env.get('LOCAL_ANON') ??
  'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZS1kZW1vIiwicm9sZSI6ImFub24iLCJleHAiOjE5ODM4MTI5OTZ9.CRXP1A7WOeoJeXxjNni43kdQwgnWNReilDMblYTn_I0';
const DB_CONTAINER = 'supabase_db_foolish';

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function api(path: string, token: string, body?: unknown): Promise<Response> {
  return await fetch(`${BASE}${path}`, {
    method: 'POST',
    headers: {
      apikey: ANON,
      Authorization: `Bearer ${token}`,
      'Content-Type': 'application/json',
    },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
}

// Run a single-value SQL query as the superuser and JSON-parse the scalar result.
async function psql(sql: string): Promise<unknown> {
  const cmd = new Deno.Command('docker', {
    args: ['exec', DB_CONTAINER, 'psql', '-U', 'postgres', '-d', 'postgres', '-tAc', sql],
    stdout: 'piped', stderr: 'piped',
  });
  const { stdout, stderr } = await cmd.output();
  const out = new TextDecoder().decode(stdout).trim();
  if (!out) throw new Error(`psql returned nothing: ${new TextDecoder().decode(stderr).trim()}`);
  return JSON.parse(out);
}

// Read a column set for a game straight from Postgres (superuser).
async function dbGame(gameId: string): Promise<{ status: string; state: string | null; players: any[]; elimination_order: any[]; good_players: any[]; good_timestamp: number | null }> {
  return await psql(
    `select json_build_object('status',status,'state',state,'players',players,'elimination_order',elimination_order,'good_players',good_players,'good_timestamp',good_timestamp) from games where id='${gameId}';`,
  ) as any;
}

async function main() {
  const tag = String(Date.now()).slice(-6);
  const email = `smoke_${tag}@foolish.local`;
  const username = `SMOKE${tag}`;

  // 1. signup (immediate session; local has email confirmations off)
  const su = await (await fetch(`${BASE}/auth/v1/signup`, {
    method: 'POST',
    headers: { apikey: ANON, 'Content-Type': 'application/json' },
    body: JSON.stringify({ email, password: 'password123', data: { username } }),
  })).json();
  const token: string = su.access_token;
  const userId: string = su.user?.id ?? su.id;
  if (!token || !userId) throw new Error(`signup failed: ${JSON.stringify(su)}`);
  console.log(`✓ signed up ${username} (${userId.slice(0, 8)})`);

  // 2. create a game
  const createRes = await api('/functions/v1/create', token);
  if (!createRes.ok) throw new Error(`create failed: ${createRes.status} ${await createRes.text()}`);
  await createRes.arrayBuffer(); // packed response; game id comes from the DB
  // create persists the row in the background AFTER responding, so poll for it.
  let gameId = '';
  for (let i = 0; i < 40 && !gameId; i++) {
    const row = await psql(
      `select coalesce((select json_build_object('id',id) from games where players @> '[{"player_id":"${userId}"}]'::jsonb order by created_at desc limit 1),'null'::json);`,
    ) as { id: string } | null;
    if (row?.id) { gameId = row.id; break; }
    await sleep(100);
  }
  if (!gameId) throw new Error('could not resolve created game id');
  console.log(`✓ created game ${gameId}`);

  // 3. pick a (non-GPT) bot and add it. The bots table is service-side only
  // (no PostgREST grant), so read it as the superuser.
  const bot = (await psql(
    `select json_build_object('id',id,'nickname',nickname,'strategy_key',strategy_key) from bots where strategy_key <> 'gpt' order by id limit 1;`,
  )) as { id: string; nickname: string; strategy_key: string };
  const addRes = await api('/functions/v1/meta', token, { type: 'add-bot', game_id: gameId, bot_id: bot.id });
  if (!addRes.ok) throw new Error(`add-bot failed: ${addRes.status} ${await addRes.text()}`);
  console.log(`✓ added bot ${bot.nickname} (${bot.strategy_key})`);

  // 4. start (marks me READY; all ready + >=2 players => deal)
  const startRes = await api('/functions/v1/meta', token, { type: 'start', game_id: gameId });
  if (!startRes.ok) throw new Error(`start failed: ${startRes.status} ${await startRes.text()}`);
  await startRes.arrayBuffer();
  console.log(`✓ started`);

  // 5. play loop
  let moves = 0, myMoves = 0, bumps = 0;
  for (let i = 0; i < 2000; i++) {
    const g = await dbGame(gameId);
    if (g.status === 'game_over') {
      console.log(`\n🏁 game_over after ${moves} server-applied moves (${myMoves} human, ${bumps} bumps)`);
      console.log(`   elimination order (fool = last standing): ${JSON.stringify(g.elimination_order)}`);
      return;
    }
    if (g.status !== 'playing' || !g.state) { await sleep(150); continue; }

    const game = deserializeGameState(hexToBytes(g.state), {
      id: gameId, name: '', version: 0, deck_length: 0,
      players: (g.players ?? []).map((p: any) => ({
        player_id: p.player_id, name: p.name, is_ai: p.is_ai, strategy_key: p.strategy_key ?? 'human',
      })),
      good_players: g.good_players ?? [], good_timestamp: g.good_timestamp ?? null,
    } as any);

    // Gate on my ACTUAL legal moves: if I have none it's the bot's turn, so
    // nudge the server bot loop instead of spamming an illegal move.
    const legal = calculateLegalMoves(game, userId).filter((x: any) => x.type !== 'wait');
    if (legal.length > 0) {
      // Prefer the handwritten strategy's pick when it's actually legal (fast,
      // sensible → rounds close and the game terminates); else any legal move.
      const pick: any = wasmChooseMoveDirect(game, userId, STRAT.handwritten, { logs: false });
      const legalKeys = new Set(legal.map(moveKey));
      const m: any = (pick && pick.type !== 'wait' && legalKeys.has(moveKey(pick))) ? pick : legal[0];
      let body: any;
      if (m.type === 'attack') body = { type: 'attack', game_id: gameId, cards: m.cards };
      else if (m.type === 'cover') body = { type: 'cover', game_id: gameId, cover_cards: m.cards, attack_cards: m.attack_cards };
      else if (m.type === 'pass') body = { type: 'pass', game_id: gameId, cards: m.cards };
      else if (m.type === 'pickup') body = { type: 'pickup', game_id: gameId };
      else if (m.type === 'good') body = { type: 'good', game_id: gameId };
      else { await sleep(120); continue; }

      const res = await api('/functions/v1/action', token, body);
      const okTxt = res.ok ? 'ok' : `HTTP ${res.status}`;
      moves++; myMoves++;
      if (myMoves <= 40 || myMoves % 10 === 0) console.log(`  [${myMoves}] human ${m.type} → ${okTxt}`);
      await res.arrayBuffer();
      await sleep(60); // let the woken bot loop take its turn
    } else {
      // Not my turn — nudge the server bot loop and wait.
      await api('/functions/v1/action', token, { type: 'bump', game_id: gameId });
      bumps++;
      await sleep(180);
    }
  }
  throw new Error('game did not finish within 2000 iterations');
}

main().catch((e) => { console.error('✗', e.message ?? e); Deno.exit(1); });
