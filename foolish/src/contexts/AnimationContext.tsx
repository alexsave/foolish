import React, { createContext, useContext, useEffect, useRef, useCallback } from 'react';
import { useServer, useServerActions } from './ServerContext';
import { useAuth } from './AuthContext';
import { useParams } from 'next/navigation';
import supabase from '../common/Connector';
import { validateActionWire } from '../utils/gameValidation';
import { clientTable, encodeAction, type AwireMove } from '@sdk/ts/table/client_table.ts';
import { pushToSequence } from '../state/pushSequence';
import { rulesOf, tableCards, type TableView, type ViewCard } from '../state/view';
import { optimisticBoard, turnedBoard, withdrawn } from '../state/clientBoards';
import { base64ToBytes } from '@sdk/ts/wire/bytes.ts';
import * as V from '@sdk/ts/gen/view_layout.bots.ts';
import { getCardKeyOwner } from '../utils/animationUtils';
import { animationFeed } from '../state/animationFeed';
import type { ClientAnimationEvent } from '../state/animationStep';
import {
    forgetRefused, passedBoard, pendingOverlayCards, rememberPending, seatOf, settlePush, sweepStaleMotions,
    type OptimisticState,
} from '../state/optimisticResolve';
import { optimisticOverlay } from '../state/optimisticOverlay';
import { authoritativeVersion, noteAuthoritativeVersion } from '../state/authoritativeVersion';
import { useAnimationRun } from '../state/useAnimationRun';
import type { ArrivingPile } from '../state/animPlan';
import { ROLE_CLAIM, useRoleMotion, type RoleHandOff } from '../state/useRoleMotion';
import { rolesOf, type ShownBoard } from '../state/roleLedger';
import { animRolesGoodsCleared, animRolesGoodsOpening, animRolesPassHandOff, animShouldDropStale } from '@sdk/ts/wasm/bots.ts';
import { ANIM_NO_MASK } from '@sdk/ts/gen/anim.bots.ts';
import { cueAudio } from '../audio/cueAudio';

type Card = ViewCard;

// Every board this pipeline commits is a board it was given - a push's step, a
// replay frame - or one the kernel made from one (src/state/clientBoards.ts): my
// pending cards kept on a push's boards, the lead and shield of my pending pass,
// the board a revert flies home to. Nothing here edits a board.

// The places a flight's card is drawn at while it flies, by the owner key the
// page's CardFace names them with (src/components/GameDisplay/CardFace.tsx): a
// seat's hand, the table, the flipped slot. The card is hidden at the place it
// leaves AND the place it lands on - a confirmation or a board committed before
// the flight lands shows it there already - so the flight is the one card the
// page draws. The flipped trump a draw deals out is NOT hidden here: the stock
// lifts it out of its slot itself, on the kernel's word that it is in the air
// (AnimFrame.trump_flight -> ViewRules.show_flipped_card), for every viewer -
// matching it against the flight's cards could only ever hide it from the one
// viewer whose draw names it.
const flightPlaces = (from: string | undefined, to: string | undefined, seat: number | undefined): (number | string)[] => {
    const places: (number | string)[] = [];
    for (const loc of [from, to]) {
        if (loc === 'hand' && seat !== undefined) places.push(seat);
        else if (loc === 'table' || loc === 'flipped') places.push(loc);
    }
    return places.filter((p, i) => places.indexOf(p) === i);
};

