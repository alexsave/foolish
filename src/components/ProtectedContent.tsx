import { Route, Routes } from "react-router-dom";
import { Navigate } from 'react-router-dom';
import { Dashboard } from './Dashboard';
import { Lobby } from './Lobby';
import { GameDisplay } from './GameDisplay';
import { useAuth } from '../contexts/AuthContext';

// Need to be logged in from these. This combines ProtectedRoute + AppContent from amgi
export const ProtectedContent = () => {
    const { user, loading } = useAuth();
    if (loading) {
        return (
            <div className="auth-container">
                <div className="auth-card">
                    <h2>Loading...</h2>
                </div>
            </div>
        );
    }
    if (loading) {
        return <div>Loading...</div>;
    }
    // Only allow access if user is authenticated
    if (!user) {
        return <Navigate to="/" />;
    }

    return <Routes>
        <Route path="/dashboard" element={<Dashboard />} />
        <Route path="/:game_id" element={<Lobby />} />
        <Route path="/game/:game_id" element={<GameDisplay />} />
    </Routes>
};