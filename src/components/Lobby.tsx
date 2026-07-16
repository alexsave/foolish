import { useServer } from "../contexts/ServerContext";
import { useParams, useRouter } from "next/navigation";
import { useEffect, useState, useRef, useMemo } from "react";
import { WEBSITE_DOMAIN } from "../constants/constants";
import { useAuth } from "../contexts/AuthContext";
import { QRCodeSVG } from "qrcode.react";
import { PLAYER_STATUS, PublicPlayer, GAME_STATUS } from "@api/core/types.ts";
import supabase from "../backend/Connector";
import { usePreventScroll } from "../hooks/usePreventScroll";
import { MAX_PLAYERS } from "@api/core/constants.ts";
import { useTexture, getTextureStyle, seedFromString, flipFromString } from "./TexturedSurface";
import { WoolBackgroundLayer } from "./WoolBackgroundLayer";
import { BackButton } from "./BackButton";
import { Text } from "./Text";
import { useLocalization } from "../contexts/LocalizationContext";
import { SovietIcon } from "./SovietIcon";
import { useStyles } from "../contexts/StyleContext";
import { botDisplayName } from "../common/botName";

interface BotOption {
    id: string;
    nickname: string;
    strategy_key: string;
}

interface PlayerCardProps {
    player: PublicPlayer;
    index: number;
    isDragging: boolean;
    isDropTarget: boolean;
    pendingReady: boolean;
    textureUrl: string | null;
    useWoodTexture: boolean;
}

const PlayerCard: React.FC<PlayerCardProps> = ({
    player,
    isDragging,
    pendingReady,
    textureUrl,
    useWoodTexture,
}) => {
    const { user_id } = useAuth();
    const playerSeed = seedFromString(player.player_id);
    const flip = flipFromString(player.player_id);
    const playerCardStyle = getTextureStyle(textureUrl, !useWoodTexture, playerSeed);
    const isReady = player.status !== PLAYER_STATUS.IDLE || (player.player_id === user_id && pendingReady);

    return (
        <div className="player-card">
            {useWoodTexture && (
                <div 
                    className="player-card__texture"
                    style={{
                        ...playerCardStyle,
                        transform: `scaleX(${flip})`,
                    }} 
                />
            )}
            <div 
                className="player-card__overlay"
                style={{ pointerEvents: isDragging ? 'auto' : 'none' }}
            />
            <p className="player-card__name">
                {player.is_ai && <><SovietIcon name="bot" size={14} /> </>}
                {player.name}
            </p>
            <div className="player-card__status">
                <SovietIcon name={isReady ? 'ready' : 'not-ready'} size={16} />
            </div>
        </div>
    );
};

