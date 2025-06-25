    // this will listen on a different channel than the main game one. I think
// or at least the UI is different enough we can have a different route
import { useServer } from "../contexts/ServerContext";
import { useParams } from "react-router-dom";

export const Lobby = () => {
    const { game_id } = useParams();
    console.log(game_id);
    return (
        <div>
            <h1>Lobby</h1>
            <h2>Game ID: {game_id}</h2>
        </div>
    );
};