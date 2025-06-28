import { useState } from "react";
import supabase from '../backend/Connector';
import { useServer } from "../contexts/ServerContext";
import { useNavigate } from "react-router-dom";
import { useAuth } from "../contexts/AuthContext";

export const Dashboard = () => {
    const [gameId, setGameId] = useState<string>('');
    const { user } = useAuth();
    const { createGame, joinGame } = useServer();
    const navigate = useNavigate();
    return (
        <div>
            <h1>Dashboard for {user}</h1>
            <input type="text" value={gameId} onChange={(e) => setGameId(e.target.value)} />
            <button onClick={() => {
                joinGame(gameId).then(data => {
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
                        navigate(`/${data.data.game.id}`);
                    }).catch(error => {
                        alert(error);
                    });
            }}>
                Create Game
            </button>
        </div>
    );
};