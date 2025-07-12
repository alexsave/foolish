import { useEffect, useState } from "react";
import { generateCardBackPattern } from "../../utils/cards";

export const CardBack = ({ deckSize = 36 }: { deckSize?: number }) => {
    const [patternDataUrl, setPatternDataUrl] = useState<string>('');

    useEffect(() => {
        generateCardBackPattern(40, 70).then(dataUrl => {
            setPatternDataUrl(dataUrl);
        });
    }, []);

    return <div style={{ position: 'relative', width: '50px', height: '70px' }}> {/* Updated to 5:7 ratio */}
        {/* Multiple card layers to show deck thickness */}
        {Array.from({ length: Math.min(Math.ceil(deckSize / 6), 6) }).map((_, layerIndex) => (
            <div
                key={`deck-layer-${layerIndex}`}
                style={{
                    position: 'absolute',
                    top: `${-layerIndex * 2}px`,
                    left: `${-layerIndex * 1}px`,
                    width: '50px', // Updated to 5:7 ratio
                    height: '70px',
                    backgroundColor: '#DC143C', // Fallback crimson red
                    border: '1px solid #8B0000', // Dark red border
                    borderRadius: '5px',
                    zIndex: layerIndex,
                    transform: 'rotate(90deg)',
                    backgroundImage: patternDataUrl ? `url(${patternDataUrl})` : undefined,
                    backgroundSize: '100% 100%',
                    backgroundRepeat: 'no-repeat'
                }}
            />
        ))}

        {/* Top card with more detailed pattern */}
        <div style={{
            position: 'absolute',
            top: `${-Math.min(Math.ceil(deckSize / 6), 6) * 2}px`,
            left: `${-Math.min(Math.ceil(deckSize / 6), 6) * 1}px`,
            width: '50px', // Updated to 5:7 ratio
            height: '70px',
            backgroundColor: '#DC143C', // Fallback crimson red
            border: '2px solid #8B0000',
            borderRadius: '5px',
            zIndex: 10,
            transform: 'rotate(90deg)',
            backgroundImage: patternDataUrl ? `url(${patternDataUrl})` : undefined,
            backgroundSize: '100% 100%',
            backgroundRepeat: 'no-repeat',
            boxShadow: '0 2px 4px rgba(0,0,0,0.3)'
        }} />
    </div>

}