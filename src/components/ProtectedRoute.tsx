import React from 'react';
import { useRouter } from 'next/navigation';
import { useAuth } from '../contexts/AuthContext';
import { ServerProvider } from '../contexts/ServerContext';
import { useEffect } from 'react';
import { DragProvider } from '../contexts/DragContext';
import { GameProvider } from '../contexts/GameContext';
import { AnimationProvider } from '../contexts/AnimationContext';
import { FernFractalProvider } from '../utils/fernFractal';

// Wrapper component that protects routes and provides ServerContext
export const ProtectedRoute = ({ children }: { children: React.ReactNode }) => {
    const { user_id, loading, setRedirectAfterLogin } = useAuth();
    const router = useRouter();

    // When unauthenticated, remember where the user was headed and send them
    // to the welcome page. (Reading window.location in the effect keeps this
    // client-only and avoids Next's useSearchParams Suspense requirement.)
    useEffect(() => {
        if (!loading && !user_id) {
            const redirectUrl = window.location.pathname + window.location.search;
            setRedirectAfterLogin(redirectUrl);
            router.replace('/');
        }
    }, [loading, user_id, setRedirectAfterLogin, router]);

    if (loading) {
        return (
            <div className="auth-container">
                <div className="auth-card">
                    <h2>Loading ProtectedRoute...</h2>
                </div>
            </div>
        );
    }

    // Only allow access if user is authenticated; otherwise render nothing
    // while the effect above redirects to the welcome page.
    if (!user_id) {
        return null;
    }

    return (
        <ServerProvider>
            <FernFractalProvider>
                    <AnimationProvider>
                        <GameProvider>
                            <DragProvider>
                                {children}
                            </DragProvider>
                        </GameProvider>
                    </AnimationProvider>
            </FernFractalProvider>
        </ServerProvider>
    );
};