interface AnimationContextType {
    isAnimating: boolean;
    currentAnimation: ClientAnimationEvent | null;
    /** How long the flight on screen lasts, from the kernel's plan: the number
     *  the overlay's CSS transition is written with. 0 when nothing is flying. */
    flightMs: number;
    /** How long the BATTLE ROW's own layout change lasts - the plan's duration
     *  for the step whose card the row is making room for. Not `flightMs` at the
     *  moment it is read: the row also moves at a landing, which falls in the gap
     *  between two flights where that one is already 0. 0 before a run's first
     *  step opens and after a seek, which are both meant to be instant. */
    rowMs: number;
    /** THE PILES THE GRID MUST NOT MAKE ROOM FOR YET - the cards of the steps
     *  this run has not started, which get no cell until their own flight opens
     *  (src/state/animPlan.ts heldPiles, drawn through `shownRow`). Empty on
     *  every frame with nothing in the air, and the same object each time, so
     *  it compares by identity. */
    heldPiles: ReadonlySet<number>;
    /** …AND THE PILES IT MUST MAKE ROOM FOR NOW - the cards a step that HAS
     *  started is bringing down, which take their cell while they are still in
     *  the air so the row grows around them as they come (src/state/animPlan.ts
     *  arrivingPiles, drawn through the same `shownRow`). This is the table's
     *  half of iMessage's round 7, "it should be at the same TIME". */
    arrivingPiles: readonly ArrivingPile[];
    // Cards currently flying from the deck pile. Drives the visible pile size.
    // Drops BEFORE the animation starts and resets when the snapshot commits.
    inFlightFromDeck: number;
    // Subset of inFlightFromDeck that's headed to the flipped slot - these
    // are still "in the deck system" so they count toward the badge total.
    inFlightToFlipped: number;
    /** The flipped trump in the air on its way to a hand (AnimFrame.trump_flight):
     *  it has left its slot, which the stock draws empty. Null otherwise. */
    trumpFlight: Card | null;
    // `owner`: the seat whose cards these are, or a place's own key ('table', 'flipped').
    getCardAnimationState: (card: Card, owner?: number | string) => {
        isAnimating: boolean;
        animationType: string | null;
        progress: number;
        fromLocation: string | null;
        toLocation: string | null;
    };
    // Game action methods that handle optimistic animations + server calls
    attack: (cards: Card[]) => Promise<{ game_id: string }>;
    pass: (cards: Card[]) => Promise<{ game_id: string }>;
    pickup: () => Promise<{ game_id: string }>;
    cover: (coverCards: Card[], attackCards: Card[]) => Promise<{ game_id: string }>;
    good: () => Promise<{ game_id: string }>;
    /** Drop everything queued or in flight, without committing pending
     *  states. Used by the replay player when seeking; a live game never
     *  needs it (the server stream is the only truth there). */
    resetAnimations: () => void;
    // ---- the role marks (src/state/useRoleMotion.ts) -------------------------
    /** WHAT THE BADGES ARE WEARING - the frozen ledger, not the live board. A
     *  sequence walks it forward beat by beat, so a bout end does not re-cast
     *  every sword a beat before the sequence that earns it has played. Null
     *  until a board has shown anything. */
    shownRoles: ShownBoard | null;
    /** The marks in the air, and the seats they left and are going to. */
    roleHandOff: RoleHandOff;
    /** A seat publishes the box its mark is drawn in, so a mark can fly to it. */
    publishRolePad: (seat: number, el: HTMLElement | null) => void;
    /** The flights layer's two reports: a ghost drew a frame, and every ghost
     *  has landed. The ghosts end a hand-off, because they run on the frames
     *  that draw them and a wall-clock timer does not. */
    noteRoleFlightFrame: () => void;
    landRoleHandOff: () => void;
}

// Exported so the tutorial can re-provide a value that overrides the action
// methods (attack/pass/pickup/cover/good) to drive its scripted playthrough,
// while still spreading the real animation state (isAnimating, etc.).
export const AnimationContext = createContext<AnimationContextType | null>(null);

// Check if any bot can possibly move on the current board. Turn eligibility
// lives in ONE place - the kernel's should_bot_act rule, asked of the board
// (ViewRules.bot_to_move; a hand-rolled copy here used to count a said-good
// attacker as movable while uncovered attacks remained, keeping the poll-bump
// timer firing for nobody).
const canBotMove = (view: TableView | undefined): boolean =>
    !!view && view.seats.length > 0 && rulesOf(view).botToMove;

