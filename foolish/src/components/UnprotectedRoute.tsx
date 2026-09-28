//the oppsoite of protected route
import { useRouter } from 'next/navigation';
import { useAuth } from '../contexts/AuthContext';
import { ServerProvider } from '../contexts/ServerContext';
import { useEffect, useState } from 'react';

// Wrapper component that protects routes and provides ServerContext
export const UnprotectedRoute = ({ children }: { children: React.ReactNode }) => {
    const { user_id, loading, redirectAfterLogin, clearRedirectAfterLogin } = useAuth();
    const router = useRouter();
    const [shouldRedirect, setShouldRedirect] = useState<string | null>(null);
    const [hasHandledRedirect, setHasHandledRedirect] = useState(false);

    // Handle redirect logic when user is authenticated
    useEffect(() => {
        if (!loading && user_id && !hasHandledRedirect) {
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
    }, [loading, user_id, redirectAfterLogin, hasHandledRedirect, clearRedirectAfterLogin]);

    // Once a destination is resolved, perform the redirect.
    useEffect(() => {
        if (shouldRedirect) {
            router.replace(shouldRedirect);
        }
    }, [shouldRedirect, router]);

    // Don't render anything until Supabase has determined the auth state.
    // Rendering Welcome prematurely causes iOS Safari to focus the username
    // input (popping the keyboard) before we redirect a signed-in user to the
    // dashboard, which is disorienting.
    if (loading) {
        return null;
    }

    // If user is authenticated, redirect to intended destination or dashboard.
    // shouldRedirect is set by the effect on the next tick; until then render
    // nothing rather than flashing Welcome to a signed-in user.
    if (user_id) {
        return null;
    }

    return (
        <ServerProvider>
            {children}
        </ServerProvider>
    );
};