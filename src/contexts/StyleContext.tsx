import React, { createContext, useContext, useMemo } from 'react';
import { useTheme } from './ThemeContext';

// Card back styles
interface CardBackStyles {
    backgroundColor: string;
    borderColor: string;
    borderWidth: string;
    borderRadius: string;
    boxShadow: string | undefined;
    usePattern: boolean;
}

// Mini card styles (PlayerRing)
interface MiniCardStyles {
    backgroundColor: string;
    borderRadius: string;
    border: string;
    boxShadow: string;
    usePattern: boolean;
    useSvgCardBack: boolean;
}

// Card in hand styles
interface CardInHandStyles {
    borderRadius: string;
    borderColor: string;
    selectedBorderColor: string;
    boxShadow: string | undefined;
}

// Text styles
interface TextStyles {
    textShadow: string;
    cardCountTextShadow: string;
}

// Icon selection
interface IconStyles {
    useEmojiIcons: boolean;
    passIcon: string;
}

// Behavioral differences
interface BehaviorStyles {
    useCustomPasswordMasking: boolean;
}

// Texture-related
interface TextureStyles {
    useWoodTexture: boolean;
}

// All styles provided by context
interface StyleContextValue {
    cardBack: CardBackStyles;
    miniCard: MiniCardStyles;
    cardInHand: CardInHandStyles;
    text: TextStyles;
    icons: IconStyles;
    behavior: BehaviorStyles;
    texture: TextureStyles;
}

const StyleContext = createContext<StyleContextValue | null>(null);

export const StyleProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
    const { isSoviet } = useTheme();

    const styles = useMemo<StyleContextValue>(() => {
        if (isSoviet) {
            return {
                cardBack: {
                    backgroundColor: '#B32929',
                    borderColor: '#0A0A0A',
                    borderWidth: '2px',
                    borderRadius: '0',
                    boxShadow: '2px 2px 0 #0A0A0A',
                    usePattern: false,
                },
                miniCard: {
                    backgroundColor: '#B32929',
                    borderRadius: '0',
                    border: '1px solid #0A0A0A',
                    boxShadow: '1px 1px 0 #0A0A0A',
                    usePattern: false,
                    useSvgCardBack: true,
                },
                cardInHand: {
                    borderRadius: '0',
                    borderColor: '#0A0A0A',
                    selectedBorderColor: '#B32929',
                    boxShadow: '2px 2px 0 #0A0A0A',
                },
                text: {
                    textShadow: 'none',
                    cardCountTextShadow: '1px 1px 0 #0A0A0A',
                },
                icons: {
                    useEmojiIcons: false,
                    passIcon: '→',
                },
                behavior: {
                    useCustomPasswordMasking: true,
                },
                texture: {
                    useWoodTexture: false,
                },
            };
        } else {
            return {
                cardBack: {
                    backgroundColor: 'rgb(180, 14, 9)',
                    borderColor: '#8B0000',
                    borderWidth: '1px',
                    borderRadius: '5px',
                    boxShadow: undefined,
                    usePattern: true,
                },
                miniCard: {
                    backgroundColor: 'rgb(180, 14, 9)',
                    borderRadius: '3px',
                    border: '1px solid #8B0000',
                    boxShadow: '0 1px 2px rgba(0,0,0,0.3)',
                    usePattern: true,
                    useSvgCardBack: false,
                },
                cardInHand: {
                    borderRadius: '5px',
                    borderColor: 'black',
                    selectedBorderColor: 'red',
                    boxShadow: undefined,
                },
                text: {
                    textShadow: 'var(--text-shadow-dark)',
                    cardCountTextShadow: '1px 1px 2px rgba(0,0,0,0.8)',
                },
                icons: {
                    useEmojiIcons: true,
                    passIcon: '🔄',
                },
                behavior: {
                    useCustomPasswordMasking: false,
                },
                texture: {
                    useWoodTexture: true,
                },
            };
        }
    }, [isSoviet]);

    return (
        <StyleContext.Provider value={styles}>
            {children}
        </StyleContext.Provider>
    );
};

export const useStyles = (): StyleContextValue => {
    const context = useContext(StyleContext);
    if (!context) {
        throw new Error('useStyles must be used within a StyleProvider');
    }
    return context;
};
