import { useEffect, useCallback } from 'react';
import { Card } from '@shared/types.ts';
import { useServer } from '../contexts/ServerContext';
import { useAnimation } from '../contexts/AnimationContext';
import { useGame } from '../contexts/GameContext';
import { useAuth } from '../contexts/AuthContext';
import { canCover } from '@shared/common_utils.ts';
import { findUnambiguousCover } from '../utils/coverCombinations';

export const KeyboardInputHandler = () => {
    const { user_id } = useAuth();
    const { game, localHandOrder } = useServer();
    const { attack, pass, cover, pickup, good } = useAnimation();
    const { selectedCards, setSelectedCards, handleCardSelection } = useGame();

    // Check if chat is focused (to prevent keyboard shortcuts during chat input)
    const isChatFocused = () => {
        const activeElement = document.activeElement;
        if (!activeElement) return false;
        
        // Check if active element is an input, textarea, or contenteditable
        return (
            activeElement.tagName === 'INPUT' ||
            activeElement.tagName === 'TEXTAREA' ||
            activeElement.getAttribute('contenteditable') === 'true' ||
            activeElement.closest('[data-chat-scrollable]') !== null ||
            activeElement.closest('[data-touch-interactive]') !== null
        );
    };

    // Get card by position (1-based indexing for user, 0-based for array)
    const getCardByPosition = (position: number): Card | null => {
        if (!localHandOrder || position < 1 || position > localHandOrder.length) {
            return null;
        }
        return localHandOrder[position - 1];
    };

    // Cover-combination resolution lives in ../utils/coverCombinations (shared
    // with DragContext).

    // Check if passing is possible
    // eslint-disable-next-line react-hooks/exhaustive-deps
    const canPass = (cardsToCheck: Card[]): boolean => {
        if (!game) return false;
        
        const table_battles = game.table_battles;
        if (table_battles.length === 0) return false;

        // All cards must have the same value
        if (!cardsToCheck.every(card => card.value === cardsToCheck[0].value)) {
            return false;
        }

        // All table battles must be uncovered (defense === null)
        // All uncovered attacks must have the same value as the cards to check
        return table_battles.every(battle =>
            battle.defense === null && battle.attack.value === cardsToCheck[0].value
        );
    };

    // Action handlers
    const handleAttack = useCallback(async () => {
        if (!game || selectedCards.length === 0) return;

        try {
            await attack(selectedCards);
            setSelectedCards([]); // Clear selection after successful action
        } catch (error) {
            console.error('Attack failed:', error);
        }
    }, [game, selectedCards, attack, setSelectedCards]);

    const handleCover = useCallback(async () => {
        if (!game || selectedCards.length === 0) return;

        try {
            if (selectedCards.length === 1) {
                // Single card cover - find which attack it can cover
                const uncoveredBattles = game.table_battles.filter(battle => !battle.defense);
                const validTargets = uncoveredBattles.filter(battle => 
                    canCover(battle.attack, selectedCards[0], game.power_suit)
                );
                
                if (validTargets.length === 1) {
                    // Card can only cover one specific attack - allow cover action
                    await cover([selectedCards[0]], [validTargets[0].attack]);
                    setSelectedCards([]); // Clear selection after successful action
                } else {
                    console.error('Cover is ambiguous or invalid');
                }
            } else {
                // Multi-card cover - check if unambiguous
                const unambiguousCover = game
                    ? findUnambiguousCover(selectedCards, game.table_battles, game.power_suit)
                    : null;
                if (unambiguousCover) {
                    await cover(unambiguousCover.coverCards, unambiguousCover.attackCards);
                    setSelectedCards([]); // Clear selection after successful action
                } else {
                    console.error('Multi-card cover is ambiguous or invalid');
                }
            }
        } catch (error) {
            console.error('Cover failed:', error);
        }
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [game, selectedCards, cover, setSelectedCards]);

    const handlePass = useCallback(async () => {
        if (!game || selectedCards.length === 0) return;

        try {
            if (canPass(selectedCards)) {
                await pass(selectedCards);
                setSelectedCards([]); // Clear selection after successful action
            } else {
                console.error('Pass is not valid');
            }
        } catch (error) {
            console.error('Pass failed:', error);
        }
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [game, selectedCards, pass, setSelectedCards, canPass]);

    const handlePickup = useCallback(async () => {
        if (!game) return;

        try {
            await pickup();
            setSelectedCards([]); // Clear selection after successful action
        } catch (error) {
            console.error('Pickup failed:', error);
        }
    }, [game, pickup, setSelectedCards]);

    const handleGood = useCallback(async () => {
        if (!game) return;

        try {
            await good();
            setSelectedCards([]); // Clear selection after successful action
        } catch (error) {
            console.error('Good failed:', error);
        }
    }, [game, good, setSelectedCards]);

    // Handle keyboard events
    useEffect(() => {
        if (!game) {
            return;
        }

        const handleKeyDown = (event: KeyboardEvent) => {
            // Don't process shortcuts if chat is focused
            if (isChatFocused()) {
                return;
            }

            // Don't process if modifiers are pressed (allow normal browser shortcuts)
            if (event.ctrlKey || event.metaKey || event.altKey) {
                return;
            }

            const key = event.key.toLowerCase();
            
            // Number keys for card selection (1-9, 0, -, =)
            const numberKeys: { [key: string]: number } = {
                '1': 1, '2': 2, '3': 3, '4': 4, '5': 5,
                '6': 6, '7': 7, '8': 8, '9': 9, '0': 10,
                '-': 11, '=': 12
            };

            if (numberKeys[key]) {
                event.preventDefault();
                const position = numberKeys[key];
                const card = getCardByPosition(position);
                
                if (card) {
                    handleCardSelection(card);
                }
                return;
            }

            // Action keys
            const self_index = game.players.findIndex((player) => player.player_id === user_id);
            const isDefending = game.defender === self_index;
            
            // Space or A for attack, Space or C for cover (mutually exclusive)
            if ((key === ' ' || key === 'a') && !isDefending) {
                event.preventDefault();
                handleAttack();
                return;
            }

            if ((key === ' ' || key === 'c') && isDefending) {
                event.preventDefault();
                handleCover();
                return;
            }

            // G for good
            if (key === 'g') {
                event.preventDefault();
                handleGood();
                return;
            }

            // P for pass
            if (key === 'p') {
                event.preventDefault();
                handlePass();
                return;
            }

            // U for pickup
            if (key === 'u') {
                event.preventDefault();
                handlePickup();
                return;
            }
        };

        document.addEventListener('keydown', handleKeyDown);
        
        return () => {
            document.removeEventListener('keydown', handleKeyDown);
        };
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [game, selectedCards, localHandOrder, user_id, handleCardSelection, handleAttack, handleCover, handlePass, handlePickup, handleGood]);

    // This component doesn't render anything - it's just for handling keyboard events
    return null;
}; 