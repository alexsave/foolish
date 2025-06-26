// this will listen on a different channel than the main game one. I think
import { useNavigate } from "react-router-dom";
// or at least the UI is different enough we can have a different route
import { useServer } from "../contexts/ServerContext";
import { useParams } from "react-router-dom";
import { QRCodeSVG } from "qrcode.react";

export const Lobby = () => {
    const { game_id } = useParams();
    const { startGame, game, player_id } = useServer();
    const navigate = useNavigate();
    console.log(game_id);
    if (!game) {
        return <div>Loading...</div>;
    }
    return (
        <div>
            <h1>Lobby</h1>
            <h2>Game ID: {game_id}</h2>
            <div style={{ marginBottom: '20px' }}>
                <h3>Join via QR Code:</h3>
                <QRCodeSVG value={`www.foolish.cards/${game_id}`} size={200} />
            </div>
            {
                game.players.map(player => (
                    <div key={player.id} style={{ display: 'flex', flexDirection: 'row', gap: '10px', color: 'white', backgroundColor: 'black' }}>
                        <p>{player.name}</p>
                        <p>{player.status !== 'idle' ? '🟢' : 
                        player.id === player_id ? <button onClick={() => {
                            startGame(game_id!);
                        }}>Ready</button> : '🔴'}</p>
                    </div>
                ))
            }
            <button disabled={game.status === 'waiting'} onClick={() => {
                navigate(`/game/${game_id}`);
            }}>Enter Game</button>
        </div>
    );
};