//the oppsoite of protected route
import { Navigate } from 'react-router-dom';
import { useAuth } from '../contexts/AuthContext';
import { ServerProvider } from '../contexts/ServerContext';

// Wrapper component that protects routes and provides ServerContext
export const UnprotectedRoute = ({ children }: { children: React.ReactNode }) => {
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
    // If use is authenticated, redirect to dashboard
    if (user) {
        return <Navigate to="/dashboard" />;
    }

    return (
        <ServerProvider>
            {children}
        </ServerProvider>
    );
};