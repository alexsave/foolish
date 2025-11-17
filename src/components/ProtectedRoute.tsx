import React, { Suspense, lazy } from 'react';
import { Navigate, useLocation } from 'react-router-dom';
import { useAuth } from '../contexts/AuthContext';
import { ServerProvider } from '../contexts/ServerContext';
import { useEffect } from 'react';
import { DragProvider } from '../contexts/DragContext';
import { GameProvider } from '../contexts/GameContext';
import { AnimationProvider } from '../contexts/AnimationContext';

const FernFractalProvider = lazy(() => 
  import('../utils/fernFractal').then(m => ({ default: m.FernFractalProvider }))
);

// Wrapper component that protects routes and provides ServerContext
export const ProtectedRoute = ({ children }: { children: React.ReactNode }) => {
    const { user_id, loading, setRedirectAfterLogin } = useAuth();
    const location = useLocation();

    // Store redirect URL when user is not authenticated
    useEffect(() => {
        if (!loading && !user_id) {
            const redirectUrl = location.pathname + location.search;
            setRedirectAfterLogin(redirectUrl);
        }
    }, [loading, user_id, location.pathname, location.search, setRedirectAfterLogin]);

    if (loading) {
        return (
            <div className="auth-container">
                <div className="auth-card">
                    <h2>Loading ProtectedRoute...</h2>
                </div>
            </div>
        );
    }

    // Only allow access if user is authenticated
    if (!user_id) {
        return <Navigate to="/login" />;
    }

    return (
        <ServerProvider>
            <Suspense fallback={<div>Loading textures...</div>}>
                <FernFractalProvider>
                    <AnimationProvider>
                        <GameProvider>
                            <DragProvider>
                                {children}
                            </DragProvider>
                        </GameProvider>
                    </AnimationProvider>
                </FernFractalProvider>
            </Suspense>
        </ServerProvider>
    );
};