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

    // Load all games when Dashboard mounts
    useEffect(() => {
        getUserGames();
    }, []);

    return (
        <div style={{
            display: 'flex',
            flexDirection: 'column',
            alignItems: 'center',
            height: '100%',
            width: '100%',
            minHeight: '100vh',
            position: 'relative',
            backgroundColor: '#ad826e'
        }}>
        <WoolBackgroundLayer />
        <SignOutButton />

            {/* Title */}
        <h1 style={{
            color: 'white',
            fontSize: '1.3rem',
            fontWeight: 'bold',
            marginBottom: '0.75rem',
            textAlign: 'center',
            position: 'relative',
                zIndex: 10,
                textShadow: '1px 1px 2px rgba(0,0,0,0.8)'
        }}>
            {t('dashboard_title', { username: username || '' })}
        </h1>

            {/* Action buttons section */}
        <div style={{
            display: 'flex',
            flexDirection: 'column',
            gap: '0.5rem',
            alignItems: 'center',
            marginBottom: '0.75rem',
            width: '300px',
            position: 'relative',
            zIndex: 10
        }}>
                <JoinGameForm />
                <CreateGameButton />
        </div>

            {/* Games list */}
            <div style={{
                width: '100%',
                maxWidth: '95vw',
                flex: '1',
                position: 'relative',
                zIndex: 10
            }}>
            <div style={{
                display: 'flex',
                flexDirection: 'column',
                gap: '6px',
                alignItems: 'center'
            }}>
                    {Object.values(games).map((game) => (
                        <GameCard key={game.id} game={game} />
                    ))}

                {Object.keys(games).length === 0 && (
                    <div style={{
                        color: 'white',
                        textAlign: 'center',
                        padding: '2rem',
                            fontSize: '1.1rem',
                            textShadow: '1px 1px 2px rgba(0,0,0,0.8)'
                    }}>
                        <Text id="no_games_available" />
                    </div>
                )}
            </div>
        </div>

        <LanguageSwitcher />
        </div>
    );
};