export const Lobby = () => {
    const game_id = useParams<{ game_id: string }>().game_id?.toLowerCase();
    const { game, updateGameName, rearrangePlayer, addBot, exitGame, joinGame, startGame } = useServer();
    const router = useRouter();
    const { user_id } = useAuth();
    const { t } = useLocalization();
    const { woodUrl, concreteUrl } = useTexture();
    const styles = useStyles();
    const useWoodTexture = styles.texture.useWoodTexture;
    const textureUrl = useWoodTexture ? woodUrl : concreteUrl;
    
    const buttonTextureStyle = useMemo(() =>
        useWoodTexture ? getTextureStyle(textureUrl, false, 0.2) : {},
        [textureUrl, useWoodTexture]
    );

    const [isEditingName, setIsEditingName] = useState(false);
    const [editingName, setEditingName] = useState('');
    const [draggedIndex, setDraggedIndex] = useState<number | null>(null);
    const [dragOverIndex, setDragOverIndex] = useState<number | null>(null);
    const [localPlayerOrder, setLocalPlayerOrder] = useState<PublicPlayer[]>([]);
    const [isDragging, setIsDragging] = useState(false);
    const [hasSwapped, setHasSwapped] = useState(false);
    const [isDirty, setIsDirty] = useState(false);
    const [isRearranging, setIsRearranging] = useState(false);
    const [hasPendingRearrange, setHasPendingRearrange] = useState(false);
    const [pendingReady, setPendingReady] = useState(false);
    const [optimisticBotIds, setOptimisticBotIds] = useState<Set<string>>(new Set());
    // The full bot roster (newest first) for the lobby bot picker, the index the
    // left/right arrows cycle, and the real bot ids we've optimistically added but
    // the server hasn't confirmed yet (so the picker drops them immediately).
    const [allBots, setAllBots] = useState<BotOption[]>([]);
    const [selectedBotIndex, setSelectedBotIndex] = useState(0);
    const [pendingBotRealIds, setPendingBotRealIds] = useState<Set<string>>(new Set());
    // Whether the bot roster fetch has finished (regardless of success), and
    // whether the user pressed "Add Bot" before it did. Together they let a click
    // that lands during the load resolve to a SPECIFIC bot instead of falling
    // through to the server's random pick (bot_id omitted).
    const [rosterLoaded, setRosterLoaded] = useState(false);
    const [pendingAdd, setPendingAdd] = useState(false);

    // Bots the picker can still add: the roster minus those already seated and
    // those added optimistically this session (server not yet caught up).
    const selectableBots = useMemo(() => {
        const taken = new Set<string>([
            ...(game?.players ?? []).filter(p => p.is_ai).map(p => p.player_id),
            ...pendingBotRealIds,
        ]);
        return allBots.filter(b => !taken.has(b.id));
    }, [allBots, game?.players, pendingBotRealIds]);

    const rearrangeTimerRef = useRef<NodeJS.Timeout | null>(null);
    const pendingReadyRef = useRef<boolean>(false);
    const inputRef = useRef<HTMLInputElement>(null);
    // Latest handleAddBot, so the deferred-add effect (declared before the early
    // return, above handleAddBot) can invoke it without a hoisting dance.
    const addBotFnRef = useRef<((bot?: BotOption) => void) | null>(null);

    useEffect(() => {
        if (game?.players) {
            if (localPlayerOrder.length === 0) {
                setLocalPlayerOrder(game.players);
                return;
            }
            
            const serverOrderIds = game.players.map(p => p.player_id).join(',');
            const localNonOptimisticIds = localPlayerOrder
                .filter(p => !optimisticBotIds.has(p.player_id))
                .map(p => p.player_id)
                .join(',');
            const hasLocalChanges = serverOrderIds !== localNonOptimisticIds;
            
            const serverBotIds = new Set(game.players.filter(p => p.is_ai).map(p => p.player_id));
            const localNonOptimisticBotIds = localPlayerOrder
                .filter(p => p.is_ai && !optimisticBotIds.has(p.player_id))
                .map(p => p.player_id);
            
            if (serverBotIds.size > localNonOptimisticBotIds.length && optimisticBotIds.size > 0) {
                const tempBotIds = Array.from(optimisticBotIds);
                const numToReplace = Math.min(
                    serverBotIds.size - localNonOptimisticBotIds.length,
                    tempBotIds.length
                );
                
                setOptimisticBotIds(prev => {
                    const next = new Set(prev);
                    for (let i = 0; i < numToReplace; i++) {
                        next.delete(tempBotIds[i]);
                    }
                    return next;
                });
                setLocalPlayerOrder(game.players);
                return;
            }
            
            const isCompletelyIdle = !isDragging && !hasPendingRearrange && !isRearranging && !pendingReady && !hasLocalChanges && optimisticBotIds.size === 0;
            
            if (isCompletelyIdle) {
                setLocalPlayerOrder(game.players);
            }
        }
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [game?.players, isDragging, hasPendingRearrange, isRearranging, pendingReady, localPlayerOrder.length, optimisticBotIds]);

    useEffect(() => {
        pendingReadyRef.current = pendingReady;
    }, [pendingReady]);

    useEffect(() => {
        setLocalPlayerOrder(game?.players ?? []);
        setIsDirty(false);
        setHasSwapped(false);
        setPendingReady(false);
        pendingReadyRef.current = false;
        setOptimisticBotIds(new Set());
        setPendingBotRealIds(new Set());

        if (rearrangeTimerRef.current) {
            clearTimeout(rearrangeTimerRef.current);
            rearrangeTimerRef.current = null;
            setHasPendingRearrange(false);
        }
        setIsRearranging(false);
    }, [game_id, game?.players]);

    useEffect(() => {
        return () => {
            if (rearrangeTimerRef.current) {
                clearTimeout(rearrangeTimerRef.current);
                setHasPendingRearrange(false);
            }
        };
    }, []);

    // Load the bot roster once for the lobby picker, newest first so the default
    // selection lands on the most recently created bot. The bots table is
    // read-only to authenticated users (RLS). GPT bots are gated to one user
    // server-side, so we don't surface them in the picker.
    useEffect(() => {
        let cancelled = false;
        supabase
            .from('bots')
            .select('id, nickname, strategy_key')
            .order('created_at', { ascending: false })
            .then(({ data, error }) => {
                if (cancelled) return;
                if (!error && data) {
                    setAllBots(data);
                }
                // Mark the roster settled even on error, so a deferred add doesn't
                // wait forever — it falls back to the server's pick only when the
                // roster genuinely yielded nothing to choose from.
                setRosterLoaded(true);
            });
        return () => { cancelled = true; };
    }, []);

    // A click on "Add Bot" that happened before the roster loaded was parked
    // (pendingAdd) rather than sent as a bot_id-less (random) request. Once the
    // roster is in, add the bot the picker now points at — a SPECIFIC bot.
    useEffect(() => {
        if (!pendingAdd || !rosterLoaded) return;
        setPendingAdd(false);
        const bot = selectableBots.length > 0
            ? selectableBots[((selectedBotIndex % selectableBots.length) + selectableBots.length) % selectableBots.length]
            : undefined;
        addBotFnRef.current?.(bot);
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [pendingAdd, rosterLoaded, selectableBots, selectedBotIndex]);

    useEffect(() => {
        const handleMouseMove = (e: MouseEvent | Touch) => {
            if (!(isDragging && draggedIndex !== null)) return;

            const elements = document.elementsFromPoint(e.clientX, e.clientY);
            const playerCardElement = elements.find(el => el.getAttribute('data-player-index') !== null);
            
            if (!playerCardElement) return;
            
            const targetIndex = parseInt(playerCardElement.getAttribute('data-player-index')!);
            if (targetIndex === draggedIndex) return;
            
            const newOrder = [...localPlayerOrder];
            const draggedPlayer = newOrder[draggedIndex];
            const targetPlayer = newOrder[targetIndex];

            newOrder[draggedIndex] = targetPlayer;
            newOrder[targetIndex] = draggedPlayer;

            setLocalPlayerOrder(newOrder);
            setDraggedIndex(targetIndex);
            setHasSwapped(true);
            setIsDirty(true);
        };

        const handleTouchMove = (e: TouchEvent) => {
            if (!(isDragging && draggedIndex !== null && e.touches.length > 0)) return;
            if (e.cancelable) e.preventDefault();
            handleMouseMove(e.touches[0]);
        };

        const handleEnd = (e: MouseEvent | TouchEvent) => {
            if (isDragging) {
                if (e.cancelable) e.preventDefault();
                handleDragEnd();
            }
        };

        if (isDragging) {
            document.addEventListener('mousemove', handleMouseMove);
            document.addEventListener('touchmove', handleTouchMove, { passive: false });
            document.addEventListener('mouseup', handleEnd);
            document.addEventListener('touchend', handleEnd);
        }

        return () => {
            document.removeEventListener('mousemove', handleMouseMove);
            document.removeEventListener('touchmove', handleTouchMove);
            document.removeEventListener('mouseup', handleEnd);
            document.removeEventListener('touchend', handleEnd);
        };
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [isDragging, draggedIndex, localPlayerOrder]);

    usePreventScroll();

    if (!game) {
        return <div><Text id="loading" /></div>;
    }

    const qrUrl = `www.${WEBSITE_DOMAIN}/${game_id}`.toUpperCase();

    // The bot the picker is currently pointing at (most recently created by
    // default). Index wraps so the arrows cycle endlessly; null until the roster
    // loads, in which case the button falls back to a plain random "Add Bot".
    const selectedBot = selectableBots.length > 0
        ? selectableBots[((selectedBotIndex % selectableBots.length) + selectableBots.length) % selectableBots.length]
        : null;

    const handleStartEditing = () => {
        setIsEditingName(true);
        setEditingName(game.name);
    };

    const handleSaveName = () => {
        const trimmedName = editingName.trim();
        if (trimmedName && Array.from(trimmedName).length > 20) {
            setIsEditingName(false);
            setEditingName('');
            return;
        }
        if (trimmedName && trimmedName !== game.name) {
            updateGameName(game_id!, trimmedName).catch(console.error);
        }
        setIsEditingName(false);
        setEditingName('');
    };

    const handleCancelEdit = () => {
        setIsEditingName(false);
        setEditingName('');
    };

    const handleKeyDown = (e: React.KeyboardEvent) => {
        if (e.key === 'Enter') {
            handleSaveName();
            inputRef.current?.blur();
        } else if (e.key === 'Escape') {
            handleCancelEdit();
            inputRef.current?.blur();
        }
    };

    const scheduleRearrangeUpdate = (newOrder: PublicPlayer[]) => {
        if (rearrangeTimerRef.current) {
            clearTimeout(rearrangeTimerRef.current);
        }

        const originalPlayers = game!.players;
        const playerIds = newOrder.map(player => player.player_id);
        setHasPendingRearrange(true);

        rearrangeTimerRef.current = setTimeout(() => {
            setHasPendingRearrange(false);
            setIsRearranging(true);
            rearrangePlayer(game_id!, playerIds)
                .then(() => {
                    setIsRearranging(false);
                    setIsDirty(false);
                    if (pendingReadyRef.current) {
                        startGame(game_id!);
                    }
                })
                .catch(error => {
                    console.error('Failed to rearrange players:', error);
                    setIsRearranging(false);
                    setPendingReady(false);
                    pendingReadyRef.current = false;
                    setLocalPlayerOrder(originalPlayers);
                });
        }, 5000);
    };

    const handleCycleBot = (delta: number) => {
        // Read clamps with modulo, so just nudge the index; wrap is handled there.
        setSelectedBotIndex(i => i + delta);
    };

    const handleAddBot = (bot?: BotOption) => {
        if (!game_id) return;

        // Use the real bot id as the optimistic id when we know it (picker), so the
        // card's React key — and thus its texture seed (seedFromString(player_id)) —
        // stays identical once the server confirms: no plank-texture pop on the
        // red→green switch. The random fallback has no id yet, so it keeps a temp id.
        const optimisticId = bot ? bot.id : `temp-bot-${Date.now()}`;
        const optimisticBot: PublicPlayer = {
            player_id: optimisticId,
            name: bot ? bot.nickname : '',
            status: PLAYER_STATUS.IDLE,
            is_ai: true,
            hand_length: 0
        };

        setOptimisticBotIds(prev => new Set(prev).add(optimisticId));
        if (bot) setPendingBotRealIds(prev => new Set(prev).add(bot.id));
        setLocalPlayerOrder(prev => [...prev, optimisticBot]);

        addBot(game_id, bot?.id).catch(error => {
            console.error('Failed to add bot:', error);
            setOptimisticBotIds(prev => {
                const next = new Set(prev);
                next.delete(optimisticId);
                return next;
            });
            if (bot) setPendingBotRealIds(prev => {
                const next = new Set(prev);
                next.delete(bot.id);
                return next;
            });
            setLocalPlayerOrder(prev => prev.filter(p => p.player_id !== optimisticId));
        });
    };
    // Keep the deferred-add effect pointed at the current handleAddBot closure.
    addBotFnRef.current = handleAddBot;

    const handleRemovePlayer = (playerId: string, isBot: boolean) => {
        if (!game_id) return;
        setLocalPlayerOrder(prev => prev.filter(p => p.player_id !== playerId));
        exitGame(game_id, isBot ? playerId : undefined, isBot ? undefined : playerId).catch(console.error);
    };

    const handleReadyClick = () => {
        setLocalPlayerOrder(prev => prev.map(p => 
            p.player_id === user_id ? { ...p, status: PLAYER_STATUS.READY } : p
        ));
        
        const serverOrderIds = game!.players.map(p => p.player_id).join(',');
        const localOrderIds = localPlayerOrder.map(p => p.player_id).join(',');
        const hasLocalChanges = serverOrderIds !== localOrderIds;
        
        setPendingReady(true);
        pendingReadyRef.current = true;
        
        if (hasLocalChanges && !hasPendingRearrange && !isRearranging) {
            scheduleRearrangeUpdate(localPlayerOrder);
        } else if (!hasLocalChanges && !hasPendingRearrange && !isRearranging) {
            startGame(game_id!);
        }
    };

    const handleDragStart = (e: React.MouseEvent | React.TouchEvent, index: number) => {
        if (game?.status !== GAME_STATUS.WAITING) return;

        const target = e.target as HTMLElement;
        if (target.tagName === 'BUTTON' || target.closest('button')) return;

        if ('button' in e) e.preventDefault();
        e.stopPropagation();

        if (rearrangeTimerRef.current) {
            clearTimeout(rearrangeTimerRef.current);
            rearrangeTimerRef.current = null;
            setHasPendingRearrange(false);
        }

        setIsDragging(true);
        setDraggedIndex(index);
    };

    const handleDragEnd = () => {
        if (!isDragging || draggedIndex === null) return;

        if (hasSwapped || (isDirty && !hasPendingRearrange && !isRearranging)) {
            scheduleRearrangeUpdate(localPlayerOrder);
        }

        setIsDragging(false);
        setDraggedIndex(null);
        setDragOverIndex(null);
        
        setTimeout(() => setHasSwapped(false), 100);
    };

    return (
        <div className="lobby">
            <WoolBackgroundLayer />
            <BackButton />
            
            <input
                ref={inputRef}
                className={`lobby__name-input ${isEditingName ? 'lobby__name-input--editing' : ''}`}
                type="text"
                value={isEditingName ? editingName : game.name}
                onChange={(e) => isEditingName && setEditingName(e.target.value)}
                onBlur={isEditingName ? handleSaveName : undefined}
                onKeyDown={handleKeyDown}
                onFocus={() => !isEditingName && handleStartEditing()}
                autoFocus={isEditingName}
                inputMode={isEditingName ? 'text' : 'none'}
                title={!isEditingName ? t('click_to_edit') : undefined}
                style={isEditingName && useWoodTexture ? buttonTextureStyle : undefined}
            />
            
            <h2 className="lobby__game-id"><Text id="id" />: {game_id}</h2>
            
            <div className="lobby__qr-container" style={useWoodTexture ? buttonTextureStyle : undefined}>
                <QRCodeSVG value={qrUrl} size={120} fgColor="#000" bgColor="transparent" />
            </div>
            
            <div className="lobby__players">
                {localPlayerOrder.map((player: PublicPlayer, index: number) => {
                    const showExitButton = game.status === GAME_STATUS.WAITING;
                    const showRemoveBotButton = !!(player.is_ai && game.status === GAME_STATUS.WAITING && game.self);
                    const showXButton = showExitButton || showRemoveBotButton;

                    return (
                        <div 
                            key={player.player_id}
                            className={`lobby__player-wrapper ${draggedIndex === index ? 'lobby__player-wrapper--dragging' : ''}`}
                            data-player-index={index}
                            onMouseDown={(e) => handleDragStart(e, index)}
                            onTouchStart={(e) => handleDragStart(e, index)}
                        >
                            <PlayerCard
                                player={player}
                                index={index}
                                isDragging={draggedIndex === index}
                                isDropTarget={dragOverIndex === index}
                                pendingReady={pendingReady}
                                textureUrl={textureUrl}
                                useWoodTexture={useWoodTexture}
                            />
                            {showXButton && (
                                <button
                                    className="btn-remove-player"
                                    onClick={(e) => {
                                        e.stopPropagation();
                                        if (showExitButton && game.players.length === 1) {
                                            router.push('/dashboard');
                                        }
                                        handleRemovePlayer(player.player_id, showRemoveBotButton);
                                    }}
                                    style={useWoodTexture ? buttonTextureStyle : undefined}
                                    title={showRemoveBotButton ? t('remove_bot') : t('exit_game')}
                                >
                                    <span className="btn-remove-player__icon">✕</span>
                                </button>
                            )}
                        </div>
                    );
                })}
            </div>
            
            {game.status === GAME_STATUS.WAITING && game.self && localPlayerOrder.length < MAX_PLAYERS && (
                <div className="lobby__add-bot-row">
                    {selectableBots.length > 1 && (
                        <button
                            className="btn-bot-cycle"
                            onClick={() => handleCycleBot(-1)}
                            style={useWoodTexture ? buttonTextureStyle : undefined}
                            aria-label="Previous bot"
                        >
                            <span className="btn-bot-cycle__icon">‹</span>
                        </button>
                    )}
                    <div
                        className="btn-add-bot"
                        onClick={() => {
                            if (selectedBot) handleAddBot(selectedBot);
                            // Roster still loading: park the intent so the effect
                            // adds a specific bot once it arrives — never a random one.
                            else if (!rosterLoaded) setPendingAdd(true);
                            else handleAddBot();
                        }}
                        style={useWoodTexture ? buttonTextureStyle : undefined}
                    >
                        {useWoodTexture && <div className="btn-add-bot__texture" style={buttonTextureStyle} />}
                        <p className="btn-add-bot__text">
                            {selectedBot
                                ? t('add_bot_named', { name: botDisplayName(selectedBot.nickname) })
                                : t('add_bot')}
                        </p>
                    </div>
                    {selectableBots.length > 1 && (
                        <button
                            className="btn-bot-cycle"
                            onClick={() => handleCycleBot(1)}
                            style={useWoodTexture ? buttonTextureStyle : undefined}
                            aria-label="Next bot"
                        >
                            <span className="btn-bot-cycle__icon">›</span>
                        </button>
                    )}
                </div>
            )}
            
            {game.status === GAME_STATUS.WAITING && !game.self && game.players.length < MAX_PLAYERS && (
                <div className="lobby__join-section">
                    <button 
                        className="btn-wood btn-wood--md"
                        onClick={() => joinGame(game_id!)}
                        style={useWoodTexture ? buttonTextureStyle : undefined}
                    >
                        <span className="btn-wood-text"><Text id="join_game" /></span>
                    </button>
                </div>
            )}
            
            {game.status === GAME_STATUS.WAITING && game.self && !pendingReady && (
                <div className="btn-ready" onClick={handleReadyClick} style={useWoodTexture ? buttonTextureStyle : undefined}>
                    {useWoodTexture && <div className="btn-ready__texture" style={buttonTextureStyle} />}
                    <p className="btn-ready__text"><Text id="ready" /></p>
                </div>
            )}
        </div>
    );
};
