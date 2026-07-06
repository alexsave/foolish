import { useState } from "react";
import { useRouter } from "next/navigation";
import { useServerActions } from "../../contexts/ServerContext";
import { TexturedSurface } from "../TexturedSurface";
import { Text } from "../Text";
import { useLocalization } from "../../contexts/LocalizationContext";

export const JoinGameForm: React.FC = () => {
    const [gameId, setGameId] = useState<string>('');
    const { joinGame } = useServerActions();
    const router = useRouter();
    const { t } = useLocalization();

    const handleSubmit = (e: React.FormEvent) => {
        e.preventDefault();
        if (gameId.trim()) {
            joinGame(gameId.toLowerCase()).then(data => {
                router.push(`/${data.game_id}`);
            }).catch(error => {
                alert(error);
            });
        }
    };

    return (
        <form onSubmit={handleSubmit} className="flex items-center justify-between w-full">
            <TexturedSurface
                as="input"
                seed={0.3}
                type="text"
                value={gameId}
                onChange={(e: React.ChangeEvent<HTMLInputElement>) => setGameId(e.target.value)}
                placeholder={t('enter_game_id')}
                inputMode="text"
                className="input-wood"
                style={{ textAlign: 'center', width: '50%' }}
            />
            <TexturedSurface
                as="button"
                seed={0.1}
                type="submit"
                disabled={!gameId.trim()}
                className="btn-wood btn-wood--sm"
                style={{
                    width: '50%',
                    marginLeft: '0.5rem',
                    opacity: gameId.trim() ? 1 : 0.6,
                }}
            >
                <span className="btn-wood-text">
                    <Text id="join" />
                </span>
            </TexturedSurface>
        </form>
    );
};
