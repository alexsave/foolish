import { useFernFractal } from "../../utils/fernFractal";
import { useStyles } from "../../contexts/StyleContext";

// Deterministic random function using a simple LCG
const seededRandom = (seed: number): number => {
    const x = Math.sin(seed) * 10000;
    return x - Math.floor(x);
};

const getRandomRotation = (seed: number, index: number): number => {
    const randomValue = seededRandom(seed + index * 1000);
    return (randomValue * 40) - 20;
};

// Soviet card back - red with gold star
const SovietCardBack = () => (
    <svg width="100%" height="100%" viewBox="0 0 50 70" preserveAspectRatio="none" style={{ display: 'block' }}>
        <rect x="0" y="0" width="50" height="70" fill="#B32929" />
        <rect x="2" y="2" width="46" height="66" fill="none" stroke="#E79743" strokeWidth="3" />
        <polygon 
            points="25,18 28.5,29 40,29 31,36.5 34.5,48 25,41 15.5,48 19,36.5 10,29 21.5,29" 
            fill="none" 
            stroke="#0A0A0A" 
            strokeWidth="3"
        />
        <polygon 
            points="25,18 28.5,29 40,29 31,36.5 34.5,48 25,41 15.5,48 19,36.5 10,29 21.5,29" 
            fill="none" 
            stroke="#F5E6C8" 
            strokeWidth="1.5"
        />
        <polygon 
            points="25,18 28.5,29 40,29 31,36.5 34.5,48 25,41 15.5,48 19,36.5 10,29 21.5,29" 
            fill="#E79743" 
        />
    </svg>
);

export const CardBack = ({ deckSize = 36, enableRandomRotation = false }: { deckSize?: number; enableRandomRotation?: boolean }) => {
    const styles = useStyles();
    const { fernPattern } = useFernFractal();
    
    const seed = 42;
    const hasPattern = styles.cardBack.usePattern && !!fernPattern;
    const backgroundColor = hasPattern ? '#000000' : styles.cardBack.backgroundColor;

    return (
        <div style={{ position: 'relative', width: '50px', height: '70px' }}>
            {Array.from({ length: deckSize }).map((_, layerIndex) => {
                const rotation = enableRandomRotation ? getRandomRotation(seed, layerIndex) : 0;
                return (
                    <div
                        key={`deck-layer-${layerIndex}`}
                        style={{
                            position: 'absolute',
                            top: enableRandomRotation ? '0' : `${-layerIndex * 2}px`,
                            left: enableRandomRotation ? '0' : `${-layerIndex * 1}px`,
                            width: '50px',
                            height: '70px',
                            backgroundColor: !styles.cardBack.usePattern ? 'transparent' : backgroundColor,
                            border: `${styles.cardBack.borderWidth} solid ${styles.cardBack.borderColor}`,
                            borderRadius: styles.cardBack.borderRadius,
                            zIndex: layerIndex + 2,
                            transform: `rotate(90deg) rotate(${rotation}deg)`,
                            transformOrigin: 'center center',
                            backgroundImage: hasPattern ? `url(${fernPattern})` : undefined,
                            backgroundSize: '100% 100%',
                            backgroundRepeat: 'no-repeat',
                            boxShadow: styles.cardBack.boxShadow,
                            overflow: 'hidden',
                        }}
                    >
                        {!styles.cardBack.usePattern && <SovietCardBack />}
                    </div>
                );
            })}
        </div>
    );
};
