    // this will listen on a different channel than the main game one. I think
// or at least the UI is different enough we can have a different route
import { useServer } from "../contexts/ServerContext";

export const Lobby = () => {
    const { game_id } = useServer();
    console.log(game_id);
    return (
        <div>
            <h1>Lobby</h1>
        </div>
    );
};