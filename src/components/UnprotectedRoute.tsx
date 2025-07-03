//the oppsoite of protected route
import { Navigate } from 'react-router-dom';
import { useAuth } from '../contexts/AuthContext';
import { ServerProvider } from '../contexts/ServerContext';
import { useEffect, useState } from 'react';

// Wrapper component that protects routes and provides ServerContext
export const UnprotectedRoute = ({ children }: { children: React.ReactNode }) => {
    const { user, loading, redirectAfterLogin, clearRedirectAfterLogin } = useAuth();
    const [shouldRedirect, setShouldRedirect] = useState<string | null>(null);
    const [hasHandledRedirect, setHasHandledRedirect] = useState(false);
    
    // Handle redirect logic when user is authenticated
    useEffect(() => {
        if (!loading && user && !hasHandledRedirect) {
            if (redirectAfterLogin) {
                // Store the destination and clear the redirect URL
                setShouldRedirect(redirectAfterLogin);
                clearRedirectAfterLogin();
                setHasHandledRedirect(true);
            } else {
                // Default redirect to dashboard
                setShouldRedirect('/dashboard');
                setHasHandledRedirect(true);
            }
        }
    }, [loading, user, redirectAfterLogin, hasHandledRedirect, clearRedirectAfterLogin]);
    
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
    
    // If user is authenticated, redirect to intended destination or dashboard
    if (user && shouldRedirect) {
        return <Navigate to={shouldRedirect} replace />;
    }

    return (
        <ServerProvider>
            {children}
        </ServerProvider>
    );
};