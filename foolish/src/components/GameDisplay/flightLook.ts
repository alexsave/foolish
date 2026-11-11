// THE LOOK OF A CARD IN THE AIR, which FlightCard draws and which the stock held up through the opening
// deal borrows (CardBack `lifted`), so the two are the same black edge and the same shadow.

export const REVERT = {
    border: '2px solid rgb(220, 38, 38)',
    filter: 'brightness(1.3) contrast(1.2) sepia(0.3) saturate(1.8) hue-rotate(-10deg)',
    backgroundColor: 'rgb(255, 150, 150)',
    shadow: 'rgba(255,0,0,0.6)',
} as const;

export const NORMAL = {
    border: '2px solid black',
    filter: 'none',
    backgroundColor: 'var(--color-card-face)',
    shadow: 'rgba(0,0,0,0.4)',
} as const;
