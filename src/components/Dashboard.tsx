import { useEffect } from "react";
import { useServer } from "../contexts/ServerContext";
import { useAuth } from "../contexts/AuthContext";
import { WoolBackgroundLayer } from "./WoolBackgroundLayer";
import { SignOutButton } from "./SignOutButton";
import { LanguageSwitcher } from "./LanguageSwitcher";
import { Text } from "./Text";
import { useLocalization } from "../contexts/LocalizationContext";
import { GameCard } from "./Dashboard/GameCard";
import { JoinGameForm } from "./Dashboard/JoinGameForm";
import { CreateGameButton } from "./Dashboard/CreateGameButton";

export const Dashboard = () => {
    const { username } = useAuth();
    const { games, getUserGames } = useServer();
    const { t } = useLocalization();

    useEffect(() => {
        getUserGames();
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    return (
        <div className="page page--full-height">
        <WoolBackgroundLayer />
        <SignOutButton />

            <h1 className="title title--section">
            {t('dashboard_title', { username: username || '' })}
        </h1>

            <div className="flex flex-col items-center gap-sm mb-md z-content" style={{ width: '300px' }}>
                <JoinGameForm />
                <CreateGameButton />
        </div>

            <div className="w-full flex-1 z-content" style={{ maxWidth: '95vw' }}>
                <div className="flex flex-col items-center" style={{ gap: '6px' }}>
                    {Object.values(games).map((game) => (
                        <GameCard key={game.id} game={game} />
                    ))}

                {Object.keys(games).length === 0 && (
                        <div className="empty-state">
                        <Text id="no_games_available" />
                    </div>
                )}
            </div>
        </div>

        <LanguageSwitcher />
        </div>
    );
};
