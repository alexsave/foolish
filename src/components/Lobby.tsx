// this will listen on a different channel than the main game one. I think
// or at least the UI is different enough we can have a different route
import { useServer } from "../contexts/ServerContext";
import { useParams } from "react-router-dom";
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
    onDragStart: (e: React.DragEvent, index: number) => void;
    onDragOver: (e: React.DragEvent, index: number) => void;
    onDragLeave: (e: React.DragEvent) => void;
    onDrop: (e: React.DragEvent, index: number) => void;
    onDragEnd: (e: React.DragEvent) => void;
    onRemoveBot?: (botId: string) => void;
}

const PlayerCard: React.FC<PlayerCardProps> = ({
    player,
    index,
    isDragging,
    isDropTarget,
    onDragStart,
    onDragOver,
    onDragLeave,
    onDrop,
    onDragEnd,
    onRemoveBot,
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
        width: '200px',
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
        marginBottom: '6px',
        zIndex: 1500,
        pointerEvents: isDragging ? 'none' : 'auto'
    };

    return (
        <div
            key={player.player_id}
            draggable={true}
            onDragStart={(e) => onDragStart(e, index)}
            onDragOver={(e) => onDragOver(e, index)}
            onDragLeave={onDragLeave}
            onDrop={(e) => onDrop(e, index)}
            onDragEnd={onDragEnd}
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
            <p style={{ zIndex: 10 }}>{player.is_ai ? '🤖 ' : ''}{player.name}</p>
            <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                {player.status !== 'idle' ? '🟢' : player.player_id === user_id ? <button
                    onClick={() => startGame(game_id)}
                    style={{
                        ...woodButtonStyle,
                        padding: '4px 8px',
                        fontSize: '12px',
                        border: '3px solid #5D3A1A',
                        borderRadius: '0',
                        color: '#ffffff',
                        fontWeight: 'bold',
                        cursor: 'pointer',
                        transition: 'all 0.2s ease',
                        textShadow: '2px 2px 4px rgba(0,0,0,0.9)',
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
                >Ready</button> : '🔴'}
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
                            fontSize: '12px'
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
    const { game, updateGameName, rearrangePlayer, addBot, exitGame, joinGame } = useServer();

    // Get wood styles with seeds - memoized to prevent new object creation
    const woodButtonBaseStyle = useWoodStyle(0.2);
    const woodInputBaseStyle = useWoodStyle(0.8);
    const woodQRBaseStyle = useWoodStyle(0.3);
    
    const woodButtonStyle = useMemo(() => woodButtonBaseStyle, [woodButtonBaseStyle]);
    const woodButtonHoverStyle = useMemo(() => ({ 
        ...woodButtonBaseStyle, 
        filter: 'brightness(1.1) contrast(1.1)', 
        transform: 'translateY(-1px)',
        boxShadow: `inset 0 2px 0 rgba(255,255,255,0.3), inset 0 -2px 0 rgba(0,0,0,0.4), 0 4px 8px rgba(0,0,0,0.5)`
    }), [woodButtonBaseStyle]);
    const woodInputStyle = useMemo(() => woodInputBaseStyle, [woodInputBaseStyle]);
    const woodQRStyle = useMemo(() => woodQRBaseStyle, [woodQRBaseStyle]);

    const [isEditingName, setIsEditingName] = useState(false);
    const [editingName, setEditingName] = useState('');

    // Enhanced drag and drop state
    const [draggedIndex, setDraggedIndex] = useState<number | null>(null);
    const [dragOverIndex, setDragOverIndex] = useState<number | null>(null);
    const [localPlayerOrder, setLocalPlayerOrder] = useState<PublicPlayer[]>([]);
    const [isDragging, setIsDragging] = useState(false);
    const [draggedElement, setDraggedElement] = useState<HTMLElement | null>(null);

    const rearrangeTimerRef = useRef<NodeJS.Timeout | null>(null);
    const inputRef = useRef<HTMLInputElement>(null);

    // Update local player order when game changes
    useEffect(() => {
        if (game?.players) {
            setLocalPlayerOrder(game.players);
        }
    }, [game?.players]);

    // Cleanup timer on unmount
    useEffect(() => {
        return () => {
            if (rearrangeTimerRef.current) {
                clearTimeout(rearrangeTimerRef.current);
            }
        };
    }, []);

    // Enhanced drag behavior with mouse following
    useEffect(() => {
        const handleMouseMove = (e: MouseEvent) => {
            if (isDragging && draggedElement) {
                const style = draggedElement.style;
                style.position = 'fixed';
                style.top = e.clientY - draggedElement.clientHeight / 2 + 'px';
                style.left = e.clientX - draggedElement.clientWidth / 2 + 'px';
                style.zIndex = '1500';
                style.pointerEvents = 'none';
            }
        };

        if (isDragging) {
            document.addEventListener('mousemove', handleMouseMove);
        }

        return () => {
            document.removeEventListener('mousemove', handleMouseMove);
        };
    }, [isDragging, draggedElement]);

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
        // Cancel existing timer
        if (rearrangeTimerRef.current) {
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

        // Set new 1.3-second timer
        rearrangeTimerRef.current = setTimeout(() => {
            rearrangePlayer(game_id!, playerIds).catch(error => {
                console.error('Failed to rearrange players:', error);
                // Revert to original order on error
                setLocalPlayerOrder(originalPlayers);
            });
        }, 400);
    };

    const handleDragStart = (e: React.DragEvent, index: number) => {
        if (game?.status !== 'waiting') return;

        // Cancel any pending rearrange update since user is still actively dragging
        if (rearrangeTimerRef.current) {
            clearTimeout(rearrangeTimerRef.current);
            rearrangeTimerRef.current = null;
        }

        e.dataTransfer.setData('text/plain', '');
        setIsDragging(true);
        setDraggedIndex(index);
        setDraggedElement(e.currentTarget as HTMLElement);
        e.dataTransfer.effectAllowed = 'move';
    };

    const handleDragEnd = (e: React.DragEvent) => {
        if (!isDragging || draggedIndex === null) return;

        setIsDragging(false);

        // Reset drag state
        setDraggedIndex(null);
        setDragOverIndex(null);
        setDraggedElement(null);

        // Reset element styles
        const element = e.currentTarget as HTMLElement;
        element.style.position = '';
        element.style.top = '';
        element.style.left = '';
        element.style.zIndex = '';
    };

    const handleDragOver = (e: React.DragEvent, index: number) => {
        if (game?.status !== 'waiting' || !isDragging || draggedIndex === null) return;

        e.preventDefault();
        e.dataTransfer.dropEffect = 'move';

        if (index !== draggedIndex) {
            const newOrder = [...localPlayerOrder];
            const draggedPlayer = newOrder[draggedIndex];

            // Remove dragged player from current position
            newOrder.splice(draggedIndex, 1);

            // Insert at new position
            newOrder.splice(index, 0, draggedPlayer);

            setLocalPlayerOrder(newOrder);
            setDraggedIndex(index);
            scheduleRearrangeUpdate(newOrder);
        }
    };

    const handleDragLeave = (e: React.DragEvent) => {
        // Placeholder for compatibility
    };

    const handleDrop = (e: React.DragEvent, dropIndex: number) => {
        e.preventDefault();
        // Actual drop logic handled in handleDragEnd
    };

    return <div style={{
        touchAction: 'manipulation',
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center',
        height: '100%',
        width: '100%'
    }}>
        <WoolBackgroundLayer />
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
                border: isEditingName ? undefined : 'none',
                padding: '0.75rem 1rem 0.5rem 1rem',
                cursor: isEditingName ? 'text' : 'pointer',
                transition: 'all 0.2s ease',
                position: 'relative',
                zIndex: 1000
            }}
            title={!isEditingName ? "Click to edit game name" : undefined}
        />
        <h2 style={{ margin: '1rem' }}>Game ID: {game_id}</h2>
        <div style={{ 
            marginBottom: '10px',
            position: 'relative',
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            width: '228px',
            height: '228px',
            ...woodQRStyle,  // Wood background
            //padding: '3px',
            border: '2px solid #5D3A1A',
            boxSizing: 'border-box',
            boxShadow: `
                inset 0 1px 0 rgba(255,255,255,0.2),
                inset 0 -1px 0 rgba(0,0,0,0.3),
                0 3px 6px rgba(0,0,0,0.4)`
        }}>
            <div style={{
                position: 'relative',
                width: '204px',
                height: '204px',
                backgroundColor: 'rgba(139, 69, 19, 0.0)',  // Light wood tone for background
            }}>
                <QRCodeSVG 
                    value={qrUrl} 
                    size={204} 
                    fgColor="rgba(70, 35, 20, 0.7)"  // Dark wood tone for QR pattern
                    bgColor="transparent"
                    style={{
                        mixBlendMode: 'color-burn',  // Blend with wood background
                        filter: 'contrast(1.2) brightness(0.9) blur(.3px)'
                    }}
                />
            </div>
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
                    onDragOver={handleDragOver}
                    onDragLeave={handleDragLeave}
                    onDrop={handleDrop}
                    onDragEnd={handleDragEnd}
                    onRemoveBot={(botId) => exitGame(game_id!, botId)}
                />
            ))
        }
        {game.status === 'waiting' && (
            <div style={{ marginTop: '20px', display: 'flex', flexDirection: 'column', gap: '10px', alignItems: 'center' }}>
                {game.self && game.players.length < MAX_PLAYERS && (
                    <button 
                        onClick={() => addBot(game_id!)}
                        style={{
                            ...woodButtonStyle,
                            padding: '10px 20px',
                            fontSize: '16px',
                            border: '3px solid #5D3A1A',
                            borderRadius: '0',
                            color: '#ffffff',
                            fontWeight: 'bold',
                            cursor: 'pointer',
                            transition: 'all 0.2s ease',
                            textShadow: '2px 2px 4px rgba(0,0,0,0.9)',
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
                        Add Bot
                    </button>
                )}
                
                {/* Exit/Join buttons */}
                {game.self ? (
                    <button 
                        onClick={() => exitGame(game_id!)}
                        style={{
                            ...woodButtonStyle,
                            padding: '10px 20px',
                            fontSize: '16px',
                            border: '3px solid #5D3A1A',
                            borderRadius: '0',
                            color: '#ffffff',
                            fontWeight: 'bold',
                            cursor: 'pointer',
                            transition: 'all 0.2s ease',
                            textShadow: '2px 2px 4px rgba(0,0,0,0.9)',
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
                        Exit Game
                    </button>
                ) : game.players.length < MAX_PLAYERS && (
                    <button 
                        onClick={() => joinGame(game_id!)}
                        style={{
                            ...woodButtonStyle,
                            padding: '10px 20px',
                            fontSize: '16px',
                            border: '3px solid #5D3A1A',
                            borderRadius: '0',
                            color: '#ffffff',
                            fontWeight: 'bold',
                            cursor: 'pointer',
                            transition: 'all 0.2s ease',
                            textShadow: '2px 2px 4px rgba(0,0,0,0.9)',
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
                        Join Game
                    </button>
                )}
            </div>
        )}
    </div>;
};