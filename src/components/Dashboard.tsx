import { useState } from "react";
import supabase from '../backend/Connector';
import { useServer } from "../contexts/ServerContext";
import { useNavigate } from "react-router-dom";
import { useAuth } from "../contexts/AuthContext";

export const Dashboard = () => {
    const [gameId, setGameId] = useState<string>('');
    const { username } = useAuth();
    const { joinGame, games } = useServer();
    const navigate = useNavigate();

    return <div>
        <h1>Dashboard for {username}</h1>
        <input type="text" value={gameId} onChange={(e) => setGameId(e.target.value)} />
        <button onClick={() => {
            joinGame(gameId.toLowerCase()).then(data => {
                console.log(data);
                navigate(`/${data.game_id}`);
            }).catch(error => {
                alert(error);
            });
        }}>
            Join Game
        </button>
        <button onClick={() => {
            // Just call it directly for now, i dont care
            supabase.functions.invoke('create')
                .then(data => {
                    console.log(data);
                    navigate(`/${data.data.id}`);
                }).catch(error => {
                    alert(error);
                });
        }}>
            Create Game
        </button>
        <p>Games:</p>
        {Object.values(games).map((game) => (
            <div key={game.id} style={{ border: '1px solid black', cursor: 'pointer' }} onClick={() => {
                navigate(`/${game.id}`);
            }}>
                <p>{game.name}</p>
                <p>{game.status}</p>
                <p>{game.players.map(player => player.name).join(', ')}</p>
            </div>
        ))}
    </div>;
};