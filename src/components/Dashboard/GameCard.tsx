import { useRouter } from "next/navigation";
import { useAuth } from "../../contexts/AuthContext";
import { useTexture, getTextureStyle, seedFromString, flipFromString } from "../TexturedSurface";
import { Text } from "../Text";
import { SovietIcon, SuitIcon } from "../SovietIcon";
import { PLAYER_STATUS, GAME_STATUS, PublicGame } from "@shared/core/types.ts";

interface GameCardProps {
    game: PublicGame;
}

export const GameCard: React.FC<GameCardProps> = ({ game }) => {
    const router = useRouter();
    const { username } = useAuth();
    const { woodUrl } = useTexture();

    const isGameOver = game.status === GAME_STATUS.GAME_OVER;
    const isWaiting = game.status === GAME_STATUS.WAITING;
    const isPlaying = game.status === GAME_STATUS.PLAYING;

    const readyPlayers = game.players.filter(p => p.status === PLAYER_STATUS.READY).length;
    const totalPlayers = game.players.length;

    const gameSeed = seedFromString(game.id);
    const flip = flipFromString(game.id);

    const statusClass = isWaiting ? 'badge--waiting' : isPlaying ? 'badge--playing' : 'badge--gameover';

    return (
        <div className="game-card" onClick={() => router.push(`/${game.id}`)}>
            {/* CSS hides this in Soviet mode via [data-theme="soviet"] .bg-wood { display: none } */}
            <div 
                className="bg-wood"
                style={{
                    ...getTextureStyle(woodUrl, false, gameSeed),
                    transform: `scaleX(${flip})`,
                }} 
            />

            <div className="game-card__header">
                <div className="flex items-center gap-md flex-1">
                    <h3 className="game-card__title">{game.name}</h3>

                    {isWaiting && (
                        <span className="game-card__info">
                            <Text id="ready" />: {readyPlayers}/{totalPlayers}
                        </span>
                    )}

                    {isPlaying && (
                        <span className="game-card__info flex items-center gap-xs">
                            <Text id="deck_cards" />: {game.deck_length + (game.flipped ? 1 : 0)} <SuitIcon suit={game.power_suit} size={16} />
                        </span>
                    )}
                </div>

                <span className={`badge ${statusClass}`}>
                    {isWaiting && <Text id="waiting" />}
                    {isPlaying && <Text id="playing" />}
                    {isGameOver && <Text id="game_over" />}
                </span>
            </div>

            <div className="game-card__players">
                <div className="flex flex-wrap items-center gap-sm">
                    {game.players.map((player, idx) => {
                        const isDefender = isPlaying && game.defender === idx;
                        const isFirstAttacker = isPlaying && game.first_attacker === idx;
                        const isCurrentUser = player.name === username;

                        return (
                            <span key={idx} className="game-card__player">
                                <SovietIcon name={player.is_ai ? 'bot' : 'person'} size={14} />
                                <span className={`game-card__player-name ${isCurrentUser ? 'game-card__player-name--current' : ''}`}>
                                    {player.name}
                                </span>

                                {isWaiting && (
                                    <SovietIcon name={player.status === PLAYER_STATUS.READY ? 'ready' : 'not-ready'} size={12} />
                                )}

                                {isPlaying && (
                                    <span className="game-card__player-status">
                                        {isDefender && <SovietIcon name="shield" size={14} />}
                                        {isFirstAttacker && <SovietIcon name="sword" size={14} />}
                                        {player.hand_length > 0 && <span className="game-card__hand-count">({player.hand_length})</span>}
                                    </span>
                                )}

                                {isGameOver && player.hand_length === 0 && (
                                    <SovietIcon name="crown" size={14} />
                                )}
                            </span>
                        );
                    })}
                </div>
            </div>
        </div>
    );
};
