// Lobby and "game meta" requests (docs/C_GAME_SHAPE_MIGRATION.md 2.4): the `meta`
// endpoint's body is an ask (c/src/ask.h), read and run by the kernel.
//
// Nothing here knows what an ask says. Its layout is ask_decode's, which edit a
// kind means is table_ask's, and every edit's rule - who may do it (Q9),
// whether the table is in its lobby, a full table, a duplicate join, a title, a
// permutation, the rules, whether it deals - is the edit's own. What is left
// here is I/O: the `bots` read add-bot needs, the crypto deal seed an edit that
// may deal is handed (table_io), and the CAS loop.

import * as L from '@sdk/ts/gen/game_layout.bots.ts';
import { serverTable, type TableAsk } from '@sdk/ts/table/server_table.ts';
import { runTableOp } from './table_io.ts';
import { refused, supabaseClient, type HandlerResult, type RequestContext } from './utils.ts';

interface BotRow { id: string; nickname: string; strategy_key: string }
type BotsRead = PromiseLike<{ data: BotRow[] | null; error: unknown }>;

/** The bots read add-bot needs. */
const readBots = (): BotsRead =>
    supabaseClient.from('bots').select('id, nickname, strategy_key') as unknown as BotsRead;

/** What a request body asks, as the kernel read it. Throws for a body that is not an ask. */
export async function readAsk(body: Uint8Array): Promise<TableAsk> {
    const ask = (await serverTable()).askDecode(body);
    if (typeof ask === 'number') throw refused(L.ASK_WHY_MALFORMED, 'the body is not an ask');
    return ask;
}

/** One meta request. Shared by the `meta` edge function and tests. */
export async function handleMetaAction(ctx: RequestContext): Promise<HandlerResult> {
    const { user, userName, body, reqId } = ctx;
    const ask = await readAsk(body);
    const gameId = ask.gid;
    const actor = user.id;
    const addBot = ask.kind === L.ASK_ADD_BOT;

    let bots: BotRow[] = [];
    if (addBot) {
        const { data, error } = await readBots();
        if (error || !data) throw new Error('Failed to fetch bots');
        bots = data;
    }

    const out = await runTableOp({
        gameId, reqId, viewerId: actor,
        noCommit: (rc) => rc === L.TABLE_MOOT,
        run: ({ table, dealSeed }) => {
            if (!addBot) return table.ask(body, actor, userName ?? '', dealSeed);
            // Which bot row to seat is the request's choice among the rows (the
            // one it names, or any not already seated); whether it may sit is
            // the kernel's.
            const seated = new Set(table.seats().map((s) => s.id));
            const pool = ask.bot ? bots.filter((b) => b.id === ask.bot) : bots.filter((b) => !seated.has(b.id));
            const bot = pool[Math.floor(Math.random() * pool.length)];
            if (!bot) {
                throw refused(L.ASK_WHY_NO_BOT, ask.bot
                    ? `Bot ${ask.bot} is not available to add to this game`
                    : 'No available bots to add to the game');
            }
            return table.ask(body, actor, userName ?? '', dealSeed, { id: bot.id, nickname: bot.nickname, brain: bot.strategy_key });
        },
    });
    // A bump commits nothing and wakes the bots whenever the table has bot work;
    // an edit wakes them when it committed a table that has (the deal).
    const wake = ask.kind === L.ASK_BUMP ? out.needsBots : out.committed && out.needsBots;
    return { body: out.envelope, runBots: wake ? gameId : null };
}
