import { useState } from "react";
import { useServer } from "../contexts/ServerContext";

export const Dashboard = () => {
    const [gameId, setGameId] = useState<string>('');
    const { createGame, joinGame } = useServer();
    return (
        <div>
            <h1>Dashboard</h1>
            <input type="text" value={gameId} onChange={(e) => setGameId(e.target.value)} />
            <button onClick={() => {
                joinGame(gameId);
            }}>
                Join Game
            </button>
            <button onClick={() => {
                createGame();
            }}>
                Create Game
            </button>
        </div>
    );
};