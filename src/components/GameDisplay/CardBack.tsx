import { useEffect, useState } from "react";
import { generateCardBackPattern } from "../../utils/cards";

// Deterministic random function using a simple LCG (Linear Congruential Generator)
const seededRandom = (seed: number): number => {
    const x = Math.sin(seed) * 10000;
    return x - Math.floor(x);
};

// Generate random rotation between -40 and 40 degrees
const getRandomRotation = (seed: number, index: number): number => {
    const randomValue = seededRandom(seed + index * 1000);
    return (randomValue * 50) - 25; // Maps 0-1 to -25 to 25
};

export const CardBack = ({ deckSize = 36, enableRandomRotation = false }: { deckSize?: number; enableRandomRotation?: boolean }) => {
    const [patternDataUrl, setPatternDataUrl] = useState<string>('');
    const seed = 12345; // Fixed seed for deterministic results

    useEffect(() => {
        generateCardBackPattern(40, 70).then(dataUrl => {
            setPatternDataUrl(dataUrl);
        });
    }, []);

    return <div style={{ position: 'relative', width: '50px', height: '70px' }}> {/* Updated to 5:7 ratio */}
        {/* Multiple card layers to show deck thickness */}
        {Array.from({ length: deckSize }).map((_, layerIndex) => {
            const rotation = enableRandomRotation ? getRandomRotation(seed, layerIndex) : 0;
            return (
                <div
                    key={`deck-layer-${layerIndex}`}
                    style={{
                        position: 'absolute',
                        top: enableRandomRotation ? '0' : `${-layerIndex * 2}px`,
                        left: enableRandomRotation ? '0' : `${-layerIndex * 1}px`,
                        width: '50px', // Updated to 5:7 ratio
                        height: '70px',
                        backgroundColor: '#DC143C', // Fallback crimson red
                        border: '1px solid #8B0000', // Dark red border
                        borderRadius: '5px',
                        zIndex: layerIndex,
                        transform: `rotate(90deg) rotate(${rotation}deg)`,
                        transformOrigin: 'center center',
                        backgroundImage: patternDataUrl ? `url(${patternDataUrl})` : undefined,
                        backgroundSize: '100% 100%',
                        backgroundRepeat: 'no-repeat'
                    }}
                />
            );
        })}

    </div>

}