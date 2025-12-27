import { useState, useMemo } from 'react';
import { useAuth } from '../contexts/AuthContext';
import { useWoodStyle } from './WoodTexture';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { LanguageSwitcher } from './LanguageSwitcher';
import { Text } from './Text';
import { useLocalization } from '../contexts/LocalizationContext';
import { Link } from 'react-router-dom';

export const Welcome = () => {
    const [name, setName] = useState('');
    const [password, setPassword] = useState('');
    const { signIn, signUp } = useAuth();
    const { t } = useLocalization();
    
    const woodStyleBase = useWoodStyle(0.5);
    const woodStyleBase2 = useWoodStyle(0.9);

    const woodButtonStyle: React.CSSProperties = useMemo(() => ({
        ...woodStyleBase,
        border: '3px solid #5D3A1A',
        borderRadius: '0',
        fontWeight: 'bold',
        cursor: 'pointer',
        transition: 'all 0.2s ease',
        boxShadow: `
            inset 0 1px 0 rgba(255,255,255,0.2),
            inset 0 -1px 0 rgba(0,0,0,0.3),
            0 2px 4px rgba(0,0,0,0.4)`,
        position: 'relative' as const,
        overflow: 'hidden' as const,
        padding: '10px 20px',
        fontSize: '16px',
        mixBlendMode: 'normal' as const,
    }), [woodStyleBase]);

    const woodButtonHoverStyle: React.CSSProperties = useMemo(() => ({
        ...woodStyleBase,
        filter: 'brightness(1.1) contrast(1.1)',
        transform: 'translateY(-1px)',
        boxShadow: `
            inset 0 2px 0 rgba(255,255,255,0.3),
            inset 0 -2px 0 rgba(0,0,0,0.4),
            0 4px 8px rgba(0,0,0,0.5)`,
        mixBlendMode: 'normal' as const,
    }), [woodStyleBase]);

    const woodButtonStyle2: React.CSSProperties = useMemo(() => ({
        ...woodStyleBase2,
        border: '3px solid #5D3A1A',
        borderRadius: '0',
        fontWeight: 'bold',
        cursor: 'pointer',
        transition: 'all 0.2s ease',
        boxShadow: `
            inset 0 1px 0 rgba(255,255,255,0.2),
            inset 0 -1px 0 rgba(0,0,0,0.3),
            0 2px 4px rgba(0,0,0,0.4)`,
        position: 'relative' as const,
        overflow: 'hidden' as const,
        padding: '10px 20px',
        fontSize: '16px',
        mixBlendMode: 'normal' as const,
    }), [woodStyleBase2]);

    const woodButtonHoverStyle2: React.CSSProperties = useMemo(() => ({
        ...woodStyleBase2,
        filter: 'brightness(1.1) contrast(1.1)',
        transform: 'translateY(-1px)',
        boxShadow: `
            inset 0 2px 0 rgba(255,255,255,0.3),
            inset 0 -2px 0 rgba(0,0,0,0.4),
            0 4px 8px rgba(0,0,0,0.5)`,
        mixBlendMode: 'normal' as const,
    }), [woodStyleBase2]);

    const handleLogin = async (e: React.FormEvent) => {
        e.preventDefault();
        try {
            const { weakPassword } = await signIn(name, password);
            if (weakPassword) {
                alert('Weak password');
            }
        } catch (error: any) {
            alert(error.message);
        }
    };

    const handleSignUp = async (e: React.FormEvent) => {
        e.preventDefault();
        try {
            await signUp(name, password);
        } catch (error: any) {
            alert(error.message);
        }
    };

    return (
        <div style={{ 
            display: 'flex', 
            flexDirection: 'column', 
            alignItems: 'center', 
            justifyContent: 'center', 
            height: '100vh', 
            width: '100vw', 
            position: 'relative',
            backgroundColor: '#ad826e' 
        }}>
            <WoolBackgroundLayer />
            <p style={{ 
                fontSize: '72px', 
                fontWeight: 'bold', 
                color: '#B22222',
                textShadow: '0 4px 8px rgba(0, 0, 0, 0.4), 0 2px 4px rgba(0, 0, 0, 0.3)',
                marginBottom: '30px',
                position: 'relative',
                zIndex: 10
            }}><Text id="foolish" /></p>
            <form onSubmit={handleLogin} style={{ 
                display: 'flex', 
                flexDirection: 'column', 
                alignItems: 'center', 
                gap: '15px',
                position: 'relative',
                zIndex: 10
            }}>
                <div style={{ display: 'flex', flexDirection: 'column', gap: '5px' }}>
                    <label htmlFor="username" style={{ color: '#333', fontWeight: 'bold' }}><Text id="username" />:</label>
                    <input
                        id="username"
                        type="text"
                        value={name}
                        onChange={(e) => setName(e.target.value)}
                        autoComplete="username"
                        required
                        style={{
                            padding: '8px 12px',
                            fontSize: '16px',
                            border: '2px solid #5D3A1A',
                            borderRadius: '0',
                            minWidth: '250px'
                        }}
                    />
                </div>
                <div style={{ display: 'flex', flexDirection: 'column', gap: '5px' }}>
                    <label htmlFor="password" style={{ color: '#333', fontWeight: 'bold' }}><Text id="password" />:</label>
                    <input
                        id="password"
                        type="password"
                        value={password}
                        onChange={(e) => setPassword(e.target.value)}
                        autoComplete="current-password"
                        required
                        style={{
                            padding: '8px 12px',
                            fontSize: '16px',
                            border: '2px solid #5D3A1A',
                            borderRadius: '0',
                            minWidth: '250px'
                        }}
                    />
                </div>
                <div style={{ display: 'flex', gap: '10px', marginTop: '10px' }}>
                    <button 
                        type="submit"
                        style={woodButtonStyle}
                        onMouseEnter={(e) => {
                            Object.assign(e.currentTarget.style, woodButtonHoverStyle);
                        }}
                        onMouseLeave={(e) => {
                            Object.assign(e.currentTarget.style, woodButtonStyle);
                        }}
                    >
                        <span style={{
                            color: 'rgba(70, 35, 20, 0.8)',
                            mixBlendMode: 'color-burn',
                            filter: 'contrast(1.2) brightness(0.9)',
                        }}><Text id="login" /></span>
                    </button>
                    <button 
                        type="button"
                        onClick={handleSignUp}
                        style={woodButtonStyle2}
                        onMouseEnter={(e) => {
                            Object.assign(e.currentTarget.style, woodButtonHoverStyle2);
                        }}
                        onMouseLeave={(e) => {
                            Object.assign(e.currentTarget.style, woodButtonStyle2);
                        }}
                    >
                        <span style={{
                            color: 'rgba(70, 35, 20, 0.8)',
                            mixBlendMode: 'color-burn',
                            filter: 'contrast(1.2) brightness(0.9)',
                        }}><Text id="sign_up" /></span>
                    </button>
                </div>
            </form>
            <LanguageSwitcher />
            <Link 
                to="/about"
                style={{
                    position: 'absolute',
                    bottom: '20px',
                    left: '50%',
                    transform: 'translateX(-50%)',
                    color: '#5D3A1A',
                    textDecoration: 'underline',
                    fontSize: '14px',
                    cursor: 'pointer',
                    zIndex: 10,
                    textShadow: '0 1px 2px rgba(255, 255, 255, 0.5)'
                }}
            >
                <Text id="about" />
            </Link>
        </div>
    );
};