export const AnimationProvider = ({ children }: { children: React.ReactNode }) => {
    // Actions come from the stable actions context (identity never changes);
    // only the state this provider genuinely needs comes from the state context.
    const serverActions = useServerActions();
    const { updateGameState } = serverActions;
    const { views: games, game_id } = useServer();
    const { user_id } = useAuth();
    const url_game_id = useParams<{ game_id: string }>().game_id?.toLowerCase();

    // The animation state the page renders from comes from the frame loop
    // (useAnimationRun, below): the kernel holds the timing, so nothing about a
    // run is state this file declares.
    const pendingCompletionCallbackRef = useRef<(() => void) | null>(null);
    const remainingSequenceEventsRef = useRef<number>(0);

    // Track if there have been bot moves in the last interval
    const hasBotMovedRef = useRef<boolean>(false);

    // Ref to track current game state (avoids stale closure in interval)
    const currentGameRef = useRef<typeof games[string] | undefined>(undefined);
    // All loaded games, ref-mirrored for the feed subscription callback (the
    // packed-envelope decode needs the roster of the game the message names,
    // not the possibly-stale `games` closure the effect captured).
    const gamesRef = useRef(games);

    // Store the current game ID for this animation sequence
    const currentGameIdRef = useRef<string | null>(null);

    // MY MOVES THE SERVER HAS NOT ANSWERED: the cards I have predicted, and my
    // pending pass. src/state/optimisticResolve.ts owns the shape and every
    // routine that settles it; this file only holds it and hands it over.
    const optimistic = useRef<OptimisticState>({ motions: [], pass: null }).current;

    // THE FIRST TAP ANYWHERE UNDER THIS PROVIDER LETS THE GAME BE HEARD. A
    // browser will not start audio nobody asked for, so the engine waits for a
    // gesture (src/audio/cueAudio.ts); arming it here rather than on the board
    // means a tap in the lobby or on the dashboard has already paid that toll
    // by the time the first card is dealt.
    useEffect(() => { cueAudio.arm(); }, []);

    // Expose the local player's live optimistic table cards to the REST load path,
    // so a reconnect resync re-applies them instead of momentarily wiping them
    // (the "vanish then reappear" glitch).
    useEffect(() => optimisticOverlay.register(() => pendingOverlayCards(optimistic)), []);

    // THE ONE DEDUP AND ORDERING RULE of the live feed is the kernel's version
    // gate (c/src/anim_plan.h anim_should_drop_stale), asked in
    // handleAnimationMessage against the newest version applied for the push's
    // own game (src/state/authoritativeVersion.ts). A cast is one committed
    // version, so a retry, the same version on a second channel and a late
    // out-of-order push are all "at or below what I hold". An authoritative REST
    // load (first load, post-reconnect resync) raises the same number, so a
    // push that was in flight before the load is dropped too. A replay's or the
    // tutorial's sequence carries no version and is never gated.
    useEffect(() => {
        noteAuthoritativeVersion(url_game_id, url_game_id ? games[url_game_id]?.version : undefined);
    }, [url_game_id, games]);

    // Keep currentGameRef in sync with latest game state
    useEffect(() => {
        currentGameRef.current = url_game_id ? games[url_game_id] : undefined;
        gamesRef.current = games;
    }, [url_game_id, games]);


    // Start bot bump timer when component mounts and game is loaded
    useEffect(() => {
        if (!url_game_id) {
            return;
        }

        // Check if there are any AI players in the game
        const currentGame = games[url_game_id];
        const hasAIPlayers = currentGame?.seats.some(seat => seat.isAi) || false;

        // Only start timer if there are AI players
        if (!hasAIPlayers) {
            return;
        }

        // Start interval that checks every 5 seconds
        const intervalId = setInterval(() => {
            // Check if there have been bot moves in the last interval
            if (!hasBotMovedRef.current) {
                // Check if any bot can actually move in current state (use ref for fresh data)
                const botCanMove = canBotMove(currentGameRef.current);
                if (!botCanMove) {
                    // No bot can move - we're waiting for a human player, skip bump
                    return;
                }

                // No bot moves but a bot could move - nudge the bot loop
                supabase.functions.invoke('meta', {
                    body: new Blob([clientTable().askBody({ kind: V.ASK_BUMP, gid: url_game_id }) as BlobPart])
                }).catch(error => {
                    console.error('Bot bump failed:', error);
                });
            }

            // Reset the flag for the next interval
            hasBotMovedRef.current = false;
        }, 5000); // 5 seconds

        return () => clearInterval(intervalId);
    // Re-run when game data loads or players change (bot might be added)
    // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [url_game_id, url_game_id ? games[url_game_id]?.seats.length : 0]);

    // Clear OLD optimistic animations every 5 seconds (older than 30 seconds)
    useEffect(() => {
        const interval = setInterval(() => sweepStaleMotions(optimistic, Date.now(), 30000), 5000);
        return () => clearInterval(interval);
    }, []);

    // Consume the animation feed. The transport lives elsewhere: live games
    // mount RealtimeAnimationFeed (src/state/) which republishes the supabase
    // broadcast channel into the bus; the replay screen publishes synthesized
    // sequences from a decoded replay integer. Either way the messages are
    // identical in shape, so this provider animates both.
    useEffect(() => {
        return animationFeed.subscribe((message) => {
            handleAnimationMessage(message);
        });
        // handleAnimationMessage closes over stable refs + setState updaters;
        // resubscribing on these deps mirrors the old channel effect.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [user_id, url_game_id]);

    // Packed envelopes we can't decode (unknown game, corrupt bytes, roster
    // desync): refetch the authoritative state once instead of dropping
    // silently forever. The refetch re-checks the landed version against the
    // broadcast's - a load that was already in flight when the broadcast
    // committed can return an OLDER state (its read predates the commit), so
    // one more load is chained in that case.
    const packedRefetchInFlight = useRef<Set<string>>(new Set());
    const refetchForEnvelope = (gid: string | undefined, minVersion: number | undefined) => {
        if (!gid || packedRefetchInFlight.current.has(gid)) return;
        packedRefetchInFlight.current.add(gid);
        serverActions.loadGame(gid)
            .then(() => {
                const landed = gamesRef.current[gid];
                if (landed && minVersion !== undefined && (landed.version ?? 0) < minVersion) {
                    return serverActions.loadGame(gid);
                }
            })
            .catch(() => { /* resubscribe resync covers persistent failures */ })
            .finally(() => packedRefetchInFlight.current.delete(gid));
    };

    // A realtime cast { b, game_id } -> the sequence shape (events + final game)
    // handleAnimationMessage consumes. `b` is the kernel's cast (evwire.h): the
    // committed version and the as3 push, read whole by the client slot
    // (client_table.h client_cast_open), so nothing here is told the format or
    // the version beside the bytes.
    //
    // Who sits where: a push that changed the roster carries it. Otherwise the
    // table's identity the kernel kept from the last envelope or roster push it
    // read for this game names the seats.
    const decodeCast = (m: any): any | null => {
        const gid = typeof m.game_id === 'string' ? m.game_id : url_game_id;
        const table = clientTable();
        let bytes: Uint8Array | null = null;
        let read = null;
        try {
            bytes = base64ToBytes(m.b);
            read = table.readCast(bytes, { gameId: gid });
        } catch (e) {
            console.error('cast decode failed:', e);
        }
        // Unreadable bytes, or a push the kernel could not name the seats of
        // (no identity for this game yet, or one whose seats the push's boards
        // do not have - the roster changed while this client was away): fetch
        // the authoritative state and drop this sequence. The load carries the
        // fresh roster, and is asked for at least the version the cast names.
        if (!read || read.final.seats.length === 0 || read.final.seats[0].id === '') {
            refetchForEnvelope(gid, (bytes && table.castVersion(bytes)) ?? undefined);
            return null;
        }
        const decoded = pushToSequence(read);
        return { version: read.final.version, events: decoded.events, game: decoded.game };
    };

    // Handle animation messages from real-time channel
    const handleAnimationMessage = (message: any) => {
        // A live message is a cast: `b` and nothing else (animationFeed.ts
        // CastMessage). A replay or the tutorial publishes the sequence itself.
        if (message && typeof message.b === 'string') {
            message = decodeCast(message);
            if (!message) return;
        }
        if (!message.events || !Array.isArray(message.events)) {
            return;
        }

        // The version gate (see the effect above). Replay-synthesized sequences
        // have no version and are never gated.
        if (typeof message.version === 'number') {
            const gid = message.game.gameId;
            if (animShouldDropStale(authoritativeVersion(gid) ?? null, message.version)) return;
            // Also what the next tap is stamped with (ServerContext's round guard).
            noteAuthoritativeVersion(gid, message.version);

        }

        // A push for a game this page is not showing is a board to keep, not a
        // sequence to play: its cards have no place on this table. ServerContext
        // leaves a game's streams when the page does, but leaving is not
        // instant - a push already on the socket still arrives - so the stream's
        // owner and this test cover different moments, and both stay.
        if (typeof message.version === 'number' && message.game.gameId !== url_game_id) {
            updateGameState(message.game.gameId, message.game);
            return;
        }

        // Store the game ID for use during animations
        currentGameIdRef.current = message.game.gameId;

        // The push against my unanswered moves (src/state/optimisticResolve.ts):
        // what is left of `message.events` is what this client has not already
        // animated as a prediction, and the flights carry home what it dooms.
        const pushWasEmpty = message.events.length === 0;
        const revertEvents = settlePush(optimistic, message, typeof message.version === 'number');

        // If ALL events were optimistic, just update state and return
        if (message.events.length === 0) {
            // A GOOD CAN BE THE WHOLE STREAM, AND THIS IS THE WEB'S EMPTY-STREAM
            // GUARD - so the good has to be played ABOVE it, not below.
            //
            // There is no ANIM_EVT_GOOD: a good flies no card, so it emits no
            // event. The server used to drop its push for exactly that reason
            // (`nEvents > 0`) and the mark only reached a screen folded into
            // whatever moved next; TableCommit.goods_changed sends it now, and
            // what arrives is a push whose `events` are EMPTY and whose trailer
            // board is the entire move. Everything below this guard - the
            // opening role beat included - is reached only by a stream with
            // steps in it, so a good that arrives alone would fall straight
            // through to `updateGameState` and be left to the bystander effect,
            // which `anim_shown_ledger_allows` refuses outright while another
            // sequence is still walking the marks forward.
            //
            // iMessage made this exact call first and says why, from the other
            // side: it seeds its roles "AHEAD OF THE EMPTY-STREAM GUARD, because
            // the stream that needs it most is the empty one: a `good` that does
            // not close the bout emits no step, so the difference between these
            // two role states is the ENTIRE move" (MessageTableView+Sequence.swift).
            //
            // ADDED goods only - `animRolesGoodsOpening` is the kernel's rule and
            // answers null for anything else, which is right: a good being taken
            // away is a consequence of the card that cleared it, and that card is
            // an event, so such a push is never one of these.
            if (pushWasEmpty && message.game) {
                const shown = roleMotionRef.current.read();
                const opening = shown ? animRolesGoodsOpening(shown.roles, message.game.goodMask) : null;
                // The seats do not change, only what they are wearing: the same
                // coin flip in place the opening beat of a real stream makes.
                if (opening) roleMotionRef.current.syncRoles(opening);
                // A sequence still in flight settles on the board in this ref at
                // its closing beat. This push's board is strictly newer than that
                // one (the version gate above guarantees it), so leaving the old
                // board there would have the closing beat take the check back off
                // the badge it was just put on.
                if (sequenceFinalRef.current) sequenceFinalRef.current = message.game;
            }
            if (message.game) {
                updateGameState(message.game.gameId, message.game);
            }
            return;
        }

        // Store the completion callback to update final game state
        pendingCompletionCallbackRef.current = () => {
            if (!message.game) {
                return;
            }

            // The board this sequence settles on, with my pass on it if the server
            // has not taken it yet. The kernel decides which of those a board is
            // (client_optimistic_apply): a final board whose table already shows
            // the pass's cards is the server's answer and comes back untouched,
            // and one that does not is a board my move is still in the air over,
            // so it gets the shield handed on. Three tests used to stand here -
            // is this message mine, does the server agree with my two cached
            // seats, and is it mine but disagreeing - to pick between trusting my
            // guess and trusting the server. There is no guess left to trust.
            message.game = passedBoard(optimistic, message.game) ?? message.game;

            updateGameState(message.game.gameId, message.game);
        };
        remainingSequenceEventsRef.current = message.events.length + revertEvents.length;

        // The board this stream SETTLES on, for the one rule that cannot be
        // answered from the stream: a pass writes no event for the hand-over.
        sequenceFinalRef.current = message.game ?? null;

        // A GOOD BEING SET LEADS THE STREAM. It is somebody's move, and the
        // transition, the discard and the deal behind it are its consequences -
        // flip it late and an attacker's check would snap on after the cards it
        // caused had already been swept. `firstGoodMask` is the stream's own
        // event-0 mask (the kernel's AnimBeats.first_good_mask), and the rule
        // that only ADDED goods lead is the kernel's too.
        //
        // A GOOD THAT ARRIVES ALONE never reaches this line: it has no `events[0]`
        // to take a mask from, and it is played at the empty-stream guard above,
        // where iMessage plays it too. `events[0]` is therefore still always
        // there by the time this runs.
        const openingShown = roleMotionRef.current.read();
        const opening = openingShown
            ? animRolesGoodsOpening(openingShown.roles, stepGoodMask(message.events[0]))
            : null;
        // The seats do not change here, only what they are wearing, so nothing
        // flies: this is the coin flip each badge makes where it stands.
        if (opening) roleMotionRef.current.syncRoles(opening);

        // Every return flight goes first, then the push as it came.
        enqueue([...revertEvents, ...message.events]);
    };


    // ---- THE ROLE MARKS, and the three moments they move at ------------------
    //
    // The ledger and the hand-off live in src/state/useRoleMotion.ts; WHICH
    // marks change at WHICH point of a sequence is the kernel's, and this file
    // is where each of its three answers is asked, because this is where the
    // sequence is. c/src/anim_plan.h states the rule the three firing points
    // below are:
    //
    //   a good being SET leads the stream - it is somebody's move, and the
    //     transition, the discard and the deal behind it are its consequences;
    //   a good being CLEARED runs parallel with the throw-in that cleared it,
    //     since the card and the marks are one event and neither leads;
    //   a PASS hands the shield over WITH the transfer card;
    //   everything else waits for the closing beat at the end of the sequence.
    //
    // iMessage fires them in exactly these four places
    // (MessageTableView+Sequence.swift: the top of `runEventStream`, beside each
    // beat's own flights, and the closing beat at the bottom).
    const roleMotion = useRoleMotion();
    const roleMotionRef = useRef(roleMotion);
    roleMotionRef.current = roleMotion;
    // The board a running sequence SETTLES on. A pass is snapshotted before the
    // hand-over and writes no event for it, so the new defender appears nowhere
    // in the stream - only on the final board, which is why
    // `anim_pass_hand_off` takes it as an argument.
    const sequenceFinalRef = useRef<TableView | null>(null);

    /** The good mask a step's own board carries, or ANIM_NO_MASK for a step that
     *  carried no board - the one value a mask may not take, and the value both
     *  goods rules answer 0 for. */
    const stepGoodMask = (step: ClientAnimationEvent): number =>
        step.game_state ? step.game_state.goodMask : ANIM_NO_MASK;

    // ONE FRAME LOOP, and the kernel answers it (src/state/useAnimationRun.ts).
    // What is left here is what a landing MEANS - which board it commits, what
    // tracking it releases, when a sequence's final board is the truth - because
    // that is about the game, and the loop is about time.
    const {
        isAnimating, currentAnimation, flightMs, rowMs, heldPiles, arrivingPiles,
        inFlightFromDeck, inFlightToFlipped, trumpFlight, animatingCards, enqueue, reset: resetRun,
    } = useAnimationRun<ClientAnimationEvent>({
        board: () => currentGameRef.current,
        placesOf: (step) => flightPlaces(step.from_location, step.to_location, step.seat),
        keyOf: (card, place) => getCardKeyOwner(card, place),
        // ONE MOVE, ONE MOVEMENT. The kernel spends one COVER event per card, so
        // a defender who covered two attacks in one move arrives as two steps
        // and the plan opens both at the same instant (AnimPlanStep.beat_n).
        // This is what the page draws for that instant: the steps' cards in one
        // flight, each still aimed at the pile the kernel named it for -
        // `target_cards` is the per-card form the overlay already reads for a
        // multi-card cover of my own, and it is the only form that survives the
        // merge, because `target_card` and `battle_index` describe ONE event.
        // The board is the LAST step's: the boards inside one move are boards
        // nobody was ever shown.
        //
        // RENDERING ONLY. Every step of the beat still has its own landing
        // taken, in order, so `commit_board`, `commit_if` and the sequence
        // countdown are untouched by this.
        mergeBeat: (steps) => {
            const cards = steps.flatMap((s) => s.cards ?? []);
            const targets = steps.flatMap((s) => (s.cards ?? []).map(() => s.target_card));
            const named = targets.filter((t): t is Card => t !== undefined);
            return {
                ...steps[0],
                cards,
                // All or nothing: a half-named list would slide every card after
                // the gap onto the wrong pile, where no list at all leaves the
                // overlay's own last-resort guess exactly as it was.
                target_cards: named.length === cards.length ? named : undefined,
                battle_index: undefined,
                game_state: steps[steps.length - 1].game_state ?? steps[0].game_state,
            };
        },
        onLanded: (step) => {
            // A PREDICTION'S BOARD IS MADE AT ITS LANDING, from whatever is on
            // screen then - a broadcast can commit fresher state inside the
            // flight, and a board derived at tap time would write the stale
            // table and hand back over it.
            const board = step.commit_board ? step.commit_board() : step.game_state;
            const commitGameId = currentGameIdRef.current ?? board?.gameId;
            if (board && commitGameId && (step.commit_if?.() ?? true)) {
                updateGameState(commitGameId, board);
            }
            if (pendingCompletionCallbackRef.current && remainingSequenceEventsRef.current > 0) {
                remainingSequenceEventsRef.current--;
            }
        },
        // A STEP'S CARDS ARE IN THE AIR AS OF THIS FRAME, which is when the marks
        // that move WITH a card move. Both rules are fired and not awaited: the
        // badge's turn and the card's flight are one event and start in the same
        // instant, the shorter of the two simply finishing first.
        onOpened: (step) => {
            const roles = roleMotionRef.current;
            const shown = roles.read();
            if (!shown) return;
            // THE GOODS THIS STEP CLEARS TURN NOW, WITH ITS CARD. Gated on the
            // step actually PUTTING A CARD DOWN (the kernel's ANIM_BEAT_PLACED
            // read off the step's own destination): a throw-in is the only thing
            // that clears a good this way, and a mask that changes for any other
            // reason still belongs to the closing beat with the rest of the
            // consequences.
            if (step.to_location === 'table') {
                const cleared = animRolesGoodsCleared(shown.roles, stepGoodMask(step));
                if (cleared) roles.syncRoles(cleared, { tableOpen: true });
            }
            // AND A TRANSFER HANDS THE SHIELD OVER WITH ITS CARD. The kernel
            // tells a pass from an attack, because on the wire they are the same
            // event and only the rules say which is which; the web names the
            // seats that laid cards and asks.
            if (step.type === 'attack_pass' && step.seat !== undefined) {
                const final = sequenceFinalRef.current;
                const handOff = final
                    ? animRolesPassHandOff(shown.roles, 1 << step.seat, rolesOf(final).defender)
                    : null;
                // The two swords need no line of their own: the coins turn them
                // off the departing / arriving seats this sync publishes.
                if (handOff) roles.syncRoles(handOff, { tableOpen: true });
            }
        },
        onIdle: () => {
            // THE CLOSING BEAT: everything the three rules above did not already
            // move. By now the sweep has landed and the board is the one the
            // sequence settles on, so this is the hand-off - the shield to the
            // next defender, the opening sword to the next seat to swing - and
            // the rotate-out of every sword that nobody took over.
            roleMotionRef.current.syncFromView(
                sequenceFinalRef.current ?? currentGameRef.current, ROLE_CLAIM.handOff, false);
            sequenceFinalRef.current = null;
            if (pendingCompletionCallbackRef.current && remainingSequenceEventsRef.current === 0) {
                const callback = pendingCompletionCallbackRef.current;
                pendingCompletionCallbackRef.current = null;
                callback();
            }
        },
        // A bot's move is a move: the bump timer only nudges a table nobody moved.
        onQueued: (steps) => {
            if (!url_game_id) return;
            const seats = games[url_game_id]?.seats;
            if (!seats) return;
            if (steps.some((e) => e.seat !== undefined && seats[e.seat]?.isAi)) {
                hasBotMovedRef.current = true;
            }
        },
    });

    // A BOARD THAT CHANGED WITH NO SEQUENCE OF ITS OWN still moves the roles: a
    // first load, a resync, a move whose whole stream this client had already
    // animated as a prediction. A sequence syncs its own roles at its closing
    // beat, so this write is a BYSTANDER's - and whether a bystander may write
    // while a sequence runs is `anim_shown_ledger_allows`, asked rather than
    // decided here (src/state/useRoleMotion.ts). iMessage has the same pair: a
    // board's `onChange` syncs the roles when the change carried no sequence.
    useEffect(() => {
        roleMotion.syncFromView(url_game_id ? games[url_game_id] : undefined,
                                ROLE_CLAIM.bystander, isAnimating);
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [url_game_id, games, isAnimating]);

    // Get animation state for a specific card
    const getCardAnimationState = (card: Card, owner?: number | string) => {
        const cardKey = getCardKeyOwner(card, owner);
        const cardAnimation = animatingCards.get(cardKey);

        if (cardAnimation) {
            return {
                isAnimating: true,
                animationType: cardAnimation.animationType,
                progress: cardAnimation.progress,
                fromLocation: cardAnimation.fromLocation,
                toLocation: cardAnimation.toLocation
            };
        }

        return {
            isAnimating: false,
            animationType: null,
            progress: 0,
            fromLocation: null,
            toLocation: null
        };
    };

    // ONE PREDICTED MOVE - an attack, a pass, a cover, a pickup - from tap to verdict:
    //
    //   1. THE REQUEST GOES FIRST. The server is authoritative and rejects an
    //      illegal move, so the round trip is never held up on the local gate.
    //   2. The same wire bytes are judged here (the kernel's gate), and only a
    //      legal move is predicted: its cards fly at once and are remembered as
    //      pending. THE BOARD THE MOVE LEAVES IS MADE AT ITS LANDING, by the
    //      kernel, from whatever is on screen then (clientBoards.optimisticBoard) -
    //      a push can commit fresher state inside the flight, and a board derived
    //      at tap time would write the stale table and hand back over it.
    //   3. A refusal flies home each card that is still pending (a push may have
    //      settled or doomed it already), over the board with the move undone -
    //      while the board on screen is still the version the move was made on.
    //      Once a push has moved the game on, the server's board is on its way
    //      through the queue and none is made here.
    const play = async (
        move: AwireMove,
        send: (wire: Uint8Array) => Promise<{ game_id: string }>,
        undo: (held: TableView, tap: TableView) => TableView | null,
    ): Promise<{ game_id: string }> => {
        if (!game_id || !games[game_id]) {
            throw new Error('No active game');
        }
        const game = games[game_id], seat = seatOf(game);
        // The awire bytes are built ONCE per move: the exact buffer the kernel
        // validates is what travels as the POST body. A mismatched cover/attack
        // pairing throws here (a client bug, not a race) - the caller sees a
        // rejected promise, same as a server reject.
        const wire = encodeAction(move);
        const toHand = move.kind === 'pickup', isCover = move.kind === 'cover';
        const dest = toHand ? V.ANIM_DEST_MY_HAND : V.ANIM_DEST_TABLE;
        const cards = toHand ? tableCards(game) : move.cards ?? [];
        const [from, to] = toHand ? ['table', 'hand'] as const : ['hand', 'table'] as const;

        let refused = false;
        const serverPromise = send(wire);
        let valid = true;
        try {
            validateActionWire(game, wire);
        } catch {
            valid = false;
        }
        if (valid) {
            rememberPending(optimistic, cards, dest, Date.now(),
                isCover ? move.attack_cards : undefined, move.kind === 'pass' ? wire : undefined);
            enqueue([{
                type: toHand ? 'pickup' : isCover ? 'cover' : 'attack_pass',
                cards,
                target_cards: isCover ? move.attack_cards : undefined,
                from_location: from,
                to_location: to,
                seat,
                commit_board: () => {
                    const held = currentGameRef.current;
                    return held ? optimisticBoard(held, wire) : null;
                },
                // A refusal that arrives mid-flight stops a card-laying move's
                // board landing. A refused PICKUP still takes the table into the
                // hand: its cards' return flight starts from the hand, which hides
                // them while they fly, and the refusal's board puts the table back.
                commit_if: () => toHand || !refused,
            }]);
        }

        try {
            return await serverPromise;
        } catch (error) {
            refused = true;
            const held = currentGameRef.current;
            const home = (held && held.version === game.version ? undo(held, game) : null) ?? undefined;
            enqueue(forgetRefused(optimistic, cards, dest, isCover).map((card): ClientAnimationEvent => ({
                type: 'revert', cards: [card], from_location: to, to_location: from, seat, is_revert: true, game_state: home,
            })));
            throw error;
        }
    };

    const attack = (cards: Card[]) =>
        play({ kind: 'attack', cards }, (wire) => serverActions.attack(cards, wire), (held) => withdrawn(held, cards));

    // A refused pass: the cards land back in my hand, and the lead and the shield are as they were.
    const pass = (cards: Card[]) =>
        play({ kind: 'pass', cards }, (wire) => serverActions.pass(cards, wire), (held, tap) => {
            const back = withdrawn(held, cards);
            return back && turnedBoard(back, tap.firstAttacker, tap.defender);
        });

    // A refused pickup: the table goes back to the board the pickup was made on.
    const pickup = () => play({ kind: 'pickup' }, (wire) => serverActions.pickup(wire), (_held, tap) => tap);

    // A refused cover: the covers land back in my hand, and their attacks stand uncovered.
    const cover = (coverCards: Card[], attackCards: Card[]) =>
        play({ kind: 'cover', cards: coverCards, attack_cards: attackCards },
            (wire) => serverActions.cover(coverCards, attackCards, wire), (held) => withdrawn(held, coverCards));

    /**
     * MY OWN GOOD, FLIPPED THE INSTANT I TAP.
     *
     * The three lines above this one used to be one: `await serverActions.good()`.
     * Everything else on the branch makes ANOTHER player's good arrive as its own
     * push; none of it helps my own, which still waited for the round trip and
     * then flipped - the same lag the owner named, wearing the one hat the server
     * fix does not take off.
     *
     * WHAT IS BEING REPRODUCED IS THE VISIBLE PROPERTY, NOT THE MECHANISM.
     * iMessage flips this badge when the move is STAGED into the bubble, and says
     * so in the round-21 block of MessageTableView+Sequence.swift: "that board
     * flipped the mark when the move was staged (the owner: 'we shouldn't show
     * the good animation as staging it should've already shown it'), so by the
     * time the settlement is released there is no difference left to find."
     * THE WEB HAS NO STAGING. A Messages extension stages a move into a bubble
     * and sends it as a separate act; that split is the platform's, not a design
     * (the owner: "in the web there is no staged + actually sent distinction.
     * Really more of an iMessage quirk"). Here the tap IS the send, so the only
     * moment available is the optimistic submit, and that is where the flip goes.
     * Same result on screen, different machinery - do not go looking for a
     * staging concept in src/, there is none.
     *
     * The property both hosts end up with: the check is up the moment I act, and
     * when the confirmation lands there is no difference left to animate. That
     * second half is the kernel's doing and not a special case here -
     * `anim_goods_opening` adds only the goods the shown roles DO NOT already
     * wear, so the push that confirms my own good finds nothing to add and the
     * badge does not turn twice.
     *
     * SAME POSTURE AS EVERY OTHER OPTIMISTIC MOVE (attack, pass, pickup, cover):
     * send first, because the server is authoritative; predict only what the
     * kernel's own gate says is legal; and put it back if the server refuses.
     * The revert matters MORE here than on iMessage, not less: a staged move can
     * be un-staged by hand before it is ever sent, but the web's tap is the send,
     * so a refusal is the only way back and a good that flipped and was then
     * refused would strand a check on a seat that never said it.
     */
    const good = async (): Promise<{ game_id: string }> => {
        const game = game_id ? games[game_id] : undefined;
        const seat = seatOf(game);
        // The same awire bytes the request carries, put through the kernel's gate
        // (c/src/legal.c, via validateActionWire) before anything is drawn. A
        // spectator has no seat and predicts nothing.
        const wire = encodeAction({ kind: 'good' });
        const serverPromise = serverActions.good();

        let flipped = false;
        if (game && seat !== undefined) {
            let legal = true;
            try { validateActionWire(game, wire); } catch { legal = false; }
            const shown = legal ? roleMotionRef.current.read() : null;
            // The kernel decides what the badges become; this file only asks.
            const opening = shown
                ? animRolesGoodsOpening(shown.roles, shown.roles.goodMask | (1 << seat))
                : null;
            if (opening) {
                // A coin turning where it stands - nothing flies, exactly as the
                // same call does for a good that arrives on a push.
                roleMotionRef.current.syncRoles(opening);
                flipped = true;
            }
        }

        try {
            return await serverPromise;
        } catch (error) {
            // REFUSED: take the check back off. Asked of the kernel again rather
            // than remembered, so a board that has moved on underneath the
            // refusal answers for itself - if my bit is no longer shown (a round
            // transition swept every good while the request was in the air),
            // `anim_goods_cleared` finds nothing removed and returns null.
            if (flipped && seat !== undefined) {
                const shown = roleMotionRef.current.read();
                const cleared = shown
                    ? animRolesGoodsCleared(shown.roles, shown.roles.goodMask & ~(1 << seat))
                    : null;
                if (cleared) roleMotionRef.current.syncRoles(cleared);
            }
            throw error;
        }
    };

    const resetAnimations = useCallback(() => {
        pendingCompletionCallbackRef.current = null;
        remainingSequenceEventsRef.current = 0;
        resetRun();
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    return (
        <AnimationContext.Provider value={{
            isAnimating,
            currentAnimation,
            flightMs,
            rowMs,
            heldPiles,
            arrivingPiles,
            inFlightFromDeck,
            inFlightToFlipped,
            trumpFlight,
            getCardAnimationState,
            shownRoles: roleMotion.shown,
            roleHandOff: roleMotion.handOff,
            publishRolePad: roleMotion.publishPad,
            noteRoleFlightFrame: roleMotion.noteFlightFrame,
            landRoleHandOff: roleMotion.landHandOff,
            attack,
            pass,
            pickup,
            cover,
            good,
            resetAnimations
        }}>
            {children}
        </AnimationContext.Provider>
    );
};

export const useAnimation = () => {
    const context = useContext(AnimationContext);
    if (!context) {
        throw new Error('useAnimation must be used within an AnimationProvider');
    }
    return context;
};
 