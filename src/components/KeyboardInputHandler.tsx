import { useEffect, useCallback } from 'react';
import { Card } from '@api/core/types.ts';
import { useServer } from '../contexts/ServerContext';
import { useAnimation } from '../contexts/AnimationContext';
import { useGame } from '../contexts/GameContext';
import { useAuth } from '../contexts/AuthContext';
import { playBoardFor } from '../wasm/playMenu';
import { playCoverableBattles } from '@sdk/ts/wasm/bots.ts';
import { canPass } from '../utils/gameValidation';
import { kernelUnambiguousCover } from '@sdk/ts/wasm/bots.ts';

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

    // Cover-combination resolution lives in the kernel now
    // (kernelUnambiguousCover -> legal.c unambiguous_cover), shared by every
    // input path and every host (A7/F9).
    //
    // Pass legality uses the SHARED canPass (src/utils/gameValidation.ts) — the
    // same predicate the buttons/drag use — so the keyboard path can't diverge.
    // The previous local copy omitted the next-player capacity check AND the
    // eliminated-seat skip, so it offered passes the server would reject.

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
                // Single card cover - which attack it can cover is the kernel's
                // answer (legal.h play_coverable_battles), off this seat's menu.
                const pb = playBoardFor(game);
                const validTargets = pb ? playCoverableBattles(pb, [selectedCards[0]]) : [];

                if (validTargets.length === 1) {
                    // Card can only cover one specific attack - allow cover action
                    await cover([selectedCards[0]], [game.table_battles[validTargets[0]].attack]);
                    setSelectedCards([]); // Clear selection after successful action
                } else {
                    console.error('Cover is ambiguous or invalid');
                }
            } else {
                // Multi-card cover - check if unambiguous
                const unambiguousCover = game
                    ? kernelUnambiguousCover(selectedCards, game.table_battles, game.power_suit)
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
            if (canPass(game, selectedCards)) {
                await pass(selectedCards);
                setSelectedCards([]); // Clear selection after successful action
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