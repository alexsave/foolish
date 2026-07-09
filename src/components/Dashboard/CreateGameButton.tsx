import { useState } from "react";
import { useRouter } from "next/navigation";
import { useServerActions } from "../../contexts/ServerContext";
import { TexturedSurface } from "../TexturedSurface";
import { Text } from "../Text";
import { GameLoadingPlaceholder } from "../GameLoadingPlaceholder";

export const CreateGameButton: React.FC = () => {
    const router = useRouter();
    const { createGame } = useServerActions();
    // `creating` does double duty: it guards against the button being mashed into
    // spawning multiple games (the create round-trip is a few hundred ms of dead
    // time otherwise), and it drives the optimistic full-screen placeholder so the
    // click feels instant.
    const [creating, setCreating] = useState(false);

    const handleCreate = () => {
        if (creating) return;            // ignore repeat taps while in flight
        setCreating(true);
        // Go through ServerContext.createGame, NOT a raw functions.invoke: `create`
        // returns a PACKED binary view buffer (the game is persisted in the
        // background after responding), and createGame decodes it with the shared
        // codec. Reading `data.id` off the raw Blob response would always be
        // undefined — the game still gets created server-side, but the client would
        // wrongly report failure.
        createGame()
            .then(({ game_id }) => {
                // Navigate to the new game. Leave the overlay up: this component
                // unmounts on navigation, and GameView shows the same placeholder
                // until the game state loads, so there's no blank gap.
                router.push(`/${game_id}`);
            })
            .catch((error) => {
                setCreating(false);
                alert(error?.message || 'Could not create game. Please try again.');
            });
    };

    return (
        <>
            <TexturedSurface
                as="button"
                seed={0.7}
                onClick={handleCreate}
                disabled={creating}
                aria-busy={creating}
                className="btn-wood btn-wood--sm btn-wood--full"
            >
                <span className="btn-wood-text">
                    <Text id="create_new_game" />
                </span>
            </TexturedSurface>

            {creating && (
                <div style={{ position: 'fixed', inset: 0, zIndex: 9999 }}>
                    <GameLoadingPlaceholder />
                </div>
            )}
        </>
    );
};
