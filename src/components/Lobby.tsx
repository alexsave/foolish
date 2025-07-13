// this will listen on a different channel than the main game one. I think
// or at least the UI is different enough we can have a different route
import { useServer } from "../contexts/ServerContext";
import { useParams } from "react-router-dom";
import { QRCodeSVG } from "qrcode.react";
import { useEffect, useState, useRef } from "react";
import { WEBSITE_DOMAIN } from "../constants/constants";
import { useAuth } from "../contexts/AuthContext";
import { PublicPlayer } from "../common/types";
import { usePreventScroll } from "../hooks/usePreventScroll";

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
    onDragEnd
}) => {
    const game_id = useParams().game_id!.toLowerCase();
    const { startGame } = useServer();
    const { user_id } = useAuth();
    const style: React.CSSProperties = {
        width: '180px',
        display: 'flex',
        flexDirection: 'row',
        justifyContent: 'space-between',
        gap: '10px',
        padding: '0 10px',
        color: 'white',
        border: '1px solid white',
        opacity: isDragging ? 0.3 : 1,
        backgroundColor: isDropTarget ? 'rgba(255, 255, 255, 0.1)' : 'transparent',
        cursor: 'move',
        transition: isDragging ? 'none' : 'all 0.2s ease',
        transform: isDragging ? 'scale(1.05)' : 'scale(1)',
        userSelect: 'none',
        borderRadius: '4px',
        marginBottom: '4px',
        position: 'relative',
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
            <p>{player.status !== 'idle' ? '🟢' : player.player_id === user_id ? <button
                onClick={() => startGame(game_id)}
            >Ready</button> : '🔴'}</p>
        </div>
    );
};

export const Lobby = () => {
    const game_id = useParams().game_id?.toLowerCase();
    const { game, updateGameName, rearrangePlayer, addBot } = useServer();

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

        // Create indices array based on original order
        const originalPlayers = game!.players;
        const indices = newOrder.map(newPlayer =>
            originalPlayers.findIndex(origPlayer => origPlayer.player_id === newPlayer.player_id)
        );

        // Set new 2-second timer
        rearrangeTimerRef.current = setTimeout(() => {
            rearrangePlayer(game_id!, indices).catch(error => {
                console.error('Failed to rearrange players:', error);
                // Revert to original order on error
                setLocalPlayerOrder(originalPlayers);
            });
        }, 2000);
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
                width: '100%',
                fontSize: '2rem',
                fontWeight: 'bold',
                textAlign: 'center',
                color: isEditingName ? 'black' : 'white',
                background: isEditingName ? 'white' : 'transparent',
                border: 'none',
                padding: '0.75rem 0 0.5rem 0',
                cursor: isEditingName ? 'text' : 'pointer',
                transition: 'all 0.2s ease',
                position: 'relative',
                zIndex: 1000
            }}
            title={!isEditingName ? "Click to edit game name" : undefined}
        />
        <h2 style={{ margin: '1rem' }}>Game ID: {game_id}</h2>
        <div style={{ marginBottom: '20px' }}>
            <QRCodeSVG value={qrUrl} size={200} fgColor="rgb(152, 38, 33)" bgColor="rgb(255, 255, 255)" />
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
                />
            ))
        }
        {game.status === 'waiting' && (
            <div style={{ marginTop: '20px' }}>
                <button 
                    onClick={() => addBot(game_id!)}
                    style={{
                        padding: '10px 20px',
                        backgroundColor: '#4CAF50',
                        color: 'white',
                        border: 'none',
                        borderRadius: '5px',
                        cursor: 'pointer',
                        fontSize: '16px',
                        fontWeight: 'bold'
                    }}
                >
                    Add Bot
                </button>
            </div>
        )}
    </div>;
};