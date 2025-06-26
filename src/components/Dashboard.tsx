import { useState } from "react";
import { useServer } from "../contexts/ServerContext";
import { useNavigate } from "react-router-dom";

export const Dashboard = () => {
    const [gameId, setGameId] = useState<string>('');
    const { createGame, joinGame } = useServer();
    const navigate = useNavigate();
    return (
        <div>
            <h1>Dashboard</h1>
            <input type="text" value={gameId} onChange={(e) => setGameId(e.target.value)} />
            <button onClick={() => {
                joinGame(gameId).then(data => {
                    console.log(data);
                    navigate(`/lobby/${data.game_id}`);
                }).catch(error => {
                    alert(error);
                });
            }}>
                Join Game
            </button>
            <button onClick={() => {
                createGame().then(data => {
                    console.log(data);
                    navigate(`/lobby/${data.game_id}`);
                }).catch(error => {
                    alert(error);
                });
            }}>
                Create Game
            </button>
        </div>
    );
};