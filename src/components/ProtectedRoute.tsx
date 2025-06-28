import { Navigate } from 'react-router-dom';
import { useAuth } from '../contexts/AuthContext';
import { ServerProvider } from '../contexts/ServerContext';

// Wrapper component that protects routes and provides ServerContext
export const ProtectedRoute = ({ children }: { children: React.ReactNode }) => {
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

    return (
        <ServerProvider>
            {children}
        </ServerProvider>
    );
};