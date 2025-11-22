import React from 'react';

/**
 * ELO Display Utility
 * 
 * Color scheme inspired by popular competitive games:
 * - Bronze: 0-999 (below starting ELO)
 * - Silver: 1000-1199 (beginner)
 * - Gold: 1200-1399 (intermediate)
 * - Platinum: 1400-1599 (advanced)
 * - Diamond: 1600-1799 (expert)
 * - Master: 1800-1999 (master)
 * - Grandmaster: 2000-2199 (grandmaster)
 * - Legend: 2200+ (legendary - with animations)
 */

export interface EloTier {
    name: string;
    minElo: number;
    color: string;
    gradient?: string;
    animated: boolean;
}

export const ELO_TIERS: EloTier[] = [
    { name: 'Bronze', minElo: 0, color: '#CD7F32', animated: false },
    { name: 'Silver', minElo: 1000, color: '#C0C0C0', animated: false },
    { name: 'Gold', minElo: 1200, color: '#FFD700', animated: false },
    { name: 'Platinum', minElo: 1400, color: '#4DD0E1', animated: false },
    { name: 'Diamond', minElo: 1600, color: '#2196F3', animated: false },
    { name: 'Master', minElo: 1800, color: '#9C27B0', animated: false },
    { name: 'Grandmaster', minElo: 2000, color: '#FF5722', animated: false },
    { name: 'Legend', minElo: 2200, color: '#FFD700', gradient: 'linear-gradient(90deg, #ff0000, #ff7f00, #ffff00, #00ff00, #0000ff, #4b0082, #9400d3)', animated: true },
];

/**
 * Get the ELO tier for a given rating
 */
export const getEloTier = (elo: number | undefined): EloTier => {
    if (elo === undefined) {
        return ELO_TIERS[1]; // Default to Silver (1000)
    }
    
    // Find the highest tier that the player qualifies for
    for (let i = ELO_TIERS.length - 1; i >= 0; i--) {
        if (elo >= ELO_TIERS[i].minElo) {
            return ELO_TIERS[i];
        }
    }
    
    return ELO_TIERS[0]; // Default to Bronze
};

/**
 * Get the color for a given ELO rating
 */
export const getEloColor = (elo: number | undefined): string => {
    return getEloTier(elo).color;
};

/**
 * Check if the ELO should have animated effects
 */
export const isAnimatedElo = (elo: number | undefined): boolean => {
    return getEloTier(elo).animated;
};

/**
 * Get inline styles for ELO-colored text
 */
export const getEloTextStyle = (elo: number | undefined): React.CSSProperties => {
    const tier = getEloTier(elo);
    
    const baseStyle: React.CSSProperties = {
        color: tier.color,
        fontWeight: 'bold',
    };
    
    if (tier.animated && tier.gradient) {
        return {
            ...baseStyle,
            background: tier.gradient,
            WebkitBackgroundClip: 'text',
            WebkitTextFillColor: 'transparent',
            backgroundClip: 'text',
            backgroundSize: '200% 100%',
            animation: 'rainbow-slide 3s linear infinite',
        };
    }
    
    return baseStyle;
};

/**
 * Component wrapper for displaying player names with ELO coloring
 */
interface EloPlayerNameProps {
    name: string;
    elo: number | undefined;
    style?: React.CSSProperties;
    className?: string;
}

export const EloPlayerName: React.FC<EloPlayerNameProps> = ({ name, elo, style = {}, className }) => {
    const tier = getEloTier(elo);
    const eloStyle = getEloTextStyle(elo);
    
    const combinedStyle: React.CSSProperties = {
        ...eloStyle,
        ...style,
    };
    
    if (tier.animated) {
        return (
            <>
                <style>{`
                    @keyframes rainbow-slide {
                        0% { background-position: 0% 50%; }
                        100% { background-position: 200% 50%; }
                    }
                    @keyframes glow-pulse {
                        0%, 100% { filter: drop-shadow(0 0 2px ${tier.color}) drop-shadow(0 0 5px ${tier.color}); }
                        50% { filter: drop-shadow(0 0 5px ${tier.color}) drop-shadow(0 0 10px ${tier.color}); }
                    }
                    .elo-animated {
                        animation: glow-pulse 2s ease-in-out infinite;
                    }
                `}</style>
                <span 
                    className={`elo-animated ${className || ''}`}
                    style={combinedStyle}
                    title={`${tier.name} - ${elo} ELO`}
                >
                    {name}
                </span>
            </>
        );
    }
    
    return (
        <span 
            className={className}
            style={combinedStyle}
            title={`${tier.name} - ${elo || 1000} ELO`}
        >
            {name}
        </span>
    );
};
