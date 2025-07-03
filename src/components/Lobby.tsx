// this will listen on a different channel than the main game one. I think
import { useNavigate } from "react-router-dom";
// or at least the UI is different enough we can have a different route
import { useServer } from "../contexts/ServerContext";
import { useParams } from "react-router-dom";
import { QRCodeSVG } from "qrcode.react";
import { useEffect, useState } from "react";
import { WEBSITE_DOMAIN } from "../constants/constants";
import { useAuth } from "../contexts/AuthContext";
import { PublicPlayer } from "../common/types";

export const Lobby = () => {
    const game_id = useParams().game_id?.toLowerCase();
    const { user_id } = useAuth();
    const { startGame, game, loadGame, updateGameName } = useServer();
    const navigate = useNavigate();
    
    const [isEditingName, setIsEditingName] = useState(false);
    const [editingName, setEditingName] = useState('');
    
    console.log(game_id);
    
    // Automatically navigate when game status is no longer waiting
    useEffect(() => {
        if (game && game.status !== 'waiting') {
            loadGame(game_id!).then(() => navigate(`/game/${game_id}`));
        }
    }, [game?.status, game_id, loadGame, navigate]);
    
    if (!game) {
        return <div>Loading...</div>;
    }

    const qrUrl = `www.${WEBSITE_DOMAIN}/${game_id}`.toUpperCase();

    const handleStartEditing = () => {
        setIsEditingName(true);
        setEditingName(game.name);
    };

    const handleSaveName = () => {
        if (editingName.trim() && editingName.trim() !== game.name) {
            // Fire and forget - optimistic update handles UI immediately
            updateGameName(game_id!, editingName.trim()).catch(error => {
                console.error('Failed to update game name:', error);
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
        } else if (e.key === 'Escape') {
            handleCancelEdit();
        }
    };

    return (
        <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', height: '100%', width: '100%' }}>
            <input
                type="text"
                value={isEditingName ? editingName : game.name}
                onChange={(e) => setEditingName(e.target.value)}
                onBlur={isEditingName ? handleSaveName : undefined}
                onKeyDown={handleKeyDown}
                onClick={!isEditingName ? handleStartEditing : undefined}
                autoFocus={isEditingName}
                readOnly={!isEditingName}
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
                }}
                title={!isEditingName ? "Click to edit game name" : undefined}
            />
            <h2>Game ID: {game_id}</h2>
            <div style={{ marginBottom: '20px' }}>
                <QRCodeSVG value={qrUrl} size={200} fgColor="rgb(152, 38, 33)" bgColor="rgb(255, 255, 255)" />
            </div>
            {
                game.players.map((player: PublicPlayer) => (
                    <div key={player.id} style={{ display: 'flex', flexDirection: 'row', gap: '10px', color: 'white' }}>
                        <p>{player.name}</p>
                        <p>{player.status !== 'idle' ? '🟢' :
                            player.id === user_id ? <button onClick={() => {
                                startGame(game_id!);
                            }}>Ready</button> : '🔴'}</p>
                    </div>
                ))
            }
        </div>
    );
};