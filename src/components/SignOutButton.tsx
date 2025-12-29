import { useNavigate } from 'react-router-dom';
import { useIconButtonStyle } from '../hooks/useIconButtonStyle';
import { useAuth } from '../contexts/AuthContext';

export const SignOutButton = () => {
    const navigate = useNavigate();
    const { signOut } = useAuth();
    const { buttonBaseStyle, woodButtonStyle, woodButtonHoverStyle, iconStyle } = useIconButtonStyle();

    const handleSignOut = async () => {
        try {
            await signOut();
            navigate('/');
        } catch (error) {
            console.error('Sign out failed:', error);
            // Navigate anyway since local state is cleared
            navigate('/');
        }
    };

    return (
        <button
            onClick={handleSignOut}
            style={{
                ...buttonBaseStyle,
                right: '10px',
                overflow: 'visible',
            }}
            onMouseEnter={(e) => {
                Object.assign(e.currentTarget.style, woodButtonHoverStyle);
            }}
            onMouseLeave={(e) => {
                Object.assign(e.currentTarget.style, woodButtonStyle);
            }}
            title="Sign Out"
        >
            {/* Arrow pointing out of box icon - using positioned elements for cross-browser consistency */}
            <div style={{
                position: 'relative',
                width: '24px',
                height: '24px',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
            }}>
                {/* Box/Door frame */}
                <div style={{
                    position: 'absolute',
                    width: '14px',
                    height: '18px',
                    border: `3px solid ${iconStyle.color}`,
                    borderRight: 'none',
                    left: '0',
                }} />
                
                {/* Arrow shaft */}
                <div style={{
                    position: 'absolute',
                    width: '14px',
                    height: '3px',
                    backgroundColor: iconStyle.color,
                    right: '0',
                }} />
                
                {/* Arrow head - top part */}
                <div style={{
                    position: 'absolute',
                    width: '0',
                    height: '0',
                    borderLeft: `6px solid ${iconStyle.color}`,
                    borderTop: '4px solid transparent',
                    borderBottom: '4px solid transparent',
                    right: '-2px',
                }} />
            </div>
        </button>
    );
};

