import { useFernFractal } from "../../utils/fernFractal";

// Deterministic random function using a simple LCG (Linear Congruential Generator)
const seededRandom = (seed: number): number => {
    const x = Math.sin(seed) * 10000;
    return x - Math.floor(x);
};

// Generate random rotation between -40 and 40 degrees
const getRandomRotation = (seed: number, index: number): number => {
    const randomValue = seededRandom(seed + index * 1000);
    return (randomValue * 40) - 20; // Maps 0-1 to -20 to 20
};

export const CardBack = ({ deckSize = 36, enableRandomRotation = false }: { deckSize?: number; enableRandomRotation?: boolean }) => {
    const { fernPattern } = useFernFractal();
    const seed = 42; // Fixed seed for deterministic results
    
    // Log when CardBack renders with fern pattern
    React.useEffect(() => {
      if (fernPattern) {
        import('../../utils/errorLogger').then(({ errorLogger }) => {
          errorLogger.logSVGOperation('CardBack Render', {
            deckSize,
            enableRandomRotation,
            fernPatternSize: fernPattern.length,
            fernPatternSizeKB: (fernPattern.length / 1024).toFixed(2),
          });
        });
      }
    }, [fernPattern, deckSize, enableRandomRotation]);

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
                        backgroundColor: '#000000', // Black background
                        border: '1px solid #8B0000', // Dark red border
                        borderRadius: '5px',
                        zIndex: layerIndex + 2,
                        transform: `rotate(90deg) rotate(${rotation}deg)`,
                        transformOrigin: 'center center',
                        backgroundImage: fernPattern ? `url(${fernPattern})` : undefined,
                        backgroundSize: '100% 100%',
                        backgroundRepeat: 'no-repeat'
                    }}
                />
            );
        })}

    </div>

}