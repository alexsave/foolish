import React, { createContext, useContext, useEffect, useState } from 'react';
import { Card, PersonalGame } from '../common/types';
import { useServer } from './ServerContext';

const GameContext = createContext<GameContextType | null>(null);

// Things related to game state, mostly taken from GameDisplay. 
// Sometimes the game will directly use ServerContext though
export const GameProvider = ({ children }: { children: React.ReactNode }) => {
    const { game } = useServer() as { game: PersonalGame, rearrangeHand: (game_id: string, indices: number[]) => Promise<{ game_id: string }> };

    const [coverMap, setCoverMap] = useState<Map<Card, Card>>(new Map());

    const [selectedCards, setSelectedCards] = useState<Card[]>([]);
    // we chose a card to cover WITH, now we choose WHICH card to cover
    const [isSelectingCover, setIsSelectingCover] = useState(false);

    // Game loading state is now handled by ServerContext

    const handleCardSelection = (card: Card) => {
        const isSelected = selectedCards.some(selectedCard =>
            selectedCard.value === card.value && selectedCard.suit === card.suit
        );

        if (isSelected) {
            setSelectedCards(selectedCards.filter(c => !(c.value === card.value && c.suit === card.suit)));
        } else {
            setSelectedCards([...selectedCards, card]);
        }
    };

    return (
        <GameContext.Provider value={{
            selectedCards,
            setSelectedCards,
            coverMap,
            isSelectingCover,
            setCoverMap,
            setIsSelectingCover,
            handleCardSelection,
        }}>
            {children}
        </GameContext.Provider>
    );
};

interface GameContextType {
    selectedCards: Card[];
    setSelectedCards: (selectedCards: Card[]) => void;
    coverMap: Map<Card, Card>;
    isSelectingCover: boolean;
    setCoverMap: (coverMap: Map<Card, Card>) => void;
    setIsSelectingCover: (isSelectingCover: boolean) => void;
    handleCardSelection: (card: Card) => void;
}


export const useGame = () => {
    const context = useContext(GameContext);
    if (!context) {
        throw new Error('useAuth must be used within a AuthProvider');
    }
    return context;
}; 