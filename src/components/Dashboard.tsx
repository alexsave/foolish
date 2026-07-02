import { useEffect } from "react";
import { useRouter } from "next/navigation";
import { useServer } from "../contexts/ServerContext";
import { useAuth } from "../contexts/AuthContext";
import { TexturedSurface } from "./TexturedSurface";
import { WoolBackgroundLayer } from "./WoolBackgroundLayer";
import { SignOutButton } from "./SignOutButton";
import { LanguageSwitcher } from "./LanguageSwitcher";
import { Text } from "./Text";
import { useLocalization } from "../contexts/LocalizationContext";
import { GameCard } from "./Dashboard/GameCard";
import { JoinGameForm } from "./Dashboard/JoinGameForm";
import { CreateGameButton } from "./Dashboard/CreateGameButton";

export const Dashboard = () => {
    const router = useRouter();
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

                <div className="flex gap-sm w-full">
                    {([
                        { id: 'leaderboard', path: '/leaderboard', seed: 0.35 },
                        { id: 'match_history', path: '/history', seed: 0.85 },
                    ] as const).map((nav) => (
                        <TexturedSurface
                            key={nav.id}
                            as="button"
                            seed={nav.seed}
                            onClick={() => router.push(nav.path)}
                            className="btn-wood btn-wood--sm"
                            style={{ flex: 1 }}
                        >
                            <span className="btn-wood-text">
                                <Text id={nav.id} />
                            </span>
                        </TexturedSurface>
                    ))}
                </div>
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
