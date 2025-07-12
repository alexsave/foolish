import { PersonalGame } from "../../common/types";
import { useGame } from "../../contexts/GameContext";
import { useServer } from "../../contexts/ServerContext";

export const CoverArrows = () => {
    const game: PersonalGame = useServer().game as PersonalGame;

    const { coverMap } = useGame();

    return <svg style={{ position: 'absolute', top: 0, left: 0, width: '100%', height: '100%', pointerEvents: 'none', zIndex: 500 }}>
        {Array.from(coverMap.entries()).map(([coveringCard, coveredCard], index) => {
            // Skip if spectator (no self)
            if (!game.self) return null;

            // Find the position of the covering card (in hand)
            const handCardIndex = game.self.hand.findIndex(card =>
                card.value === coveringCard.value && card.suit === coveringCard.suit
            );

            // Find the position of the covered card (on table)
            const tableCardIndex = game.table_battles.findIndex(battle =>
                battle.attack.value === coveredCard.value && battle.attack.suit === coveredCard.suit
            );

            if (handCardIndex === -1 || tableCardIndex === -1) return null;

            // Calculate approximate positions
            // Hand cards are at the bottom center
            const handCardsStartX = window.innerWidth / 2 - (game.self.hand.length * 50) / 2; // Updated for 5:7 ratio cards
            const handX = handCardsStartX + (handCardIndex * 50) + 25; // 25 is half card width (50px)
            const handY = window.innerHeight - 100; // approximate bottom position

            // Table cards are in the center
            const tableX = window.innerWidth / 2;
            const tableY = window.innerHeight / 2 + (tableCardIndex * 80) - (game.table_battles.length * 40);

            return <g key={`arrow-${index}`}>
                {/* Arrow line */}
                <line
                    x1={handX}
                    y1={handY}
                    x2={tableX}
                    y2={tableY}
                    stroke="yellow"
                    strokeWidth="3"
                    markerEnd="url(#arrowhead)"
                />
            </g>;
        })}

        {/* Arrow marker definition */}
        <defs>
            <marker
                id="arrowhead"
                markerWidth="10"
                markerHeight="7"
                refX="9"
                refY="3.5"
                orient="auto"
            >
                <polygon
                    points="0 0, 10 3.5, 0 7"
                    fill="yellow"
                />
            </marker>
        </defs>
    </svg>
};