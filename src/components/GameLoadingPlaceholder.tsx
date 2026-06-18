'use client';

import React from 'react';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';

/**
 * A lobby-shaped skeleton shown while a game is being created or is still
 * loading, so the screen looks like a real game coming to life instead of a
 * frozen/blank page. It reuses the same `lobby*` CSS classes as the real Lobby
 * so the layout matches, and is replaced by the real Lobby the instant game
 * state arrives.
 *
 * Used in two places:
 *   - CreateGameButton renders it as a full-screen overlay the moment "create"
 *     is pressed (before the server has even returned an id), so the click feels
 *     instant and the button can't be mashed into spawning many games.
 *   - GameView renders it while `game` is still loading after navigation, so the
 *     hand-off from the create overlay to the real lobby has no blank gap.
 */
export const GameLoadingPlaceholder: React.FC = () => {
  return (
    <div className="lobby" aria-busy="true" aria-label="Setting up game">
      <WoolBackgroundLayer />
      <style>{`
        @keyframes foolish-skel-pulse { 0%,100% { opacity: .35 } 50% { opacity: .7 } }
        .foolish-skel { animation: foolish-skel-pulse 1.25s ease-in-out infinite;
          background: rgba(0,0,0,.20); border-radius: 12px; }
      `}</style>

      {/* game-name title placeholder */}
      <div className="foolish-skel" style={{ width: 220, height: '2.2rem', margin: '0 auto' }} />

      {/* game-id line placeholder */}
      <h2 className="lobby__game-id">
        <span className="foolish-skel" style={{ display: 'inline-block', width: 120, height: '1.1em', verticalAlign: 'middle' }} />
      </h2>

      {/* QR placeholder, same box the real QR sits in */}
      <div className="lobby__qr-container">
        <div className="foolish-skel" style={{ width: 120, height: 120 }} />
      </div>

      {/* seat placeholders */}
      <div className="lobby__players">
        {[0, 1].map((i) => (
          <div className="lobby__player-wrapper" key={i}>
            <div className="foolish-skel" style={{ width: 150, height: 86 }} />
          </div>
        ))}
      </div>
    </div>
  );
};
