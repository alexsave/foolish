// Lobby and "game meta" edits (docs/C_GAME_SHAPE_MIGRATION.md 2.4): the `meta`
// endpoint's body.type, dispatched to one C Table operation each.
//
// The JSON request body stays (it is HTTP input, and shipped iOS builds send
// it). Every edit's rule - who may do it (Q9), whether the table is in its
// lobby, a full table, a duplicate join, a title, a permutation, whether it
// deals - is the kernel's. What is left here is I/O: the `bots` read add-bot
// needs, the crypto deal seed an edit that may deal is handed (table_io), and
// the CAS loop.

import * as L from '@sdk/ts/gen/game_layout.bots.ts';
import { runTableOp, type TableOutcome } from './table_io.ts';
import { supabaseClient, type HandlerResult, type RequestContext } from './utils.ts';

interface BotRow { id: string; nickname: string; strategy_key: string }

/** add-bot only: the bots read, which does not depend on the game, so it can start before the game loads. */
export function prefetchBots(body: { type?: string }): PromiseLike<{ data: BotRow[] | null; error: unknown }> | undefined {
    return body?.type === 'add-bot'
        ? supabaseClient.from('bots').select('id, nickname, strategy_key') as unknown as PromiseLike<{ data: BotRow[] | null; error: unknown }>
        : undefined;
}

const str = (v: unknown): string => (typeof v === 'string' ? v : '');

/** One meta request. Shared by the `meta` edge function and tests. */
export async function handleMetaAction(ctx: RequestContext, botsPrefetch = prefetchBots(ctx.body)): Promise<HandlerResult> {
    const { user, userName, body, reqId } = ctx;
    const gameId = str(body?.game_id);
    if (!gameId) throw new Error('game_id is required');
    const actor = user.id;

    const op = (run: Parameters<typeof runTableOp>[0]['run'], noCommit?: (rc: number) => boolean): Promise<TableOutcome> =>
        runTableOp({ gameId, reqId, viewerId: actor, run, noCommit });
    const moot = (rc: number) => rc === L.TABLE_MOOT;

    let out: TableOutcome;
    switch (body?.type) {
        case 'start':
            out = await op(({ table, dealSeed }) => table.ready(actor, dealSeed), moot);
            break;
        case 'join':
            out = await op(({ table }) => table.join(actor, userName ?? ''), moot);
            break;
        case 'exit': {
            // An id that is present must be an id: a malformed one is a malformed
            // request, never "no target given" (which would make the caller leave).
            const given = (v: unknown) => v !== undefined && v !== null;
            const malformed = (given(body.bot_id) && typeof body.bot_id !== 'string')
                || (given(body.player_id) && typeof body.player_id !== 'string');
            const botId = str(body.bot_id);
            const target = str(body.player_id) || actor;
            out = await op(({ table }) => (malformed ? L.TABLE_E_WIRE
                : botId ? table.removeBot(actor, botId) : table.leave(actor, target)), moot);
            break;
        }
        case 'add-bot': {
            const { data: bots, error } = await (botsPrefetch ?? prefetchBots({ type: 'add-bot' })!);
            if (error || !bots) throw new Error('Failed to fetch bots');
            const wanted = str(body.bot_id);
            out = await op(({ table, dealSeed }) => {
                // Which bot row to seat is the request's choice among the rows
                // (a named one, or any not already seated); whether it may sit
                // is the kernel's.
                const seated = new Set(table.seats().map((s) => s.id));
                const pool = wanted ? bots.filter((b) => b.id === wanted) : bots.filter((b) => !seated.has(b.id));
                const bot = pool[Math.floor(Math.random() * pool.length)];
                if (!bot) throw new Error(wanted ? `Bot ${wanted} is not available to add to this game` : 'No available bots to add to the game');
                return table.addBot(actor, bot.id, bot.nickname, bot.strategy_key, dealSeed);
            }, moot);
            break;
        }
        case 'continue':
            out = await op(({ table }) => table.continueGame(actor), moot);
            break;
        case 'rearrange-hand': {
            const indices = body.card_indices;
            out = await op(({ table }) => (Array.isArray(indices)
                ? table.rearrangeHand(actor, indices as number[])
                : L.TABLE_E_WIRE), moot);
            break;
        }
        case 'rearrange-players': {
            const order = body.new_order;
            out = await op(({ table }) => (Array.isArray(order) && order.every((id: unknown) => typeof id === 'string')
                ? table.reseat(actor, order as string[])
                : L.TABLE_E_WIRE), moot);
            break;
        }
        case 'update-name':
            out = await op(({ table }) => table.retitle(actor, str(body.new_name)), moot);
            break;
        default:
            throw new Error(`unknown meta action type: ${body?.type}`);
    }
    return { body: out.envelope, runBots: out.committed && out.needsBots ? gameId : null };
}
