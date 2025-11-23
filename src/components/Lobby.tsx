// this will listen on a different channel than the main game one. I think
// or at least the UI is different enough we can have a different route
import { useServer } from "../contexts/ServerContext";
import { useParams, useNavigate } from "react-router-dom";
import { useEffect, useState, useRef, useMemo } from "react";
import { WEBSITE_DOMAIN } from "../constants/constants";
import { useAuth } from "../contexts/AuthContext";
import { QRCodeSVG } from "qrcode.react";
import { PublicPlayer } from "../common/types";
import { usePreventScroll } from "../hooks/usePreventScroll";
import { MAX_PLAYERS } from "../common/constants";
import { useWoodStyle } from "./WoodTexture";
import { WoolBackgroundLayer } from "./WoolBackgroundLayer";

interface PlayerCardProps {
    player: PublicPlayer;
    index: number;
    isDragging: boolean;
    isDropTarget: boolean;
    onDragStart: (e: React.MouseEvent | React.TouchEvent, index: number) => void;
    onRemoveBot?: (botId: string) => void;
    onExitGame?: () => void;
    isRearranging: boolean;
    pendingReady: boolean;
    onReadyClick: () => void;
}

const PlayerCard: React.FC<PlayerCardProps> = ({
    player,
    index,
    isDragging,
    isDropTarget,
    onDragStart,
    onRemoveBot,
    onExitGame,
    isRearranging,
    pendingReady,
    onReadyClick,
}) => {
    const game_id = useParams().game_id!.toLowerCase();
    const { startGame, game } = useServer();
    const gameStatus = game?.status;
    const { user_id } = useAuth();
    
    // Generate unique wood texture seed and transform for each player card
    const playerSeed = (player.player_id.charCodeAt(0) + player.player_id.charCodeAt(1)) / 200;
    const flip = (player.player_id.charCodeAt(3) || 0) % 2 === 0 ? 1 : -1;
    
    // Get wood styles with seeds
    const woodButtonStyle = useWoodStyle(0.2);
    const woodButtonHoverStyle = { ...woodButtonStyle, filter: 'brightness(1.1) contrast(1.1)', transform: 'translateY(-1px)' };
    const playerCardWoodStyle = useWoodStyle(playerSeed);
    
    const style: React.CSSProperties = {
        border: '2px solid #5D3A1A',
        borderRadius: '0',
        boxShadow: `
            inset 0 1px 0 rgba(255,255,255,0.2),
            inset 0 -1px 0 rgba(0,0,0,0.3),
            0 2px 4px rgba(0,0,0,0.4)`,
        position: 'relative' as const,
        overflow: 'hidden' as const,
        width: '260px',
        boxSizing: 'border-box' as const,
        //height: '60px',
        minHeight: '50px',
        display: 'flex',
        flexDirection: 'row',
        justifyContent: 'space-between',
        gap: '10px',
        padding: '8px 12px',
        opacity: isDragging ? 0.3 : 1,
        backgroundColor: isDropTarget ? 'rgba(255, 255, 255, 0.1)' : undefined,
        cursor: 'move',
        transition: isDragging ? 'none' : 'all 0.2s ease',
        transform: isDragging ? 'scale(1.05)' : 'scale(1)', // Don't flip the entire card
        userSelect: 'none',
        marginBottom: '4px',
        zIndex: 1500,
        pointerEvents: isDragging ? 'none' : 'auto'
    };

    return (
        <div
            key={player.player_id}
            data-player-index={index}
            onMouseDown={(e) => onDragStart(e, index)}
            onTouchStart={(e) => onDragStart(e, index)}
            style={style}
        >
            {/* Wood texture background layer - can be transformed independently */}
            <div style={{
                position: 'absolute',
                top: 0,
                left: 0,
                right: 0,
                bottom: 0,
                zIndex: -1,
                ...playerCardWoodStyle,
                transform: `scaleX(${flip})`,
                transformOrigin: 'center center'
            }} />
            
            {/* Invisible overlay to prevent text selection during drag operations */}
            <div style={{
                position: 'absolute',
                top: 0,
                left: 0,
                right: 0,
                bottom: 0,
                zIndex: 1600,
                pointerEvents: isDragging ? 'auto' : 'none',
                userSelect: 'none'
            }} />
            <p style={{ zIndex: 10, textAlign: 'center',  lineHeight: '30px', justifyContent: 'center', padding: '0 5px', margin: '0' }}>{player.is_ai ? '🤖 ' : ''}{player.name}</p>
            <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                {player.status !== 'idle' ? '🟢' : player.player_id === user_id ? (
                    <>
                        <button
                            onClick={onReadyClick}
                            style={{
                                ...woodButtonStyle,
                                padding: '4px 8px',
                                fontSize: '12px',
                                border: '3px solid #5D3A1A',
                                borderRadius: '0',
                                fontWeight: 'bold',
                                cursor: 'pointer',
                                transition: 'all 0.2s ease',
                                boxShadow: `inset 0 1px 0 rgba(255,255,255,0.2), inset 0 -1px 0 rgba(0,0,0,0.3), 0 2px 4px rgba(0,0,0,0.4)`,
                                position: 'relative' as const,
                                overflow: 'hidden' as const,
                                opacity: pendingReady ? 0.7 : 1,
                                zIndex: 1700, // Above drag overlay to prevent accidental drags
                            }}
                            onMouseEnter={(e) => {
                                Object.assign(e.currentTarget.style, woodButtonHoverStyle);
                            }}
                            onMouseLeave={(e) => {
                                Object.assign(e.currentTarget.style, woodButtonStyle);
                            }}
                        >
                            <span style={{
                                color: 'rgba(70, 35, 20, 0.8)',
                                mixBlendMode: 'color-burn',
                                filter: 'contrast(1.2) brightness(0.9) blur(.3px)',
                            }}>{pendingReady ? '⏳ Ready' : 'Ready'}</span>
                        </button>
                        {gameStatus === 'waiting' && onExitGame && (
                            <button
                                onClick={(e) => {
                                    e.stopPropagation();
                                    onExitGame();
                                }}
                                style={{
                                    padding: '2px 6px',
                                    backgroundColor: '#f44336',
                                    color: 'white',
                                    border: 'none',
                                    borderRadius: '3px',
                                    cursor: 'pointer',
                                    fontSize: '12px',
                                    position: 'relative' as const,
                                    zIndex: 1700, // Above drag overlay to prevent accidental drags
                                }}
                                title="Exit game"
                            >
                                ✕
                            </button>
                        )}
                    </>
                ) : '🔴'}
                {player.is_ai && gameStatus === 'waiting' && onRemoveBot && game?.self && (
                    <button
                        onClick={(e) => {
                            e.stopPropagation();
                            onRemoveBot(player.player_id);
                        }}
                        style={{
                            padding: '2px 6px',
                            backgroundColor: '#f44336',
                            color: 'white',
                            border: 'none',
                            borderRadius: '3px',
                            cursor: 'pointer',
                            fontSize: '12px',
                            position: 'relative' as const,
                            zIndex: 1700, // Above drag overlay to prevent accidental drags
                        }}
                        title="Remove bot"
                    >
                        ✕
                    </button>
                )}
            </div>
        </div>
    );
};

