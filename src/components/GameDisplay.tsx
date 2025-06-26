import React from 'react';
import { Card, Game, PersonalGame, Player, OtherPlayer } from '../common/common';
import { useServer } from '../contexts/ServerContext';

const SUIT_MAP: Record<number, string> = {
  // emojis
  0: '♠️',
  1: '♥️',
  2: '♣️',
  3: '♦️',
}

const VALUE_MAP: Record<number, string> = {
  1: '2',
  2: '3',
  3: '4',
  4: '5',
  5: '6',
  6: '7',
  7: '8',
  8: '9',
  9: '10',
  10: 'J',
  11: 'Q',
  12: 'K',
  13: 'A',
}

export const GameDisplay = () => {
  const { game, player_id } = useServer();
  const state = game as PersonalGame;


  const CardDisplay = ({card}: {card: Card}) => {
    return (
      <div style={{ backgroundColor: 'white', width: '30px', height: '60px', borderRadius: '5px', border: '1px solid black', display: 'flex', justifyContent: 'center', alignItems: 'center' }}>
        <p>{VALUE_MAP[card.value] + SUIT_MAP[card.suit]}</p>
      </div>
    )
  }

  const CardBack = () => {
    return (
      <div style={{ backgroundColor: 'black', width: '30px', height: '60px', borderRadius: '5px', border: '1px solid black', display: 'flex', justifyContent: 'center', alignItems: 'center' }}>
        <p>?</p>
      </div>
    )
  }

  return (
    <div style={{ backgroundColor: '#982621', width: '100%', height: '100vh'}}>
      <div style={{  display: 'flex',  flex: 1, justifyContent: 'center', alignItems: 'center' }}>
        <p>FOOLISH</p>
        <div style={{ display: 'flex', position: 'absolute', top: '0px', left: '0px', flexDirection: 'column', justifyContent: 'center', alignItems: 'center', height: '100px' }}>
          {state.flipped && <CardDisplay card={state.flipped} />}
          <CardBack />
          <p>{JSON.stringify(state.deck_length)}</p>
        </div>
        <div style={{ display: 'flex', position: 'absolute', bottom: '0px', left: '0px', right: '0px', justifyContent: 'center', alignItems: 'center', height: '100px' }}>
          {
            state.self.hand.map((card) => {
              return <CardDisplay card={card} />
            })
          }

        </div>

        <div style={{position: 'absolute', display: 'flex', flexDirection: 'column', top: 0, width: '100%', bottom: 0, alignItems: 'center', justifyContent: 'center'}}>
          <div style={{backgroundColor: 'black', height: '10px', width: '10px'}}/>
          {
            state.players.map((player, index) => {
            // array of 100 black squares
            //Array.from({length: state.players.length-1}).map((_, index) => {
              const radians = (1.5)*Math.PI*index/(state.players.length-1) + Math.PI/4;
              const x = ((Math.sin(radians) *40) + 50) + '%';
              const y = ((Math.cos(radians) *40) + 50) + '%';

              let color = 'black';
              if (index === state.currentlyAttacked) {
                color = 'red';
              } else if (index === state.firstAttacker) {
                color = 'orange';
              }

              return <div style={{backgroundColor: color, height: '10px', width: '10px', position: 'absolute', top: y, left: x}}>
                <p>{player.name}</p>
                { player.hand_length && <p>{player.hand_length}</p> }
              </div>
            })
          }
        </div>
      </div>
    </div>
  );
};