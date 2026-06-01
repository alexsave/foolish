import { useRouter } from 'next/navigation';
import { useAuth } from '../contexts/AuthContext';
import { TexturedSurface } from './TexturedSurface';

export const SignOutButton = () => {
    const router = useRouter();
    const { signOut } = useAuth();

    const handleSignOut = async () => {
        try {
            await signOut();
            router.push('/');
        } catch (error) {
            console.error('Sign out failed:', error);
            router.push('/');
        }
    };

    return (
        <TexturedSurface
            as="button"
            seed={0.2}
            className="btn-icon btn-icon--right"
            onClick={handleSignOut}
            style={{ overflow: 'visible' }}
            title="Sign Out"
        >
            <div className="flex flex-center relative" style={{ width: '24px', height: '24px' }}>
                <div className="absolute" style={{
                    width: '14px',
                    height: '18px',
                    border: '3px solid var(--color-text-dark)',
                    borderRight: 'none',
                    left: '0',
                }} />
                <div className="absolute" style={{
                    width: '14px',
                    height: '3px',
                    backgroundColor: 'var(--color-text-dark)',
                    right: '0',
                }} />
                <div className="absolute" style={{
                    width: '0',
                    height: '0',
                    borderLeft: '6px solid var(--color-text-dark)',
                    borderTop: '4px solid transparent',
                    borderBottom: '4px solid transparent',
                    right: '-2px',
                }} />
            </div>
        </TexturedSurface>
    );
};