export const Lobby = () => {
    const game_id = useParams().game_id?.toLowerCase();
    const { game, updateGameName, rearrangePlayer, addBot, exitGame, joinGame, startGame } = useServer();
    const navigate = useNavigate();

    // Get wood styles with seeds - memoized to prevent new object creation
    const woodButtonBaseStyle = useWoodStyle(0.2);
    const woodInputBaseStyle = useWoodStyle(0.8);
    const woodQRBaseStyle = useWoodStyle(0.3);
    const woodAddBotCardStyle = useWoodStyle(0.5);
    
    const woodButtonStyle = useMemo(() => ({
        ...woodButtonBaseStyle,
        mixBlendMode: 'normal' as const,
    }), [woodButtonBaseStyle]);
    const woodButtonHoverStyle = useMemo(() => ({ 
        ...woodButtonBaseStyle, 
        filter: 'brightness(1.1) contrast(1.1)', 
        transform: 'translateY(-1px)',
        boxShadow: `inset 0 2px 0 rgba(255,255,255,0.3), inset 0 -2px 0 rgba(0,0,0,0.4), 0 4px 8px rgba(0,0,0,0.5)`,
        mixBlendMode: 'normal' as const,
    }), [woodButtonBaseStyle]);
    const woodInputStyle = useMemo(() => woodInputBaseStyle, [woodInputBaseStyle]);
    const woodQRStyle = useMemo(() => woodQRBaseStyle, [woodQRBaseStyle]);
    const woodAddBotStyle = useMemo(() => woodAddBotCardStyle, [woodAddBotCardStyle]);

    const [isEditingName, setIsEditingName] = useState(false);
    const [editingName, setEditingName] = useState('');

    // Enhanced drag and drop state
    const [draggedIndex, setDraggedIndex] = useState<number | null>(null);
    const [dragOverIndex, setDragOverIndex] = useState<number | null>(null);
    const [localPlayerOrder, setLocalPlayerOrder] = useState<PublicPlayer[]>([]);
    const [isDragging, setIsDragging] = useState(false);
    const [draggedElement, setDraggedElement] = useState<HTMLElement | null>(null);
    const [touchStartY, setTouchStartY] = useState<number | null>(null);
    const [hasSwapped, setHasSwapped] = useState(false);
    const [isDirty, setIsDirty] = useState(false); // Track if there are ANY unsent changes to server
    const [isRearranging, setIsRearranging] = useState(false); // Track if network call is in progress
    const [hasPendingRearrange, setHasPendingRearrange] = useState(false); // Track if timer is active
    const [pendingReady, setPendingReady] = useState(false); // Track if user clicked ready while rearranging

    const rearrangeTimerRef = useRef<NodeJS.Timeout | null>(null);
    const pendingReadyRef = useRef<boolean>(false);
    const inputRef = useRef<HTMLInputElement>(null);

    // Initialize local player order only when game first loads or when we have no local modifications
    // This makes localPlayerOrder the source of truth on the client
    useEffect(() => {
        if (game?.players) {
            // Only update if we don't have a local order yet (first load)
            if (localPlayerOrder.length === 0) {
                setLocalPlayerOrder(game.players);
                return;
            }
            
            // Check if our local order differs from server order
            const serverOrderIds = game.players.map(p => p.player_id).join(',');
            const localOrderIds = localPlayerOrder.map(p => p.player_id).join(',');
            const hasLocalChanges = serverOrderIds !== localOrderIds;
            
            // Or if we're completely idle (no local modifications in progress or pending)
            // Also don't sync if we have a pending ready (waiting to start game with our local order)
            // Most importantly: don't sync if we have ANY local changes at all
            const isCompletelyIdle = !isDragging && !hasPendingRearrange && !isRearranging && !pendingReady && !hasLocalChanges;
            
            if (isCompletelyIdle) {
                setLocalPlayerOrder(game.players);
            }
        }
    }, [game?.players, isDragging, hasPendingRearrange, isRearranging, pendingReady, localPlayerOrder.length]);

    // Keep ref in sync with state
    useEffect(() => {
        pendingReadyRef.current = pendingReady;
    }, [pendingReady]);

    // Reset local state when game_id changes (navigating between lobbies)
    useEffect(() => {
        console.log('GAME ID CHANGED: Resetting local state for new lobby');
        setLocalPlayerOrder([]);
        setIsDirty(false);
        setHasSwapped(false);
        setPendingReady(false);
        pendingReadyRef.current = false;
        
        // Clear any pending timers
        if (rearrangeTimerRef.current) {
            clearTimeout(rearrangeTimerRef.current);
            rearrangeTimerRef.current = null;
            setHasPendingRearrange(false);
        }
        setIsRearranging(false);
    }, [game_id]);

    // Cleanup timer on unmount
    useEffect(() => {
        return () => {
            console.log('CLEANUP: Component unmounting, clearing timer');
            if (rearrangeTimerRef.current) {
                clearTimeout(rearrangeTimerRef.current);
                setHasPendingRearrange(false);
            }
        };
    }, []);

    // Enhanced drag behavior for mouse and touch
    useEffect(() => {
        const handleMouseMove = (e: MouseEvent | Touch) => {
            if (!(isDragging && draggedIndex !== null)) {
                return;
            }

            // Find what player card we're hovering over
            const elements = document.elementsFromPoint(e.clientX, e.clientY);
            const playerCardElement = elements.find(el => el.getAttribute('data-player-index') !== null);
            
            if (!playerCardElement) {
                return;
            }
            
            const targetIndex = parseInt(playerCardElement.getAttribute('data-player-index')!);
            if (targetIndex === draggedIndex) {
                return;
            }
            
            // Do immediate swap in the array
            const newOrder = [...localPlayerOrder];
            const draggedPlayer = newOrder[draggedIndex];
            const targetPlayer = newOrder[targetIndex];

            // Swap the players
            newOrder[draggedIndex] = targetPlayer;
            newOrder[targetIndex] = draggedPlayer;

            setLocalPlayerOrder(newOrder);
            setDraggedIndex(targetIndex); // Update dragged index to new position
            setHasSwapped(true); // Mark that a swap occurred in this drag
            setIsDirty(true); // Mark that we have unsent changes
        };

        const handleTouchMove = (e: TouchEvent) => {
            if (!(isDragging && draggedIndex !== null && e.touches.length > 0)) {
                return;
            }

            // Prevent scrolling on touch devices (only if cancelable)
            if (e.cancelable) {
                e.preventDefault();
            }
            const touch = e.touches[0];
            handleMouseMove(touch);
        };

        const handleEnd = (e: MouseEvent | TouchEvent) => {
            if (isDragging) {
                if (e.cancelable) {
                    e.preventDefault();
                }
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
    }, [isDragging, draggedIndex, localPlayerOrder]);

    usePreventScroll();

    if (!game) {
        return <div>Loading...</div>;
    }

    const qrUrl = `www.${WEBSITE_DOMAIN}/${game_id}`.toUpperCase();

    const handleStartEditing = () => {
        setIsEditingName(true);
        setEditingName(game.name);
    };

    const handleSaveName = () => {
        const trimmedName = editingName.trim();

        // Client-side validation: Check unicode character count
        if (trimmedName && Array.from(trimmedName).length > 20) {
            setIsEditingName(false);
            setEditingName('');
            return;
        }

        if (trimmedName && trimmedName !== game.name) {
            // Fire and forget - optimistic update handles UI immediately
            updateGameName(game_id!, trimmedName).catch(error => {
                console.error('Failed to update game name:', error);
                // You might want to show user-friendly error message here
            });
        }
        // Always exit editing mode immediately
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
            // Blur the input to hide cursor and keyboard on mobile
            inputRef.current?.blur();
        } else if (e.key === 'Escape') {
            handleCancelEdit();
            inputRef.current?.blur();
        }
    };

    const scheduleRearrangeUpdate = (newOrder: PublicPlayer[]) => {
        console.log('SCHEDULE REARRANGE: Called', {
            hadExistingTimer: !!rearrangeTimerRef.current,
            hasPendingRearrange,
            isRearranging
        });
        
        // Cancel existing timer
        if (rearrangeTimerRef.current) {
            console.log('SCHEDULE REARRANGE: Clearing existing timer');
            clearTimeout(rearrangeTimerRef.current);
        }

        // Create player IDs array based on new order
        const originalPlayers = game!.players;
        const playerIds = newOrder.map(player => player.player_id);

        console.log('LOBBY DEBUG: About to rearrange players:', {
            game_id: game_id,
            newOrder: newOrder.map(p => ({ name: p.name, id: p.player_id })),
            playerIds: playerIds,
            playerIds_type: typeof playerIds,
            playerIds_length: playerIds.length
        });

        // Mark that we have a pending rearrange
        console.log('SCHEDULE REARRANGE: Setting hasPendingRearrange = true');
        setHasPendingRearrange(true);

        // Set new 5-second timer (only fires after no dragging for 5 seconds)
        console.log('SCHEDULE REARRANGE: Starting 5 second timer');
        rearrangeTimerRef.current = setTimeout(() => {
            console.log('REARRANGE TIMER: Fired! Calling rearrangePlayer');
            setHasPendingRearrange(false);
            setIsRearranging(true);
            rearrangePlayer(game_id!, playerIds)
                .then(() => {
                    console.log('REARRANGE: Success, clearing dirty flag', {
                        pendingReadyRef: pendingReadyRef.current
                    });
                    setIsRearranging(false);
                    setIsDirty(false); // Clear dirty flag on successful sync
                    
                    // If user clicked ready while rearranging, start the game now
                    // Use ref to avoid closure issues with state
                    if (pendingReadyRef.current) {
                        console.log('REARRANGE: Starting game (pendingReady was true)');
                        setPendingReady(false);
                        pendingReadyRef.current = false;
                        startGame(game_id!);
                    }
                })
                .catch(error => {
                    console.error('Failed to rearrange players:', error);
                    setIsRearranging(false);
                    setPendingReady(false); // Clear pending ready on error
                    pendingReadyRef.current = false;
                    // Revert to original order on error
                    setLocalPlayerOrder(originalPlayers);
                    // Note: Keep isDirty true on error so we can retry
                });
        }, 5000);
        console.log('SCHEDULE REARRANGE: Timer created with ref:', !!rearrangeTimerRef.current);
    };

    const handleReadyClick = () => {
        // Check if our local order differs from server order
        const serverOrderIds = game!.players.map(p => p.player_id).join(',');
        const localOrderIds = localPlayerOrder.map(p => p.player_id).join(',');
        const hasLocalChanges = serverOrderIds !== localOrderIds;
        
        console.log('READY CLICK:', {
            hasLocalChanges,
            hasPendingRearrange,
            isRearranging,
            timerExists: !!rearrangeTimerRef.current,
            serverOrderIds,
            localOrderIds
        });
        
        // Always queue the ready action - this ensures we never call startGame before rearrange completes
        setPendingReady(true);
        pendingReadyRef.current = true; // Keep ref in sync
        
        // If we have local changes that haven't been scheduled yet, schedule them now
        // BUT: if there's already a pending rearrange or one in progress, DON'T reschedule
        // (this would reset the timer and cancel the existing request)
        if (hasLocalChanges && !hasPendingRearrange && !isRearranging) {
            console.log('READY: Scheduling rearrange because local changes detected');
            scheduleRearrangeUpdate(localPlayerOrder);
        } else if (!hasLocalChanges && !hasPendingRearrange && !isRearranging) {
            console.log('READY: No changes, starting game immediately');
            // No local changes and no pending operations - start immediately
            // We use setTimeout to ensure setPendingReady has taken effect
            setTimeout(() => {
                startGame(game_id!);
                setPendingReady(false);
                pendingReadyRef.current = false;
            }, 0);
        } else {
            console.log('READY: Waiting for existing rearrange to complete');
        }
        // Otherwise: hasPendingRearrange or isRearranging is true, just wait for it to complete
    };

    const handleDragStart = (e: React.MouseEvent | React.TouchEvent, index: number) => {
        if (game?.status !== 'waiting') return;

        // Don't start drag if clicking on a button (Ready, Exit, Remove bot)
        const target = e.target as HTMLElement;
        if (target.tagName === 'BUTTON' || target.closest('button')) {
            console.log('DRAG START: Clicked on button, ignoring drag');
            return;
        }

        // For mouse events, we can safely prevent default
        // For touch events, React registers them as passive, so we skip preventDefault here
        // (it will be called in the document-level touchmove handler instead)
        if ('button' in e) {
            e.preventDefault();
        }
        e.stopPropagation();

        console.log('DRAG START:', {
            hadExistingTimer: !!rearrangeTimerRef.current,
            hasPendingRearrange
        });

        // Cancel any pending rearrange update since user is still actively dragging
        if (rearrangeTimerRef.current) {
            console.log('DRAG START: Clearing timer (user started dragging again)');
            clearTimeout(rearrangeTimerRef.current);
            rearrangeTimerRef.current = null;
            setHasPendingRearrange(false); // Clear pending flag when timer is cancelled
        }

        const clientY = 'touches' in e ? e.touches[0].clientY : e.clientY;
        
        setIsDragging(true);
        setDraggedIndex(index);
        setDraggedElement(e.currentTarget as HTMLElement);
        setTouchStartY(clientY);
    };

    const handleDragEnd = () => {
        if (!isDragging || draggedIndex === null) return;

        console.log('DRAG END:', { hasSwapped, isDirty, hasPendingRearrange });

        // Schedule the final update to the server if:
        // 1. Swaps occurred in THIS drag, OR
        // 2. We have dirty changes AND no rearrange is already pending
        if (hasSwapped || (isDirty && !hasPendingRearrange && !isRearranging)) {
            console.log('DRAG END: Scheduling rearrange', { 
                reason: hasSwapped ? 'swaps in this drag' : 'dirty changes from previous drag'
            });
            scheduleRearrangeUpdate(localPlayerOrder);
        }

        setIsDragging(false);

        // Reset drag state
        setDraggedIndex(null);
        setDragOverIndex(null);
        setDraggedElement(null);
        setTouchStartY(null);
        
        // Reset the swap flag after a short delay
        setTimeout(() => {
            setHasSwapped(false);
        }, 100);
    };


    return <div style={{
        touchAction: 'manipulation',
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center',
        height: '100%',
        width: '100%',
        position: 'relative'
    }}>
        <WoolBackgroundLayer />
        <button
            onClick={() => navigate('/dashboard')}
            style={{
                position: 'absolute',
                top: '10px',
                left: '10px',
                zIndex: 2000,
                ...woodButtonStyle,
                width: '44px',
                height: '44px',
                padding: '0',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
                border: '2px solid #5D3A1A',
                borderRadius: '0',
                cursor: 'pointer',
                transition: 'all 0.2s ease',
                boxShadow: `inset 0 1px 0 rgba(255,255,255,0.2), inset 0 -1px 0 rgba(0,0,0,0.3), 0 2px 4px rgba(0,0,0,0.4)`,
                overflow: 'hidden',
            }}
            onMouseEnter={(e) => {
                Object.assign(e.currentTarget.style, woodButtonHoverStyle);
            }}
            onMouseLeave={(e) => {
                Object.assign(e.currentTarget.style, woodButtonStyle);
            }}
        >
            <span style={{
                color: 'rgba(70, 35, 20, 0.8)',
                mixBlendMode: 'color-burn',
                filter: 'contrast(1.2) brightness(0.9) blur(.3px)',
                fontSize: '28px',
                fontWeight: 900,
                lineHeight: 1,
            }}>{'<'}</span>
        </button>
        <input
            ref={inputRef}
            type="text"
            value={isEditingName ? editingName : game.name}
            onChange={(e) => {
                if (isEditingName) {
                    setEditingName(e.target.value);
                }
            }}
            onBlur={isEditingName ? handleSaveName : undefined}
            onKeyDown={handleKeyDown}
            onFocus={() => {
                if (!isEditingName) {
                    handleStartEditing();
                }
            }}
            autoFocus={isEditingName}
            inputMode={isEditingName ? 'text' : 'none'}
            style={{
                ...(isEditingName ? {
                    ...woodInputStyle,
                    border: '2px solid #5D3A1A',
                    borderRadius: '0',
                    textShadow: '2px 2px 4px rgba(0,0,0,0.9)',
                    boxShadow: `inset 2px 2px 4px rgba(0,0,0,0.4), inset -1px -1px 2px rgba(255,255,255,0.2)`
                } : {}),
                width: '100%',
                fontSize: '2rem',
                fontWeight: 'bold',
                textAlign: 'center',
                color: 'white',
                background: isEditingName ? undefined : 'transparent',
                border: isEditingName ? 'none' : 'none',
                padding: '0.75rem 1rem 0.5rem 1rem',
                cursor: isEditingName ? 'text' : 'pointer',
                transition: 'all 0.2s ease',
                position: 'relative',
                zIndex: 1000
            }}
            title={!isEditingName ? "Click to edit game name" : undefined}
        />
        <h2 style={{ margin: '.25rem' }}>ID: {game_id}</h2>
        <div style={{ 
            marginBottom: '10px',
            position: 'relative',
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            width: '120px',
            height: '120px',
            ...woodQRStyle,  // Wood background
            padding: '5px',
            border: '2px solid #5D3A1A',
            boxSizing: 'border-box',
            boxShadow: `
                inset 0 1px 0 rgba(255,255,255,0.2),
                inset 0 -1px 0 rgba(0,0,0,0.3),
                0 3px 6px rgba(0,0,0,0.4)`
        }}>
                <QRCodeSVG 
                    value={qrUrl} 
                    size={120} 
                    fgColor="rgba(70, 35, 20, 0.7)"  // Dark wood tone for QR pattern
                    bgColor="transparent"
                    style={{
                        mixBlendMode: 'color-burn',  // Blend with wood background
                        filter: 'contrast(1.2) brightness(0.9) blur(.3px)'
                    }}
                />
        </div>
        {
            localPlayerOrder.map((player: PublicPlayer, index: number) => (
                <PlayerCard
                    key={player.player_id}
                    player={player}
                    index={index}
                    isDragging={draggedIndex === index}
                    isDropTarget={dragOverIndex === index}
                    onDragStart={handleDragStart}
                    onRemoveBot={(botId) => exitGame(game_id!, botId)}
                    onExitGame={() => exitGame(game_id!)}
                    isRearranging={isRearranging}
                    pendingReady={pendingReady}
                    onReadyClick={handleReadyClick}
                />
            ))
        }
        {game.status === 'waiting' && game.self && game.players.length < MAX_PLAYERS && (
            <div 
                onClick={() => addBot(game_id!)}
                style={{
                    border: '2px solid #5D3A1A',
                    borderRadius: '0',
                    boxShadow: `
                        inset 0 1px 0 rgba(255,255,255,0.2),
                        inset 0 -1px 0 rgba(0,0,0,0.3),
                        0 2px 4px rgba(0,0,0,0.4)`,
                    position: 'relative' as const,
                    overflow: 'hidden' as const,
                    width: '260px',
                    boxSizing: 'border-box' as const,
                    display: 'flex',
                    flexDirection: 'row' as const,
                    justifyContent: 'center',
                    alignItems: 'center',
                    gap: '10px',
                    padding: '8px 12px',
                    cursor: 'pointer',
                    transition: 'all 0.2s ease',
                    userSelect: 'none' as const,
                    marginBottom: '4px',
                }}
                onMouseEnter={(e) => {
                    e.currentTarget.style.filter = 'brightness(1.1) contrast(1.1)';
                    e.currentTarget.style.transform = 'translateY(-1px)';
                }}
                onMouseLeave={(e) => {
                    e.currentTarget.style.filter = '';
                    e.currentTarget.style.transform = '';
                }}
            >
                {/* Wood texture background layer */}
                <div style={{
                    position: 'absolute',
                    top: 0,
                    left: 0,
                    right: 0,
                    bottom: 0,
                    zIndex: -1,
                    ...woodAddBotStyle,
                }} />
                <p style={{ 
                    color: 'rgba(70, 35, 20, 0.8)',
                    mixBlendMode: 'color-burn',
                    filter: 'contrast(1.2) brightness(0.9) blur(.3px)',
                    fontWeight: 'bold',
                    zIndex: 1,
                }}>Add Bot</p>
            </div>
        )}
        {game.status === 'waiting' && !game.self && game.players.length < MAX_PLAYERS && (
            <div style={{ marginTop: '10px', display: 'flex', flexDirection: 'column', gap: '10px', alignItems: 'center' }}>
                {/* Join button for non-members */}
                <button 
                    onClick={() => joinGame(game_id!)}
                    style={{
                        ...woodButtonStyle,
                        padding: '10px 20px',
                        fontSize: '16px',
                        border: '3px solid #5D3A1A',
                        borderRadius: '0',
                        fontWeight: 'bold',
                        cursor: 'pointer',
                        transition: 'all 0.2s ease',
                        boxShadow: `inset 0 1px 0 rgba(255,255,255,0.2), inset 0 -1px 0 rgba(0,0,0,0.3), 0 2px 4px rgba(0,0,0,0.4)`,
                        position: 'relative' as const,
                        overflow: 'hidden' as const,
                    }}
                    onMouseEnter={(e) => {
                        Object.assign(e.currentTarget.style, woodButtonHoverStyle);
                    }}
                    onMouseLeave={(e) => {
                        Object.assign(e.currentTarget.style, woodButtonStyle);
                    }}
                >
                    <span style={{
                        color: 'rgba(70, 35, 20, 0.8)',
                        mixBlendMode: 'color-burn',
                        filter: 'contrast(1.2) brightness(0.9) blur(.3px)',
                    }}>Join Game</span>
                </button>
            </div>
        )}
    </div>;
};