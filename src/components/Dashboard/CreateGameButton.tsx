import { useState } from "react";
import { useRouter } from "next/navigation";
import supabase from "../../backend/Connector";
import { TexturedSurface } from "../TexturedSurface";
import { Text } from "../Text";
import { GameLoadingPlaceholder } from "../GameLoadingPlaceholder";

export const CreateGameButton: React.FC = () => {
    const router = useRouter();
    // `creating` does double duty: it guards against the button being mashed into
    // spawning multiple games (the create round-trip is a few hundred ms of dead
    // time otherwise), and it drives the optimistic full-screen placeholder so the
    // click feels instant.
    const [creating, setCreating] = useState(false);

    const handleCreate = () => {
        if (creating) return;            // ignore repeat taps while in flight
        setCreating(true);
        supabase.functions.invoke('create')
            .then(({ data, error }) => {
                const id = data?.id;
                if (error || !id) {
                    setCreating(false);
                    alert(error?.message || 'Could not create game. Please try again.');
                    return;
                }
                // Navigate to the new game. Leave the overlay up: this component
                // unmounts on navigation, and GameView shows the same placeholder
                // until the game state loads, so there's no blank gap.
                router.push(`/${id}`);
            })
            .catch((error) => {
                setCreating(false);
                alert(error);
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
