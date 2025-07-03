// this will listen on a different channel than the main game one. I think
import { useNavigate } from "react-router-dom";
// or at least the UI is different enough we can have a different route
import { useServer } from "../contexts/ServerContext";
import { useParams } from "react-router-dom";
import { QRCodeSVG } from "qrcode.react";
import { useEffect, useState, useRef } from "react";
import { WEBSITE_DOMAIN } from "../constants/constants";
import { useAuth } from "../contexts/AuthContext";
import { PublicPlayer } from "../common/types";

export const Lobby = () => {
    const game_id = useParams().game_id?.toLowerCase();
    const { user_id } = useAuth();
    const { startGame, game, loadGame, updateGameName, rearrangePlayer } = useServer();
    const navigate = useNavigate();
    
    const [isEditingName, setIsEditingName] = useState(false);
    const [editingName, setEditingName] = useState('');
    
    // Enhanced drag and drop state
    const [draggedIndex, setDraggedIndex] = useState<number | null>(null);
    const [dragOverIndex, setDragOverIndex] = useState<number | null>(null);
    const [localPlayerOrder, setLocalPlayerOrder] = useState<PublicPlayer[]>([]);
    const [isDragging, setIsDragging] = useState(false);
    const [draggedElement, setDraggedElement] = useState<HTMLElement | null>(null);
    const [dragStartIndex, setDragStartIndex] = useState<number>(0);
    
    const rearrangeTimerRef = useRef<NodeJS.Timeout | null>(null);
    const inputRef = useRef<HTMLInputElement>(null);
    
    // Automatically navigate when game status is no longer waiting
    useEffect(() => {
        if (game && game.status !== 'waiting') {
            loadGame(game_id!).then(() => navigate(`/game/${game_id}`)).catch(error => {
                console.log('Game not found when navigating from lobby:', error.message);
            });
        }
    }, [game?.status, game_id, loadGame, navigate]);

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

    // Prevent page scrolling/dragging during touch interactions but allow legitimate drags
    useEffect(() => {
        const preventPageScroll = (e: TouchEvent) => {
            const target = e.target as HTMLElement;
            
            // Allow dragging on draggable elements (player blocks)
            if (target.closest('[draggable="true"]')) {
                return; // Don't prevent - allow legitimate drag
            }
            
            // Allow interactions on interactive elements (buttons, inputs, or anything with higher z-index)
            if (target.tagName === 'BUTTON' || 
                target.tagName === 'INPUT' || 
                target.closest('input') ||
                window.getComputedStyle(target).zIndex === '1000') {
                return; // Don't prevent - allow button clicks and input focus
            }
            
            // Prevent page scrolling/panning on background/text areas
            if (e.touches.length > 1 || (e.touches.length === 1 && e.type === 'touchmove')) {
                e.preventDefault();
            }
        };

        // Only prevent touchmove to avoid interfering with clicks/taps
        document.addEventListener("touchmove", preventPageScroll, { passive: false });

        // Cleanup function to remove event listeners
        return () => {
            document.removeEventListener("touchmove", preventPageScroll);
        };
    }, []);
    
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
            originalPlayers.findIndex(origPlayer => origPlayer.id === newPlayer.id)
        );

        // Set new 6-second timer
        rearrangeTimerRef.current = setTimeout(() => {
            rearrangePlayer(game_id!, indices).catch(error => {
                console.error('Failed to rearrange players:', error);
                // Revert to original order on error
                setLocalPlayerOrder(originalPlayers);
            });
        }, 6000);
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
        setDragStartIndex(index);
        e.dataTransfer.effectAllowed = 'move';
    };

    const handleDragEnd = (e: React.DragEvent) => {
        if (!isDragging || draggedIndex === null) return;
        
        setIsDragging(false);
        
        // Reset drag state
        setDraggedIndex(null);
        setDragOverIndex(null);
        setDraggedElement(null);
        setDragStartIndex(0);
        
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



    return (
        <div style={{ 
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
                localPlayerOrder.map((player: PublicPlayer, index: number) => {
                    const isDragging = draggedIndex === index;
                    const isDropTarget = dragOverIndex === index;
                    const canDrag = game?.status === 'waiting';
                    
                    return (
                        <div
                            key={player.id}
                            draggable={canDrag}
                            onDragStart={(e) => handleDragStart(e, index)}
                            onDragOver={(e) => handleDragOver(e, index)}
                            onDragLeave={handleDragLeave}
                            onDrop={(e) => handleDrop(e, index)}
                            onDragEnd={handleDragEnd}
                            style={{
                                width: '180px',
                                display: 'flex',
                                flexDirection: 'row',
                                justifyContent: 'space-between',
                                gap: '10px',
                                padding: '0 10px',
                                color: 'white',
                                border: '1px solid white',
                                opacity: isDragging && draggedIndex === index ? 0.3 : 1,
                                backgroundColor: isDropTarget ? 'rgba(255, 255, 255, 0.1)' : 'transparent',
                                cursor: canDrag ? 'move' : 'default',
                                transition: isDragging && draggedIndex === index ? 'none' : 'all 0.2s ease',
                                transform: isDragging && draggedIndex === index ? 'scale(1.05)' : 'scale(1)',
                                userSelect: 'none',
                                borderRadius: '4px',
                                marginBottom: '4px',
                                position: 'relative',
                                //zIndex: isDragging && draggedIndex === index ? 1500 : 1001,
                                zIndex: 1500,
                                pointerEvents: isDragging && draggedIndex === index ? 'none' : 'auto'
                            }}
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
                            <p style={{ zIndex: 10 }}>{player.name}</p>
                            <p>{player.status !== 'idle' ? '🟢' :
                                player.id === user_id ? <button 
                                    onClick={() => {
                                        startGame(game_id!);
                                    }}
                                    style={{
                                        position: 'relative',
                                        zIndex: 1001
                                    }}
                                >Ready</button> : '🔴'}</p>
                        </div>
                    );
                })
            }
        </div>
    );
};