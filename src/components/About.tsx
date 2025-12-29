import { useWoodStyle } from './WoodTexture';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { LanguageSwitcher } from './LanguageSwitcher';
import { Text } from './Text';
import { useLocalization } from '../contexts/LocalizationContext';
import { Link } from 'react-router-dom';
import { useMemo } from 'react';

export const About = () => {
    const { t } = useLocalization();
    const woodStyleBase = useWoodStyle(0.5);

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
        textDecoration: 'none',
        display: 'inline-block',
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

    return (
        <div style={{ 
            display: 'flex', 
            flexDirection: 'column', 
            alignItems: 'center', 
            justifyContent: 'center', 
            minHeight: '100vh', 
            width: '100vw', 
            position: 'relative',
            backgroundColor: '#ad826e',
            padding: '40px 20px'
        }}>
            <WoolBackgroundLayer />
            <div style={{
                maxWidth: '800px',
                width: '100%',
                position: 'relative',
                zIndex: 10,
                display: 'flex',
                flexDirection: 'column',
                alignItems: 'center',
                gap: '20px'
            }}>
                <h1 style={{ 
                    fontSize: '48px', 
                    fontWeight: 'bold', 
                    color: '#B22222',
                    textShadow: '0 4px 8px rgba(0, 0, 0, 0.4), 0 2px 4px rgba(0, 0, 0, 0.3)',
                    margin: '0',
                    textAlign: 'center'
                }}>
                    <Text id="about_foolish" />
                </h1>

                <div style={{
                    backgroundColor: 'rgba(255, 255, 255, 0.9)',
                    padding: '20px',
                    border: '3px solid #5D3A1A',
                    boxShadow: '0 4px 12px rgba(0, 0, 0, 0.3)',
                    lineHeight: '1.5',
                    fontSize: '16px',
                    color: '#333'
                }}>
                    <p style={{ marginTop: 0 }}>
                        <Text id="about_paragraph_1" />
                    </p>
                    <p>
                        <Text id="about_paragraph_2" />
                    </p>
                    <p style={{ marginBottom: 0 }}>
                        <Text id="about_paragraph_3" />
                    </p>
                </div>

                <Link 
                    to="/"
                    style={woodButtonStyle}
                    onMouseEnter={(e) => {
                        Object.assign(e.currentTarget.style, woodButtonHoverStyle);
                    }}
                    onMouseLeave={(e) => {
                        Object.assign(e.currentTarget.style, woodButtonStyle);
                    }}
                >
                    <span style={{
                        color: '#000',
                    }}>
                        <Text id="back_to_home" />
                    </span>
                </Link>
            </div>
            <LanguageSwitcher />
        </div>
    );
};

