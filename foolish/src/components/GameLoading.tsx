'use client';

import React from 'react';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { useLobbyTitle } from '../state/lobbyTitles';

/**
 * What a game shows while it is being created or is still loading: the lobby's
 * name, in the lobby's own title slot, and nothing else.
 *
 * The name is the one thing known before the game's view arrives (see
 * src/state/lobbyTitles.ts for where it is known from), so it is the one thing
 * drawn; everything under it would be a guess. The title is the same element
 * with the same class the real Lobby draws, so when the lobby lands the name
 * does not move. With no name held - a page opened straight on a game's URL, or
 * `create` not answered yet - this is the background alone.
 *
 * Used in two places:
 *   - CreateGameButton renders it as a full-screen overlay the moment "create"
 *     is pressed, so the click feels instant and the button can't be mashed
 *     into spawning many games. The server names the game, so the overlay is
 *     bare until `create` answers.
 *   - GameView renders it while `game` is still loading after navigation.
 */
export const GameLoading: React.FC<{ gameId: string | null }> = ({ gameId }) => {
  const title = useLobbyTitle(gameId);
  return (
    <div className="lobby" aria-busy="true">
      <WoolBackgroundLayer />
      {title !== null && (
        <input className="lobby__name-input" type="text" value={title} readOnly tabIndex={-1} style={{ pointerEvents: 'none' }} />
      )}
    </div>
  );
};
