import { useEffect, useCallback } from 'react';
import { useServer } from '../contexts/ServerContext';
import { useAnimation } from '../contexts/AnimationContext';
import { useGame } from '../contexts/GameContext';
import { useAuth } from '../contexts/AuthContext';
import { canPass, coverGesture } from '../utils/gameValidation';
import { type ViewCard as Card } from '../state/view';

export const KeyboardInputHandler = () => {
    const { user_id } = useAuth();
    const { view: game, localHandOrder } = useServer();
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

    // What the Cover key means lives in the kernel now (gameValidation
    // coverGesture -> client_play, legal.h play_*), shared by every input path
    // and every host.
    //
    // Pass legality uses the SHARED canPass (src/utils/gameValidation.ts) - the
    // same predicate the buttons/drag use - so the keyboard path can't diverge.
    // The previous local copy omitted the next-player capacity check AND the
    // eliminated-seat skip, so it offered passes the server would reject.

    // Action handlers
    const handleAttack = useCallback(async () => {
        if (!game || selectedCards.length === 0) return;

        // A move spends the selection when it is sent, refused or not (see ActionButtons).
        try {
            setSelectedCards([]);
            await attack(selectedCards);
        } catch (error) {
            console.error('Attack failed:', error);
        }
    }, [game, selectedCards, attack, setSelectedCards]);

    const handleCover = useCallback(async () => {
        if (!game || selectedCards.length === 0) return;
        // The same one answer the Cover BUTTON uses: the kernel aims the cover
        // and names the move, or there is no cover to make.
        const move = coverGesture(game, selectedCards);
        if (!move) {
            console.error('Cover is ambiguous or invalid');
            return;
        }
        try {
            setSelectedCards([]);
            await cover([...move.cards], [...move.attackCards]);
        } catch (error) {
            console.error('Cover failed:', error);
        }
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [game, selectedCards, cover, setSelectedCards]);

    const handlePass = useCallback(async () => {
        if (!game || selectedCards.length === 0) return;

        try {
            if (canPass(game, selectedCards)) {
                setSelectedCards([]);
                await pass(selectedCards);
            } else {
                console.error('Pass is not valid');
            }
        } catch (error) {
            console.error('Pass failed:', error);
        }
    }, [game, selectedCards, pass, setSelectedCards]);

    const handlePickup = useCallback(async () => {
        if (!game) return;

        try {
            setSelectedCards([]);
            await pickup();
        } catch (error) {
            console.error('Pickup failed:', error);
        }
    }, [game, pickup, setSelectedCards]);

    const handleGood = useCallback(async () => {
        if (!game) return;

        try {
            setSelectedCards([]);
            await good();
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
            const isDefending = game.defender === game.mySeat;
            
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